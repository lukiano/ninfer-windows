#include "windows_service.h"

#include <windows.h>
#include <io.h>
#include <share.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ninfer::app {
namespace {

void report_error(const wchar_t* message) noexcept {
    char text[1024]{};
    WideCharToMultiByte(CP_UTF8, 0, message, -1, text, sizeof(text), nullptr, nullptr);
    std::fprintf(stderr, "ninfer-serve service: %s\n", text);
    // Also preserve failures opening the log or registering with SCM, before stderr is usable.
    HANDLE source = RegisterEventSourceW(nullptr, L"ninfer-serve");
    if (source) {
        ReportEventW(source, EVENTLOG_ERROR_TYPE, 0, 1, nullptr, 1, 0, &message, nullptr);
        DeregisterEventSource(source);
    }
}

std::wstring wide_name(const std::string& name) {
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                                         static_cast<int>(name.size()), nullptr, 0);
    if (!size) { throw std::invalid_argument("invalid UTF-8 service name"); }
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                        static_cast<int>(name.size()), result.data(), size);
    return result;
}

} // namespace

struct WindowsService::Impl {
    // Windows' service entry point has no context parameter. There is exactly one own-process
    // service in this executable, and this pointer remains valid until the dispatcher returns.
    static Impl* active;
    WindowsService& host;
    std::wstring name;
    std::filesystem::path log_path;
    std::function<int(WindowsService&)> run;
    SERVICE_STATUS_HANDLE status_handle = nullptr;
    SERVICE_STATUS status{SERVICE_WIN32_OWN_PROCESS, SERVICE_START_PENDING, 0, NO_ERROR, 0, 1, 10000};
    std::mutex status_mutex;
    std::condition_variable changed;
    std::atomic<bool> stopping{false};
    bool finished = false;
    int exit_code = 1;
    std::mutex stop_mutex;
    std::function<void()> stop;

    explicit Impl(WindowsService& owner) : host(owner) {}

    void publish_locked() {
        if (!SetServiceStatus(status_handle, &status)) {
            report_error(L"SetServiceStatus failed");
        }
    }

    static DWORD WINAPI control(DWORD code, DWORD, void*, void* context) noexcept {
        auto& self = *static_cast<Impl*>(context);
        std::lock_guard lock(self.status_mutex);
        if (code == SERVICE_CONTROL_INTERROGATE) {
            // Never publish again after SERVICE_STOPPED (SCM has closed the status handle).
            if (self.status.dwCurrentState != SERVICE_STOPPED) { self.publish_locked(); }
            return NO_ERROR;
        }
        if (code != SERVICE_CONTROL_STOP && code != SERVICE_CONTROL_SHUTDOWN) {
            return ERROR_CALL_NOT_IMPLEMENTED;
        }
        if (self.status.dwCurrentState == SERVICE_STOPPED) { return NO_ERROR; }
        self.stopping.store(true);
        self.status.dwCurrentState = SERVICE_STOP_PENDING;
        self.status.dwControlsAccepted = 0;
        self.status.dwCheckPoint = 1;
        self.status.dwWaitHint = 10000;
        self.publish_locked();
        self.changed.notify_all();
        return NO_ERROR;
    }

    static void WINAPI service_main(DWORD, wchar_t**) noexcept {
        auto& self = *active;
        self.status_handle = RegisterServiceCtrlHandlerExW(self.name.c_str(), control, &self);
        if (!self.status_handle) {
            report_error(L"RegisterServiceCtrlHandlerEx failed");
            return;
        }
        {
            std::lock_guard lock(self.status_mutex);
            self.publish_locked();
        }
        try {
            FILE* stream = nullptr;
            // freopen denies sharing on MSVC. Open once with explicit sharing so operators can
            // read/tail the live log, then initialize and duplicate the service's CRT descriptors.
            std::unique_ptr<FILE, decltype(&std::fclose)> log(
                _wfsopen(self.log_path.c_str(), L"a", _SH_DENYNO), &std::fclose);
            if (!log || _wfreopen_s(&stream, L"NUL", L"w", stderr) != 0 ||
                _wfreopen_s(&stream, L"NUL", L"w", stdout) != 0 ||
                _dup2(_fileno(log.get()), _fileno(stderr)) != 0 ||
                _dup2(_fileno(stderr), _fileno(stdout)) != 0) {
                throw std::runtime_error("cannot open --service-log; check directory and account permissions");
            }
            log.reset();
            HANDLE error_handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr)));
            HANDLE output_handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
            LARGE_INTEGER end{};
            // spdlog's Windows sink uses GetStdHandle/WriteFile rather than CRT FILE streams.
            // These duplicated handles share a file position; start it at EOF for restart append.
            if (!SetFilePointerEx(error_handle, end, nullptr, FILE_END) ||
                !SetStdHandle(STD_ERROR_HANDLE, error_handle) ||
                !SetStdHandle(STD_OUTPUT_HANDLE, output_handle)) {
                throw std::runtime_error("cannot redirect Windows service log handles");
            }
            std::setvbuf(stderr, nullptr, _IONBF, 0);
            std::setvbuf(stdout, nullptr, _IONBF, 0);

            std::jthread worker([&self] {
                int result = 1;
                try {
                    result = self.run(self.host);
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "ninfer-serve service: %s\n", error.what());
                } catch (...) {
                    report_error(L"unhandled server exception");
                }
                std::lock_guard lock(self.status_mutex);
                self.exit_code = result;
                self.finished = true;
                self.changed.notify_all();
            });

            std::unique_lock lock(self.status_mutex);
            bool stop_dispatched = false;
            while (!self.finished) {
                if (self.stopping.load() && !stop_dispatched) {
                    stop_dispatched = true;
                    lock.unlock();
                    {
                        std::lock_guard stop_lock(self.stop_mutex);
                        if (self.stop) { self.stop(); }
                    }
                    lock.lock();
                    continue;
                }
                self.changed.wait_for(lock, std::chrono::seconds(2));
                if (!self.finished && (self.status.dwCurrentState == SERVICE_START_PENDING ||
                                       self.status.dwCurrentState == SERVICE_STOP_PENDING)) {
                    ++self.status.dwCheckPoint;
                    self.publish_locked();
                }
            }
            lock.unlock();
            worker.join();
            if (self.exit_code == 0 && !self.stopping.load()) {
                report_error(L"server exited without a service stop request");
                self.exit_code = 1;
            }
        } catch (const std::exception& error) {
            std::fprintf(stderr, "ninfer-serve service: %s\n", error.what());
            report_error(L"service startup failed; check --service-log and account permissions");
            self.exit_code = 1;
        } catch (...) {
            report_error(L"service startup failed");
            self.exit_code = 1;
        }
        // All worker/model/socket resources are gone before the one final status publication.
        std::lock_guard lock(self.status_mutex);
        self.status.dwCurrentState = SERVICE_STOPPED;
        self.status.dwControlsAccepted = 0;
        self.status.dwCheckPoint = 0;
        self.status.dwWaitHint = 0;
        self.status.dwWin32ExitCode = self.exit_code ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
        self.status.dwServiceSpecificExitCode = static_cast<DWORD>(self.exit_code);
        self.publish_locked();
    }
};

WindowsService::Impl* WindowsService::Impl::active = nullptr;

WindowsService::WindowsService() : impl_(std::make_unique<Impl>(*this)) {}
WindowsService::~WindowsService() = default;

bool WindowsService::stop_requested() const noexcept { return impl_->stopping.load(); }

void WindowsService::ready() {
    std::lock_guard lock(impl_->status_mutex);
    if (impl_->stopping.load()) { return; }
    impl_->status.dwCurrentState = SERVICE_RUNNING;
    impl_->status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    impl_->status.dwCheckPoint = 0;
    impl_->status.dwWaitHint = 0;
    impl_->publish_locked();
    impl_->changed.notify_all();
}

WindowsService::StopRegistration::StopRegistration(WindowsService& host, std::function<void()> stop)
    : host_(host) {
    std::lock_guard lock(host_.impl_->stop_mutex);
    host_.impl_->stop = std::move(stop);
    if (host_.stop_requested()) { host_.impl_->stop(); }
}

WindowsService::StopRegistration::~StopRegistration() {
    std::lock_guard lock(host_.impl_->stop_mutex);
    host_.impl_->stop = {};
}

int run_windows_service(const std::string& name, const std::filesystem::path& log_path,
                        std::function<int(WindowsService&)> run) {
    WindowsService host;
    host.impl_->name = wide_name(name);
    host.impl_->log_path = log_path;
    host.impl_->run = std::move(run);
    WindowsService::Impl::active = host.impl_.get();
    SERVICE_TABLE_ENTRYW table[] = {
        {host.impl_->name.data(), WindowsService::Impl::service_main}, {nullptr, nullptr}};
    const bool dispatched = StartServiceCtrlDispatcherW(table) != 0;
    const DWORD error = dispatched ? NO_ERROR : GetLastError();
    WindowsService::Impl::active = nullptr;
    if (!dispatched) {
        std::fprintf(stderr, "ninfer-serve: service dispatcher failed (Windows error %lu). "
                             "--service must be launched by the Windows service manager.\n", error);
        return 1;
    }
    return host.impl_->exit_code;
}

} // namespace ninfer::app
