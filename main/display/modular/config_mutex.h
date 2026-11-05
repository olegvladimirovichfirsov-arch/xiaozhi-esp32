#pragma once
#include <mutex>

// Writers commit before Reconnect(); the HA task takes one coherent snapshot.
inline std::mutex& ModularHaConfigMutex() {
    static std::mutex mutex;
    return mutex;
}
