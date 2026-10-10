#include "../apps/serve/windows_service.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(std::string(message) + " (Windows error " +
                                 std::to_string(GetLastError()) + ")");
    }
}

struct ServiceHandle {
    SC_HANDLE value;
    ~ServiceHandle() { if (value) { CloseServiceHandle(value); } }
};

SERVICE_STATUS_PROCESS query(SC_HANDLE service) {
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    require(QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                 reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes) != 0,
            "QueryServiceStatusEx failed");
    return status;
}

SERVICE_STATUS_PROCESS wait_state(SC_HANDLE service, DWORD state, bool* progressed = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    DWORD checkpoint = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = query(service);
        if (progressed && status.dwCheckPoint > 1 && status.dwCheckPoint > checkpoint) {
            *progressed = true;
        }
        checkpoint = status.dwCheckPoint;
        if (status.dwCurrentState == state) { return status; }
        require(state == SERVICE_STOPPED || status.dwCurrentState != SERVICE_STOPPED,
                "service stopped before reaching the expected state");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("timed out waiting for service state");
}

// Cleanup also runs after failed assertions: this test never leaves a resident test service.
struct InstalledService {
    SC_HANDLE handle;
    ~InstalledService() {
        SERVICE_STATUS ignored{};
        ControlService(handle, SERVICE_CONTROL_STOP, &ignored);
        try { (void)wait_state(handle, SERVICE_STOPPED); } catch (...) {}
        DeleteService(handle);
    }
};

int fixture(const std::string& name, const std::filesystem::path& log, const std::string& mode) {
    return ninfer::app::run_windows_service(name, log, [&](ninfer::app::WindowsService& host) {
        const char native_message[] = "fixture native log\n";
        DWORD written = 0;
        require(WriteFile(GetStdHandle(STD_ERROR_HANDLE), native_message, sizeof(native_message) - 1,
                           &written, nullptr) != 0 && written == sizeof(native_message) - 1,
                "native Windows log handle is not redirected");
        std::fprintf(stderr, "fixture starting\n");
        if (mode == "failure") { return 42; }
        std::mutex mutex;
        std::condition_variable changed;
        bool stop = false;
        ninfer::app::WindowsService::StopRegistration registration(host, [&] {
            std::lock_guard lock(mutex);
            stop = true;
            changed.notify_all();
        });
        // Exercise pending checkpoints independently of GPU/model prerequisites.
        std::this_thread::sleep_for(std::chrono::seconds(5));
        host.ready();
        std::fprintf(stdout, "fixture running\n");
        if (mode == "unexpected") { return 0; }
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return stop; });
        lock.unlock();
        require(host.stop_requested(), "stop callback did not publish stop intent");
        std::this_thread::sleep_for(std::chrono::seconds(4));
        std::fprintf(stderr, "fixture stopped\n");
        return 0;
    });
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--fixture") {
        return fixture(argv[2], std::filesystem::path(argv[4]), argv[3]);
    }
    ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE)};
    if (!manager.value && GetLastError() == ERROR_ACCESS_DENIED) {
        std::cout << "Windows service integration test requires an administrator shell\n";
        return 77;
    }
    try {
        require(manager.value != nullptr, "OpenSCManager failed");
        wchar_t executable[32768]{};
        require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "GetModuleFileName failed");
        const std::wstring name = L"NInferNativeServiceTest-" + std::to_wstring(GetCurrentProcessId());
        const auto log = std::filesystem::temp_directory_path() / (name + L".log");
        const auto command = [&](const wchar_t* mode, const std::filesystem::path& path) {
            return L"\"" + std::wstring(executable) + L"\" --fixture " + name +
                   L" " + mode + L" \"" + path.wstring() + L"\"";
        };
        const auto binary = command(L"normal", log);
        ServiceHandle service{CreateServiceW(manager.value, name.c_str(), name.c_str(), SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
            binary.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr)};
        require(service.value != nullptr, "CreateService failed");
        InstalledService cleanup{service.value};
        for (int run = 0; run < 2; ++run) {
            require(StartServiceW(service.value, 0, nullptr) != 0, "StartService failed");
            bool startup_progress = false;
            const auto running = wait_state(service.value, SERVICE_RUNNING, &startup_progress);
            require(startup_progress, "startup did not advance pending checkpoints");
            require((running.dwControlsAccepted & (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN)) ==
                    (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN), "stop/shutdown controls not accepted");
            HANDLE reader = CreateFileW(log.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(reader != INVALID_HANDLE_VALUE, "service log cannot be read while running");
            CloseHandle(reader);
            SERVICE_STATUS status{};
            const auto before_stop = std::chrono::steady_clock::now();
            require(ControlService(service.value, SERVICE_CONTROL_STOP, &status) != 0,
                    "ControlService stop failed");
            require(std::chrono::steady_clock::now() - before_stop < std::chrono::seconds(2),
                    "SCM control handler blocked on server shutdown");
            bool shutdown_progress = false;
            const auto stopped = wait_state(service.value, SERVICE_STOPPED, &shutdown_progress);
            require(shutdown_progress, "shutdown did not advance pending checkpoints");
            require(stopped.dwWin32ExitCode == NO_ERROR && stopped.dwServiceSpecificExitCode == 0,
                    "normal service stop reported failure");
        }
        std::ifstream input(log);
        const std::string output((std::istreambuf_iterator<char>(input)), {});
        input.close(); // Release Windows' read sharing lock before starting another log writer.
        require(output.find("fixture starting") != std::string::npos &&
                output.find("fixture running") != std::string::npos &&
                output.find("fixture stopped") != std::string::npos,
                "stdout/stderr were not captured in the service log");
        require(output.find("fixture starting", output.find("fixture starting") + 1) != std::string::npos,
                "service restart did not append its log");
        require(output.find("fixture native log", output.find("fixture native log") + 1) != std::string::npos,
                "native Windows logging did not append across service restarts");
        for (const auto* mode : {L"failure", L"unexpected", L"missing-log-directory"}) {
            const bool missing_log = std::wstring(mode) == L"missing-log-directory";
            const auto failure_binary = command(mode, missing_log ? log.parent_path() / name / L"missing.log" : log);
            require(ChangeServiceConfigW(service.value, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE,
                SERVICE_NO_CHANGE, failure_binary.c_str(), nullptr, nullptr, nullptr, nullptr,
                nullptr, nullptr) != 0, "ChangeServiceConfig failed");
            require(StartServiceW(service.value, 0, nullptr) != 0, "failure fixture did not start");
            const auto stopped = wait_state(service.value, SERVICE_STOPPED);
            if (stopped.dwWin32ExitCode != ERROR_SERVICE_SPECIFIC_ERROR ||
                stopped.dwServiceSpecificExitCode != (std::wstring(mode) == L"failure" ? 42 : 1)) {
                std::cerr << "failure fixture " << (std::wstring(mode) == L"failure" ? "failure" : "unexpected")
                          << " returned Win32=" << stopped.dwWin32ExitCode
                          << " specific=" << stopped.dwServiceSpecificExitCode << '\n';
            }
            require(stopped.dwWin32ExitCode == ERROR_SERVICE_SPECIFIC_ERROR &&
                    stopped.dwServiceSpecificExitCode == (std::wstring(mode) == L"failure" ? 42 : 1),
                    "service failure exit code was not preserved");
        }
        std::filesystem::remove(log);
        std::cout << "ok: SCM startup, pending checkpoints, stop, restart, logs and failure status\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
