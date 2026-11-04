#include "settings_server.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <cstdio>
#include <cstring>

#include "ha_client.h"
#include "settings.h"

#define TAG "SettingsServer"

namespace {

constexpr int64_t kAutoStopUs = 10LL * 60 * 1000000;

std::string UrlDecode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size() + 0 && isxdigit((unsigned char)in[i + 1]) &&
                   isxdigit((unsigned char)in[i + 2])) {
            out += static_cast<char>(std::stoi(in.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

std::string FormField(const std::string& body, const char* key) {
    std::string k = std::string(key) + "=";
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find('&', pos);
        if (end == std::string::npos) {
            end = body.size();
        }
        if (body.compare(pos, k.size(), k) == 0) {
            return UrlDecode(body.substr(pos + k.size(), end - pos - k.size()));
        }
        pos = end + 1;
    }
    return "";
}

std::string HtmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else if (c == '&') o += "&amp;";
        else if (c == '"') o += "&quot;";
        else o += c;
    }
    return o;
}

const char* kHead =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Колонка</title><style>"
    "body{font-family:-apple-system,sans-serif;background:#111;color:#eee;margin:0;padding:20px}"
    "h1{font-size:20px}label{display:block;margin-top:14px;color:#aaa;font-size:14px}"
    "input{width:100%;box-sizing:border-box;padding:12px;margin-top:4px;border-radius:10px;"
    "border:1px solid #333;background:#1c1c1e;color:#fff;font-size:16px}"
    "button{margin-top:20px;width:100%;padding:14px;border:0;border-radius:12px;"
    "background:#d7ff1e;color:#000;font-size:17px;font-weight:600}"
    ".h{color:#888;font-size:13px;margin-top:6px}</style></head><body>";

esp_err_t HandleRoot(httpd_req_t* req) {
    Settings s("ha", false);
    std::string html = kHead;
    html += "<h1>Настройка колонки</h1><form method='post' action='/save'>";
    html += "<label>PIN с экрана колонки</label><input name='pin' inputmode='numeric' maxlength='4'>";
    html += "<label>Адрес Home Assistant дома</label><input name='url' placeholder='http://192.168.1.50:8123' value='" +
            HtmlEscape(s.GetString("url")) + "'>";
    html += "<label>Внешний адрес (когда вы не дома)</label><input name='url_ext' placeholder='https://ha.example.com' value='" +
            HtmlEscape(s.GetString("url_ext")) + "'>";
    html += "<label>Токен доступа HA</label><input name='token' type='password' placeholder=";
    html += s.GetString("token").empty() ? "'не задан'" : "'задан (оставьте пустым, чтобы не менять)'";
    html += ">";
    html += "<div class='h'>Токен создаётся в HA: профиль, Долгосрочные токены. Лучше отдельный пользователь без прав администратора.</div>";
    html += "<label>Метка сущностей</label><input name='label' value='" +
            HtmlEscape(s.GetString("label", "колонка")) + "'>";
    html += "<div class='h'>Всё, что помечено этой меткой в HA, появится на экране колонки.</div>";
    html += "<button>Сохранить</button></form></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html.c_str(), html.size());
}

esp_err_t HandleSave(httpd_req_t* req) {
    if (req->content_len == 0 || req->content_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_FAIL;
    }
    std::string body(req->content_len, '\0');
    int got = 0;
    while (got < static_cast<int>(req->content_len)) {
        int r = httpd_req_recv(req, &body[got], req->content_len - got);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return ESP_FAIL;
        }
        got += r;
    }
    std::string html = kHead;
    if (FormField(body, "pin") != SettingsServer::GetInstance().pin()) {
        html += "<h1>Неверный PIN</h1><p><a href='/' style='color:#d7ff1e'>Назад</a></p></body></html>";
    } else {
        Settings s("ha", true);
        s.SetString("url", FormField(body, "url"));
        s.SetString("url_ext", FormField(body, "url_ext"));
        std::string token = FormField(body, "token");
        if (!token.empty()) {
            s.SetString("token", token);
        }
        std::string label = FormField(body, "label");
        s.SetString("label", label.empty() ? "колонка" : label);
        HaClient::GetInstance().Reconnect();
        html += "<h1>Сохранено</h1><p>Колонка подключается к Home Assistant.</p></body></html>";
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html.c_str(), html.size());
}

}  // namespace

SettingsServer& SettingsServer::GetInstance() {
    static SettingsServer instance;
    return instance;
}

std::string SettingsServer::Address() const {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return "";
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "http://" IPSTR, IP2STR(&info.ip));
    return buf;
}

bool SettingsServer::Start() {
    if (handle_ != nullptr) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(timer_));
        esp_timer_start_once(static_cast<esp_timer_handle_t>(timer_), kAutoStopUs);
        return true;
    }
    char pin[8];
    snprintf(pin, sizeof(pin), "%04u", static_cast<unsigned>(esp_random() % 10000));
    pin_ = pin;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 4;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = HandleRoot, .user_ctx = nullptr};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = HandleSave, .user_ctx = nullptr};
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    handle_ = server;

    if (timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) { SettingsServer::GetInstance().Stop(); };
        args.name = "settings_srv_stop";
        esp_timer_handle_t t = nullptr;
        esp_timer_create(&args, &t);
        timer_ = t;
    }
    esp_timer_start_once(static_cast<esp_timer_handle_t>(timer_), kAutoStopUs);
    return true;
}

void SettingsServer::Stop() {
    if (handle_ != nullptr) {
        httpd_stop(static_cast<httpd_handle_t>(handle_));
        handle_ = nullptr;
        pin_.clear();
    }
}
