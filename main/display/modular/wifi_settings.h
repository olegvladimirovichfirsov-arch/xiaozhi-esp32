#pragma once

#include <ssid_manager.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Serialize modular UI and HTTP accesses. The upstream Wi-Fi task does not
// participate in this mutex; this is not a lock for the upstream manager.
namespace ModularWifi {
inline std::mutex& Mutex() {
    static std::mutex mutex;
    return mutex;
}
inline std::atomic<uint32_t>& Version() {
    static std::atomic<uint32_t> version{0};
    return version;
}
inline std::vector<SsidItem> Snapshot() {
    std::lock_guard<std::mutex> lock(Mutex());
    return SsidManager::GetInstance().GetSsidList();
}
inline void Add(const std::string& ssid, const std::string& password) {
    std::lock_guard<std::mutex> lock(Mutex());
    SsidManager::GetInstance().AddSsid(ssid, password);
    Version().fetch_add(1);
}
inline bool Remove(const std::string& ssid) {
    std::lock_guard<std::mutex> lock(Mutex());
    auto& manager = SsidManager::GetInstance();
    const auto& networks = manager.GetSsidList();
    for (size_t i = 0; i < networks.size(); ++i) {
        if (networks[i].ssid == ssid) {
            manager.RemoveSsid(static_cast<int>(i));
            Version().fetch_add(1);
            return true;
        }
    }
    return false;
}
}  // namespace ModularWifi
