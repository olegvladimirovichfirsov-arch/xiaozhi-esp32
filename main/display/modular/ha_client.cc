#include "ha_client.h"

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <web_socket.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "board.h"
#include "settings.h"

#define TAG "HaClient"

namespace {

constexpr int kIdLabels = 1;
constexpr int kIdRegistry = 2;
constexpr int kIdSubscribe = 3;

std::string ToWsUrl(std::string url) {
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    if (url.rfind("https://", 0) == 0) {
        url = "wss://" + url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        url = "ws://" + url.substr(7);
    } else if (url.rfind("ws://", 0) != 0 && url.rfind("wss://", 0) != 0) {
        url = "ws://" + url;
    }
    if (url.size() < 14 || url.compare(url.size() - 14, 14, "/api/websocket") != 0) {
        url += "/api/websocket";
    }
    return url;
}

const char* Str(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItem(o, key);
    return cJSON_IsString(v) ? v->valuestring : nullptr;
}

int DomainRank(const std::string& d) {
    if (d == "climate") return 0;
    if (d == "light") return 1;
    if (d == "switch") return 2;
    if (d == "scene") return 3;
    return 4;
}

}  // namespace

HaClient& HaClient::GetInstance() {
    static HaClient instance;
    return instance;
}

bool HaClient::IsConfigured() {
    Settings s("ha", false);
    return !s.GetString("token").empty() &&
           (!s.GetString("url").empty() || !s.GetString("url_ext").empty());
}

std::string HaClient::StatusText() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void HaClient::SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = s;
    version_++;
}

void HaClient::Start() {
    if (started_.exchange(true)) {
        return;
    }
    xTaskCreate([](void* arg) { static_cast<HaClient*>(arg)->Run(); }, "ha_client", 12288, this, 3,
                nullptr);
}

void HaClient::Reconnect() { reconnect_ = true; }

std::vector<HaEntity> HaClient::Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<HaEntity> out;
    for (const auto& id : order_) {
        auto it = entities_.find(id);
        if (it != entities_.end()) {
            out.push_back(it->second);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const HaEntity& a, const HaEntity& b) {
        return DomainRank(a.domain) < DomainRank(b.domain);
    });
    return out;
}

bool HaClient::SendJson(void* cjson_obj) {
    auto* obj = static_cast<cJSON*>(cjson_obj);
    char* text = cJSON_PrintUnformatted(obj);
    bool ok = false;
    if (text != nullptr) {
        std::lock_guard<std::mutex> lock(send_mutex_);
        if (ws_ && ws_->IsConnected()) {
            ok = ws_->Send(std::string(text));
        }
        cJSON_free(text);
    }
    return ok;
}

void HaClient::CallService(const std::string& domain, const std::string& service,
                           const std::string& entity_id, const std::string& data_json) {
    if (!connected_) {
        return;
    }
    cJSON* msg = cJSON_CreateObject();
    cJSON_AddNumberToObject(msg, "id", next_id_++);
    cJSON_AddStringToObject(msg, "type", "call_service");
    cJSON_AddStringToObject(msg, "domain", domain.c_str());
    cJSON_AddStringToObject(msg, "service", service.c_str());
    cJSON* target = cJSON_AddObjectToObject(msg, "target");
    cJSON_AddStringToObject(target, "entity_id", entity_id.c_str());
    if (!data_json.empty()) {
        cJSON* data = cJSON_Parse(data_json.c_str());
        if (data != nullptr) {
            cJSON_AddItemToObject(msg, "service_data", data);
        }
    }
    SendJson(msg);
    cJSON_Delete(msg);
}

void HaClient::Run() {
    while (true) {
        std::string url_local, url_ext, token;
        {
            Settings s("ha", false);
            url_local = s.GetString("url");
            url_ext = s.GetString("url_ext");
            token = s.GetString("token");
            label_ = s.GetString("label", "колонка");
        }
        if (token.empty() || (url_local.empty() && url_ext.empty())) {
            SetStatus("не настроено");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        reconnect_ = false;
        bool ok = false;
        if (!url_local.empty()) {
            SetStatus("подключаюсь (дом)…");
            ok = ConnectOnce(url_local, token);
        }
        if (!ok && !url_ext.empty() && !auth_failed_) {
            SetStatus("подключаюсь (внешний)…");
            ok = ConnectOnce(url_ext, token);
        }
        if (auth_failed_) {
            SetStatus("неверный токен");
            vTaskDelay(pdMS_TO_TICKS(30000));
        } else {
            if (!ok) {
                SetStatus("нет связи с HA");
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

bool HaClient::ConnectOnce(const std::string& base_url, const std::string& token) {
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        ws_ = network->CreateWebSocket(2);
    }
    if (!ws_) {
        return false;
    }
    token_ = token;
    auth_failed_ = false;
    got_labels_ = got_registry_ = false;
    label_id_.clear();
    registry_.clear();

    ws_->OnData([this](const char* data, size_t len, bool binary) {
        if (!binary) {
            HandleMessage(data, len);
        }
    });
    ws_->OnDisconnected([this]() { connected_ = false; });

    std::string ws_url = ToWsUrl(base_url);
    ESP_LOGI(TAG, "Connecting to %s", ws_url.c_str());
    if (auto r = ws_->Connect(ws_url.c_str()); !r) {
        ESP_LOGW(TAG, "Connect failed");
        std::lock_guard<std::mutex> lock(send_mutex_);
        ws_.reset();
        return false;
    }

    // Stay until the socket drops or settings change.
    int waited = 0;
    while (ws_->IsConnected() && !reconnect_ && !auth_failed_) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited++;
    }
    bool was_up = connected_.load();
    connected_ = false;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        ws_->Close();
        ws_.reset();
    }
    version_++;
    return was_up || waited > 20;
}

void HaClient::HandleMessage(const char* data, size_t len) {
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        return;
    }
    const char* type = Str(root, "type");
    if (type == nullptr) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type, "auth_required") == 0) {
        cJSON* msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "type", "auth");
        cJSON_AddStringToObject(msg, "access_token", token_.c_str());
        SendJson(msg);
        cJSON_Delete(msg);
    } else if (strcmp(type, "auth_invalid") == 0) {
        auth_failed_ = true;
    } else if (strcmp(type, "auth_ok") == 0) {
        connected_ = true;
        SetStatus("подключено");
        {
            cJSON* m = cJSON_CreateObject();
            cJSON_AddNumberToObject(m, "id", kIdLabels);
            cJSON_AddStringToObject(m, "type", "config/label_registry/list");
            SendJson(m);
            cJSON_Delete(m);
        }
        {
            cJSON* m = cJSON_CreateObject();
            cJSON_AddNumberToObject(m, "id", kIdRegistry);
            cJSON_AddStringToObject(m, "type", "config/entity_registry/list_for_display");
            SendJson(m);
            cJSON_Delete(m);
        }
    } else if (strcmp(type, "result") == 0) {
        const cJSON* id = cJSON_GetObjectItem(root, "id");
        const cJSON* result = cJSON_GetObjectItem(root, "result");
        int idv = cJSON_IsNumber(id) ? id->valueint : -1;
        if (idv == kIdLabels && cJSON_IsArray(result)) {
            const cJSON* it = nullptr;
            cJSON_ArrayForEach(it, result) {
                const char* name = Str(it, "name");
                const char* lid = Str(it, "label_id");
                if (lid != nullptr && ((name != nullptr && label_ == name) || label_ == lid)) {
                    label_id_ = lid;
                }
            }
            got_labels_ = true;
            FinishRegistry();
        } else if (idv == kIdRegistry && result != nullptr) {
            const cJSON* list = cJSON_IsArray(result) ? result : cJSON_GetObjectItem(result, "entities");
            registry_.clear();
            const cJSON* it = nullptr;
            cJSON_ArrayForEach(it, list) {
                const char* ei = Str(it, "ei");
                if (ei == nullptr) {
                    ei = Str(it, "entity_id");
                }
                if (ei == nullptr) {
                    continue;
                }
                std::vector<std::string> labels;
                const cJSON* lb = cJSON_GetObjectItem(it, "lb");
                if (lb == nullptr) {
                    lb = cJSON_GetObjectItem(it, "labels");
                }
                const cJSON* l = nullptr;
                cJSON_ArrayForEach(l, lb) {
                    if (cJSON_IsString(l)) {
                        labels.emplace_back(l->valuestring);
                    }
                }
                if (!labels.empty()) {
                    registry_.emplace_back(ei, std::move(labels));
                }
            }
            got_registry_ = true;
            FinishRegistry();
        }
    } else if (strcmp(type, "event") == 0) {
        const cJSON* ev = cJSON_GetObjectItem(root, "event");
        if (ev != nullptr) {
            const cJSON* add = cJSON_GetObjectItem(ev, "a");
            const cJSON* chg = cJSON_GetObjectItem(ev, "c");
            const cJSON* rem = cJSON_GetObjectItem(ev, "r");
            const cJSON* it = nullptr;
            if (cJSON_IsObject(add)) {
                cJSON_ArrayForEach(it, add) {
                    ApplyState(it->string, it, false);
                }
            }
            if (cJSON_IsObject(chg)) {
                cJSON_ArrayForEach(it, chg) {
                    const cJSON* plus = cJSON_GetObjectItem(it, "+");
                    if (plus != nullptr) {
                        ApplyState(it->string, plus, true);
                    }
                }
            }
            if (cJSON_IsArray(rem)) {
                const cJSON* r = nullptr;
                std::lock_guard<std::mutex> lock(mutex_);
                cJSON_ArrayForEach(r, rem) {
                    if (cJSON_IsString(r)) {
                        entities_.erase(r->valuestring);
                    }
                }
                version_++;
            }
        }
    }
    cJSON_Delete(root);
}

void HaClient::FinishRegistry() {
    if (!got_labels_ || !got_registry_) {
        return;
    }
    std::vector<std::string> ids;
    if (!label_id_.empty()) {
        for (const auto& [id, labels] : registry_) {
            if (std::find(labels.begin(), labels.end(), label_id_) != labels.end()) {
                ids.push_back(id);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entities_.clear();
        order_ = ids;
        status_ = ids.empty() ? ("нет сущностей с меткой «" + label_ + "»") : "подключено";
        version_++;
    }
    if (ids.empty()) {
        return;
    }
    cJSON* m = cJSON_CreateObject();
    cJSON_AddNumberToObject(m, "id", kIdSubscribe);
    cJSON_AddStringToObject(m, "type", "subscribe_entities");
    cJSON* arr = cJSON_AddArrayToObject(m, "entity_ids");
    for (const auto& id : ids) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(id.c_str()));
    }
    SendJson(m);
    cJSON_Delete(m);
}

void HaClient::ApplyState(const std::string& id, const void* state_obj, bool partial) {
    auto* o = static_cast<const cJSON*>(state_obj);
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(order_.begin(), order_.end(), id) == order_.end()) {
        return;
    }
    HaEntity& e = entities_[id];
    if (e.id.empty()) {
        e.id = id;
        e.domain = id.substr(0, id.find('.'));
        e.name = id;
        (void)partial;
    }
    if (const char* s = Str(o, "s")) {
        e.state = s;
    }
    const cJSON* a = cJSON_GetObjectItem(o, "a");
    if (cJSON_IsObject(a)) {
        if (const char* n = Str(a, "friendly_name")) {
            e.name = n;
        }
        const cJSON* b = cJSON_GetObjectItem(a, "brightness");
        if (cJSON_IsNumber(b)) e.brightness = b->valueint;
        const cJSON* t = cJSON_GetObjectItem(a, "temperature");
        if (cJSON_IsNumber(t)) e.target_temp = static_cast<float>(t->valuedouble);
        const cJSON* c = cJSON_GetObjectItem(a, "current_temperature");
        if (cJSON_IsNumber(c)) e.current_temp = static_cast<float>(c->valuedouble);
    }
    version_++;
}
