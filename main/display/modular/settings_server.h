#ifndef MODULAR_SETTINGS_SERVER_H
#define MODULAR_SETTINGS_SERVER_H
#include <atomic>
#include <mutex>
#include <string>

// Start/Stop request work. Socket teardown never runs on LVGL/esp_timer.
class SettingsServer {
public:
    static SettingsServer& GetInstance();
    bool Start();
    void Stop();
    bool IsRunning() const;
    bool IsRequested() const { return requested_.load(); }
    std::string pin() const;
    std::string session() const;
    std::string NewSession();
    bool locked() const { return failures_.load() >= 5; }
    void RegisterFailure() { failures_++; }
    std::string Address() const;

private:
    SettingsServer() = default;
    void Run();
    bool Open();
    void* handle_ = nullptr;  // worker-owned httpd_handle_t
    std::atomic<bool> worker_started_{false};
    std::atomic<bool> requested_{false};
    std::atomic<int64_t> expires_us_{0};
    mutable std::mutex mutex_;
    std::string pin_;
    std::string session_;
    std::atomic<int> failures_{0};
};
#endif
