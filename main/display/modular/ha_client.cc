#include "ha_client.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <web_socket.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "board.h"
#include "config_mutex.h"
#include "input_validation.h"
#include "settings.h"

#define TAG "HaClient"

namespace {

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
    if (d == "climate")
        return 0;
    if (d == "light")
        return 1;
    if (d == "switch")
        return 2;
    if (d == "scene")
        return 3;
    return 4;
}

}  // namespace

HaClient& HaClient::GetInstance() {
    static HaClient instance;
    return instance;
}

bool HaClient::IsConfigured() { return configured_.load(); }

std::string HaClient::StatusText() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void HaClient::SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ != s) {
        status_ = s;
        version_++;
    }
}

void HaClient::Start() {
    if (started_.exchange(true)) {
        return;
    }
    if (xTaskCreate([](void* arg) { static_cast<HaClient*>(arg)->Run(); }, "ha_client", 12288, this,
                    2, nullptr) != pdPASS) {
        started_ = false;
        SetStatus("не хватает памяти для HA");
    }
}

void HaClient::Reconnect() {
    reconnect_ = true;
    Start();  // also retry a previously failed task allocation
}

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
    if (!msg)
        return;
    const bool complete = cJSON_AddNumberToObject(msg, "id", next_id_++) &&
                          cJSON_AddStringToObject(msg, "type", "call_service") &&
                          cJSON_AddStringToObject(msg, "domain", domain.c_str()) &&
                          cJSON_AddStringToObject(msg, "service", service.c_str());
    cJSON* target = cJSON_AddObjectToObject(msg, "target");
    if (!complete || !target || !cJSON_AddStringToObject(target, "entity_id", entity_id.c_str())) {
        cJSON_Delete(msg);
        return;  // never send a service command with a missing target
    }
    if (!data_json.empty()) {
        cJSON* data = cJSON_Parse(data_json.c_str());
        if (!cJSON_IsObject(data) || !cJSON_AddItemToObject(msg, "service_data", data)) {
            cJSON_Delete(data);
            cJSON_Delete(msg);
            return;
        }
    }
    char* text = cJSON_PrintUnformatted(msg);
    cJSON_Delete(msg);
    if (text == nullptr)
        return;
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (connected_ && outgoing_.size() < 8 && strlen(text) <= 2048) {
            outgoing_.emplace_back(text);
            queued = true;
        }
    }
    cJSON_free(text);
    if (!queued)
        SetStatus(connected_ ? "очередь HA заполнена" : "HA недоступен");
}

void HaClient::Run() {
    while (true) {
        std::string url_local, url_ext, token;
        {
            std::lock_guard<std::mutex> config_lock(ModularHaConfigMutex());
            Settings s("ha", false);
            url_local = s.GetString("url");
            url_ext = s.GetString("url_ext");
            token = s.GetString("token");
            label_ = s.GetString("label", "колонка");
        }
        configured_ = !token.empty() && (!url_local.empty() || !url_ext.empty());
        if (!configured_) {
            SetStatus("не настроено");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        reconnect_ = false;
        if (!ModularInput::HaUrl(url_local, false) || !ModularInput::HaUrl(url_ext, true)) {
            SetStatus("проверьте адрес HA; внешний — HTTPS");
            for (int i = 0; i < 50 && !reconnect_; ++i)
                vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        auth_failed_ = false;
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
            SetStatus("нет сети");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        bool ok = false;
        if (!url_local.empty()) {
            SetStatus("подключаюсь (дом)…");
            ok = ConnectOnce(url_local, token);
        }
        if (!ok && !url_ext.empty() && !auth_failed_ && !reconnect_) {
            SetStatus("подключаюсь (внешний)…");
            ok = ConnectOnce(url_ext, token);
        }
        if (auth_failed_) {
            SetStatus("неверный токен");
            for (int i = 0; i < 300 && !reconnect_; ++i)
                vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            if (!ok) {
                SetStatus("нет связи с HA");
            }
            for (int i = 0; i < 50 && !reconnect_; ++i)
                vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

bool HaClient::ConnectOnce(const std::string& base_url, const std::string& token) {
    auto* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        return false;
    }
    ws_ = network->CreateWebSocket(2);
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        incoming_.clear();
        outgoing_.clear();
        incoming_bytes_ = 0;
        queue_overflow_ = false;
    }
    if (!ws_) {
        return false;
    }
    token_ = token;
    auth_failed_ = false;
    got_labels_ = got_registry_ = false;
    subscription_id_ = 0;
    label_id_.clear();
    registry_.clear();

    ws_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary || data == nullptr || len == 0)
            return;
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (len > 128 * 1024 || incoming_bytes_ + len > 192 * 1024 || incoming_.size() >= 16) {
            queue_overflow_ = true;
            return;
        }
        incoming_.emplace_back(data, len);
        incoming_bytes_ += len;
    });
    ws_->OnDisconnected([this]() {
        connected_ = false;
        version_++;
    });

    std::string ws_url = ToWsUrl(base_url);
    ESP_LOGI(TAG, "Connecting to %s", ws_url.c_str());
    if (auto r = ws_->Connect(ws_url.c_str()); !r) {
        ESP_LOGW(TAG, "Connect failed");
        ws_.reset();
        return false;
    }

    // Drain outside queue_mutex_: callbacks may enqueue while Send/Close joins TCP.
    const int64_t started_at = esp_timer_get_time();
    int64_t registry_at = started_at;
    while (!reconnect_ && !auth_failed_ && !queue_overflow_) {
        std::string input, output;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!incoming_.empty()) {
                input = std::move(incoming_.front());
                incoming_.pop_front();
                incoming_bytes_ -= input.size();
            }
            if (connected_ && !outgoing_.empty()) {
                output = std::move(outgoing_.front());
                outgoing_.pop_front();
            }
        }
        if (!input.empty())
            HandleMessage(input.data(), input.size());
        // HA can send auth_invalid and close immediately. Drain already queued
        // messages before teardown so that the actual error is retained.
        if (!ws_->IsConnected()) {
            if (input.empty())
                break;
            continue;
        }
        if (connected_ && got_labels_ && got_registry_ &&
            esp_timer_get_time() - registry_at >= 60000000) {
            registry_at = esp_timer_get_time();
            RequestRegistry();
        }
        if (!output.empty() && !ws_->Send(output))
            break;
        if (esp_timer_get_time() - started_at > 15000000 && !connected_)
            break;
        if (connected_ && (!got_labels_ || !got_registry_) &&
            esp_timer_get_time() - registry_at > 15000000)
            break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    bool was_up = connected_.exchange(false);
    // Destructor joins the receive task. No shared mutex may be held here.
    ws_->Close();
    ws_.reset();
    token_.clear();
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        incoming_.clear();
        outgoing_.clear();
        incoming_bytes_ = 0;
    }
    SetStatus(queue_overflow_ ? "слишком много данных HA" : "HA недоступен");
    version_++;
    return was_up;
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
        if (!msg) {
            cJSON_Delete(root);
            return;
        }
        cJSON_AddStringToObject(msg, "type", "auth");
        cJSON_AddStringToObject(msg, "access_token", token_.c_str());
        SendJson(msg);
        cJSON_Delete(msg);
    } else if (strcmp(type, "auth_invalid") == 0) {
        auth_failed_ = true;
    } else if (strcmp(type, "auth_ok") == 0) {
        connected_ = true;
        SetStatus("подключено");
        RequestRegistry();
    } else if (strcmp(type, "result") == 0) {
        const cJSON* id = cJSON_GetObjectItem(root, "id");
        const cJSON* result = cJSON_GetObjectItem(root, "result");
        int idv = cJSON_IsNumber(id) ? id->valueint : -1;
        if ((idv == labels_request_id_ || idv == registry_request_id_ || idv == subscription_id_) &&
            !cJSON_IsTrue(cJSON_GetObjectItem(root, "success"))) {
            SetStatus("HA отклонил запрос");
            reconnect_ = true;
        } else if (idv == labels_request_id_ && cJSON_IsArray(result)) {
            const cJSON* it = nullptr;
            cJSON_ArrayForEach (it, result) {
                const char* name = Str(it, "name");
                const char* lid = Str(it, "label_id");
                if (lid != nullptr && ((name != nullptr && label_ == name) || label_ == lid)) {
                    label_id_ = lid;
                }
            }
            got_labels_ = true;
            if (label_id_.empty()) {
                got_registry_ = true;
                FinishRegistry();
            } else {
                cJSON* request = cJSON_CreateObject();
                if (request) {
                    cJSON_AddNumberToObject(request, "id", registry_request_id_);
                    cJSON_AddStringToObject(request, "type",
                                            "config/entity_registry/list_for_display");
                    if (!SendJson(request))
                        reconnect_ = true;
                    cJSON_Delete(request);
                } else
                    reconnect_ = true;
            }
        } else if (idv == registry_request_id_ && result != nullptr) {
            const cJSON* list =
                cJSON_IsArray(result) ? result : cJSON_GetObjectItem(result, "entities");
            registry_.clear();
            const cJSON* it = nullptr;
            cJSON_ArrayForEach (it, list) {
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
                cJSON_ArrayForEach (l, lb) {
                    if (cJSON_IsString(l)) {
                        labels.emplace_back(l->valuestring);
                    }
                }
                if (std::find(labels.begin(), labels.end(), label_id_) != labels.end()) {
                    registry_.emplace_back(ei, std::move(labels));
                    if (registry_.size() == 64)
                        break;
                }
            }
            got_registry_ = true;
            FinishRegistry();
        } else if (cJSON_IsFalse(cJSON_GetObjectItem(root, "success"))) {
            SetStatus("ошибка команды HA");
        }
    } else if (strcmp(type, "event") == 0) {
        const cJSON* event_id = cJSON_GetObjectItem(root, "id");
        if (!cJSON_IsNumber(event_id) || event_id->valueint != subscription_id_) {
            cJSON_Delete(root);
            return;
        }
        const cJSON* ev = cJSON_GetObjectItem(root, "event");
        if (ev != nullptr) {
            const cJSON* add = cJSON_GetObjectItem(ev, "a");
            const cJSON* chg = cJSON_GetObjectItem(ev, "c");
            const cJSON* rem = cJSON_GetObjectItem(ev, "r");
            const cJSON* it = nullptr;
            if (cJSON_IsObject(add)) {
                cJSON_ArrayForEach (it, add) {
                    ApplyState(it->string, it, false);
                }
            }
            if (cJSON_IsObject(chg)) {
                cJSON_ArrayForEach (it, chg) {
                    const cJSON* plus = cJSON_GetObjectItem(it, "+");
                    if (plus != nullptr) {
                        ApplyState(it->string, plus, true);
                    }
                    const auto* removed = cJSON_GetObjectItem(cJSON_GetObjectItem(it, "-"), "a");
                    if (cJSON_IsArray(removed)) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        const auto found = entities_.find(it->string);
                        if (found == entities_.end())
                            continue;
                        auto& entity = found->second;
                        const cJSON* attr = nullptr;
                        cJSON_ArrayForEach (attr, removed) {
                            if (!cJSON_IsString(attr))
                                continue;
                            const std::string name = attr->valuestring;
                            if (name == "brightness")
                                entity.brightness = -1;
                            else if (name == "temperature")
                                entity.target_temp = -1000;
                            else if (name == "current_temperature")
                                entity.current_temp = -1000;
                            else if (name == "friendly_name")
                                entity.name = entity.id;
                            else if (name == "min_temp")
                                entity.min_temp = -1000;
                            else if (name == "max_temp")
                                entity.max_temp = -1000;
                            else if (name == "target_temp_step")
                                entity.temp_step = 0;
                        }
                        version_++;
                    }
                }
            }
            if (cJSON_IsArray(rem)) {
                const cJSON* r = nullptr;
                std::lock_guard<std::mutex> lock(mutex_);
                cJSON_ArrayForEach (r, rem) {
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

void HaClient::RequestRegistry() {
    got_labels_ = got_registry_ = false;
    registry_.clear();
    label_id_.clear();
    labels_request_id_ = next_id_++;
    registry_request_id_ = next_id_++;
    // Resolve the label before requesting entities, so only matching entries
    // (at most 64) need to be retained alongside the parsed response.
    cJSON* msg = cJSON_CreateObject();
    if (!msg) {
        reconnect_ = true;
        return;
    }
    cJSON_AddNumberToObject(msg, "id", labels_request_id_);
    cJSON_AddStringToObject(msg, "type", "config/label_registry/list");
    if (!SendJson(msg))
        reconnect_ = true;
    cJSON_Delete(msg);
}

void HaClient::FinishRegistry() {
    if (!got_labels_ || !got_registry_) {
        return;
    }
    std::vector<std::string> ids;
    if (!label_id_.empty()) {
        for (const auto& [id, labels] : registry_) {
            if (std::find(labels.begin(), labels.end(), label_id_) != labels.end()) {
                if (ids.size() < 64)
                    ids.push_back(id);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (order_ == ids && (subscription_id_ != 0 || ids.empty())) {
            status_ = ids.empty() ? "нет сущностей с меткой «" + label_ + "»" : "подключено";
            version_++;
            return;
        }
        for (auto it = entities_.begin(); it != entities_.end();) {
            if (std::find(ids.begin(), ids.end(), it->first) == ids.end())
                it = entities_.erase(it);
            else
                ++it;
        }
        order_ = ids;
        status_ = ids.empty() ? ("нет сущностей с меткой «" + label_ + "»") : "подключено";
        version_++;
    }
    if (subscription_id_) {
        cJSON* stop = cJSON_CreateObject();
        if (!stop) {
            reconnect_ = true;
            return;
        }
        cJSON_AddNumberToObject(stop, "id", next_id_++);
        cJSON_AddStringToObject(stop, "type", "unsubscribe_events");
        cJSON_AddNumberToObject(stop, "subscription", subscription_id_);
        SendJson(stop);
        cJSON_Delete(stop);
        subscription_id_ = 0;
    }
    if (ids.empty()) {
        return;
    }
    cJSON* m = cJSON_CreateObject();
    if (!m) {
        reconnect_ = true;
        return;
    }
    subscription_id_ = next_id_++;
    cJSON_AddNumberToObject(m, "id", subscription_id_);
    cJSON_AddStringToObject(m, "type", "subscribe_entities");
    cJSON* arr = cJSON_AddArrayToObject(m, "entity_ids");
    bool complete = arr != nullptr;
    for (const auto& id : ids) {
        cJSON* item = cJSON_CreateString(id.c_str());
        if (!item || !cJSON_AddItemToArray(arr, item)) {
            cJSON_Delete(item);
            complete = false;
            break;
        }
    }
    if (!complete || !SendJson(m))
        reconnect_ = true;
    cJSON_Delete(m);
}

void HaClient::ApplyState(const std::string& id, const void* state_obj, bool partial) {
    auto* o = static_cast<const cJSON*>(state_obj);
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(order_.begin(), order_.end(), id) == order_.end()) {
        return;
    }
    HaEntity& e = entities_[id];
    const HaEntity before = e;
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
            e.name = strlen(n) <= 160 ? n : e.id;
        }
        const cJSON* b = cJSON_GetObjectItem(a, "brightness");
        if (cJSON_IsNumber(b))
            e.brightness = std::clamp(b->valueint, 0, 255);
        else if (cJSON_IsNull(b) || !partial)
            e.brightness = -1;
        const cJSON* t = cJSON_GetObjectItem(a, "temperature");
        if (cJSON_IsNumber(t) && std::isfinite(t->valuedouble))
            e.target_temp = static_cast<float>(t->valuedouble);
        else if (cJSON_IsNull(t) || !partial)
            e.target_temp = -1000;
        const cJSON* c = cJSON_GetObjectItem(a, "current_temperature");
        if (cJSON_IsNumber(c) && std::isfinite(c->valuedouble))
            e.current_temp = static_cast<float>(c->valuedouble);
        else if (cJSON_IsNull(c) || !partial)
            e.current_temp = -1000;
        const auto number = [a, partial](const char* key, float& value, float missing) {
            const auto* item = cJSON_GetObjectItem(a, key);
            if (cJSON_IsNumber(item) && std::isfinite(item->valuedouble))
                value = item->valuedouble;
            else if (cJSON_IsNull(item) || !partial)
                value = missing;
        };
        number("min_temp", e.min_temp, -1000);
        number("max_temp", e.max_temp, -1000);
        number("target_temp_step", e.temp_step, 0);
    }
    if (before.id != e.id || before.name != e.name || before.state != e.state ||
        before.brightness != e.brightness || before.target_temp != e.target_temp ||
        before.current_temp != e.current_temp || before.min_temp != e.min_temp ||
        before.max_temp != e.max_temp || before.temp_step != e.temp_step)
        version_++;
}
