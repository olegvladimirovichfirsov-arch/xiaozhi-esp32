#ifndef MODULAR_SETTINGS_SERVER_H
#define MODULAR_SETTINGS_SERVER_H

#include <string>

// Tiny phone-friendly web page for settings that are painful to type on a
// 360x360 touch screen: Home Assistant URLs, token and label. It runs only
// for a few minutes after the user opens it on the device and asks for a
// 4-digit PIN that is shown on the device screen.
class SettingsServer {
public:
    static SettingsServer& GetInstance();

    bool Start();  // returns false if it cannot start
    void Stop();
    bool IsRunning() const { return handle_ != nullptr; }
    const std::string& pin() const { return pin_; }
    const std::string& session() const { return session_; }
    std::string NewSession();
    bool locked() const { return failures_ >= 5; }
    void RegisterFailure() { failures_++; }
    std::string Address() const;  // http://<ip>

private:
    SettingsServer() = default;
    void* handle_ = nullptr;  // httpd_handle_t
    void* timer_ = nullptr;   // esp_timer_handle_t
    std::string pin_;
    std::string session_;
    int failures_ = 0;
};

#endif  // MODULAR_SETTINGS_SERVER_H
