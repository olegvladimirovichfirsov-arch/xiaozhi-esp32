#ifndef MODULAR_HA_CLIENT_H
#define MODULAR_HA_CLIENT_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class WebSocket;

// One Home Assistant entity shown on the speaker.
struct HaEntity {
    std::string id;      // light.living_room
    std::string name;    // friendly name
    std::string domain;  // light, scene, climate, switch...
    std::string state;   // on/off/heat/unavailable...
    int brightness = -1;        // 0..255, -1 = n/a
    float target_temp = -1000;  // climate target, -1000 = n/a
    float current_temp = -1000;
};

// Connects to Home Assistant over its WebSocket API and mirrors every entity
// that carries the configured label (default "колонка"). New labelled entities
// appear automatically; nothing is hard-coded in the firmware.
//
// Settings live in NVS namespace "ha": url (local), url_ext (external),
// token (long-lived, write-only from the web page), label.
class HaClient {
public:
    static HaClient& GetInstance();

    void Start();      // idempotent; spawns the connection task
    void Reconnect();  // drop the connection and re-read settings
    bool IsConfigured();
    bool IsConnected() const { return connected_.load(); }
    std::string StatusText();

    // Bumps whenever the entity list or any state changed.
    uint32_t version() const { return version_.load(); }
    std::vector<HaEntity> Snapshot();

    void CallService(const std::string& domain, const std::string& service,
                     const std::string& entity_id, const std::string& data_json = "");

private:
    HaClient() = default;
    void Run();
    bool ConnectOnce(const std::string& base_url, const std::string& token);
    void HandleMessage(const char* data, size_t len);
    void FinishRegistry();
    void ApplyState(const std::string& id, const void* state_obj, bool partial);
    bool SendJson(void* cjson_obj);
    void SetStatus(const std::string& s);

    std::atomic<bool> connected_{false};
    std::atomic<bool> reconnect_{false};
    std::atomic<bool> started_{false};
    std::atomic<uint32_t> version_{0};
    std::atomic<int> next_id_{100};

    std::mutex mutex_;       // guards entities_, status_
    std::mutex send_mutex_;  // guards ws_ sends
    std::string status_ = "не настроено";
    std::map<std::string, HaEntity> entities_;
    std::vector<std::string> order_;

    std::unique_ptr<WebSocket> ws_;
    std::string token_;
    std::string label_;
    std::string label_id_;
    bool got_labels_ = false;
    bool got_registry_ = false;
    std::vector<std::pair<std::string, std::vector<std::string>>> registry_;  // id -> label ids
    bool auth_failed_ = false;
};

#endif  // MODULAR_HA_CLIENT_H
