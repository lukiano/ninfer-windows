#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace ninfer::app {

// Process host only: model loading and HTTP execution remain in the ordinary server route.
class WindowsService {
public:
    WindowsService();
    ~WindowsService();
    WindowsService(const WindowsService&) = delete;
    WindowsService& operator=(const WindowsService&) = delete;

    [[nodiscard]] bool stop_requested() const noexcept;
    void ready();

    // The registration must be destroyed before its captured server. Unregistration waits for
    // any in-flight stop callback; SCM handlers themselves never call into the HTTP server.
    class StopRegistration {
    public:
        StopRegistration(WindowsService& host, std::function<void()> stop);
        ~StopRegistration();
        StopRegistration(const StopRegistration&) = delete;
        StopRegistration& operator=(const StopRegistration&) = delete;
    private:
        WindowsService& host_;
    };

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend int run_windows_service(const std::string&, const std::filesystem::path&,
                                    std::function<int(WindowsService&)>);
};

int run_windows_service(const std::string& name, const std::filesystem::path& log_path,
                        std::function<int(WindowsService&)> run);

} // namespace ninfer::app
