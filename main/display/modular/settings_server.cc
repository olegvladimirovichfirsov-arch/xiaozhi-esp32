#include "settings_server.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include "input_validation.h"
#include "wifi_settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "config_mutex.h"
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
        } else if (in[i] == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) &&
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

std::string JsonEscape(const std::string& in) {
    std::string o;
    for (unsigned char c : in) {
        switch (c) {
            case '"':
                o += "\\\"";
                break;
            case '\\':
                o += "\\\\";
                break;
            case '\n':
                o += "\\n";
                break;
            case '\r':
                o += "\\r";
                break;
            case '\t':
                o += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

const char kPage[] = R"HTML(<!doctype html><html lang="ru"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="light dark"><title>Колонка</title><style>
:root{--bg:#f2f2f7;--card:#fff;--fg:#1c1c1e;--sub:#8e8e93;--line:#e5e5ea;--acc:#007aff;--red:#ff3b30}
@media(prefers-color-scheme:dark){:root{--bg:#000;--card:#1c1c1e;--fg:#fff;--line:#38383a;--acc:#0a84ff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.35 -apple-system,BlinkMacSystemFont,"SF Pro Text",system-ui,sans-serif;padding:max(20px,env(safe-area-inset-top)) 16px 40px}
main{max-width:560px;margin:0 auto}h1{font-size:32px;margin:12px 4px 18px;letter-spacing:-.5px}
h2{font-size:13px;text-transform:uppercase;color:var(--sub);margin:26px 14px 8px;font-weight:500}
.card{background:var(--card);border-radius:14px;overflow:hidden}.row{display:flex;align-items:center;padding:12px 16px;border-top:1px solid var(--line);gap:10px}
.row:first-child{border-top:0}.row b{font-weight:500;flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tag{color:#34c759;font-size:14px}.empty{color:var(--sub);padding:14px 16px}
input{width:100%;border:0;background:transparent;color:var(--fg);font:inherit;padding:0;outline:0}
.f{padding:11px 16px;border-top:1px solid var(--line)}.f:first-child{border-top:0}.f label{display:block;font-size:12px;color:var(--sub);margin-bottom:2px}
button{font:inherit;border:0;border-radius:12px;padding:14px;width:100%;background:var(--acc);color:#fff;font-weight:600;margin-top:12px;cursor:pointer}
button.l{background:none;color:var(--red);width:auto;margin:0;padding:4px 0;font-weight:500}button:disabled{opacity:.5}
.hint{color:var(--sub);font-size:13px;margin:8px 14px}.toast{position:fixed;left:50%;bottom:28px;transform:translateX(-50%);background:#323234;color:#fff;padding:10px 18px;border-radius:20px;opacity:0;transition:.25s;pointer-events:none}
.toast.on{opacity:1}#pin{font-size:34px;text-align:center;letter-spacing:12px}
</style></head><body><main><h1>Колонка</h1><div id="app"></div></main><div class="toast" id="t"></div><script>
const $=s=>document.querySelector(s),app=$('#app');
function toast(m){const t=$('#t');t.textContent=m;t.classList.add('on');setTimeout(()=>t.classList.remove('on'),2200)}
const E=s=>String(s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
async function api(p,d){const r=await fetch(p,d?{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Modui':'1'},body:new URLSearchParams(d)}:{});if(r.status==401){login();throw 0}if(r.status==429){app.innerHTML='<p class="hint">Слишком много попыток. Откройте настройку на колонке заново.</p>';throw 0}if(!r.ok){let m='Не удалось сохранить';try{m=(await r.json()).error||m}catch{}toast(m);throw new Error(m)}return r.json()}
function login(){app.innerHTML='<h2>PIN с экрана колонки</h2><div class="card"><div class="f"><input id="pin" inputmode="numeric" maxlength="4" autofocus placeholder="····"></div></div><button id="go">Войти</button>';
const go=async()=>{const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Modui':'1'},body:new URLSearchParams({pin:$('#pin').value})});if(r.ok)load();else if(r.status==429)app.innerHTML='<p class="hint">Слишком много попыток. Откройте настройку на колонке заново.</p>';else{toast('Неверный PIN');$('#pin').value=''}};
$('#go').onclick=go;$('#pin').oninput=e=>{if(e.target.value.length==4)go()}}
async function load(){const s=await api('/api/state');
app.innerHTML='<h2>Сети Wi-Fi</h2><div class="card" id="nets">'+(s.wifi.length?s.wifi.map((w,i)=>'<div class="row"><b>'+E(w.ssid)+'</b>'+(w.current?'<span class="tag">подключена</span>':'<button class="l" data-d="'+E(w.ssid)+'">Удалить</button>')+'</div>').join(''):'<div class="empty">Нет сохранённых сетей</div>')+'</div>'+
'<h2>Добавить сеть</h2><div class="card"><div class="f"><label>Имя сети</label><input id="ws" autocapitalize="off" autocorrect="off"></div><div class="f"><label>Пароль</label><input id="wp" type="password" autocapitalize="off"></div></div><button id="wa">Добавить</button><p class="hint">Колонка подключится к любой из сохранённых сетей, которая окажется рядом: дом, офис, телефон в машине.</p>'+
'<h2>Home Assistant</h2><div class="card"><div class="f"><label>Адрес дома</label><input id="u" value="'+E(s.ha.url)+'" placeholder="http://192.168.1.50:8123"></div><div class="f"><label>Внешний адрес (HTTPS)</label><input id="x" value="'+E(s.ha.url_ext)+'" placeholder="https://ha.example.com"></div><div class="f"><label>Токен доступа</label><input id="k" type="password" placeholder="'+(s.ha.token_set?'задан, пусто = не менять':'не задан')+'"></div><div class="f"><label>Метка сущностей</label><input id="l" value="'+E(s.ha.label)+'"></div></div><button id="hs">Сохранить</button><p class="hint">Всё, что помечено этой меткой в Home Assistant, появится на экране колонки.</p>';
document.querySelectorAll('[data-d]').forEach(b=>b.onclick=async()=>{await api('/api/wifi/del',{ssid:b.dataset.d});toast('Сеть удалена');load()});
$('#wa').onclick=async()=>{if(!$('#ws').value.trim()){toast('Введите имя сети');return}await api('/api/wifi/add',{ssid:$('#ws').value.trim(),password:$('#wp').value});toast('Сеть добавлена');load()};
$('#hs').onclick=async()=>{await api('/api/ha',{url:$('#u').value.trim(),url_ext:$('#x').value.trim(),token:$('#k').value.trim(),label:$('#l').value.trim()});toast('Сохранено');load()}}
load().catch(()=>{});
</script></body></html>)HTML";

esp_err_t SendError(httpd_req_t* req, const char* status, const char* message) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const std::string json = "{\"error\":\"" + JsonEscape(message) + "\"}";
    return httpd_resp_send(req, json.c_str(), json.size());
}

bool LocalRequest(httpd_req_t* req) {
    auto& srv = SettingsServer::GetInstance();
    if (!srv.IsRunning())
        return false;
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    sockaddr_storage peer = {};
    socklen_t size = sizeof(peer);
    if (!sta || esp_netif_get_ip_info(sta, &info) != ESP_OK || info.ip.addr == 0 ||
        info.netmask.addr == 0 ||
        getpeername(httpd_req_to_sockfd(req), reinterpret_cast<sockaddr*>(&peer), &size) != 0 ||
        peer.ss_family != AF_INET)
        return false;
    auto address = reinterpret_cast<sockaddr_in*>(&peer)->sin_addr.s_addr;
    if ((address & info.netmask.addr) != (info.ip.addr & info.netmask.addr))
        return false;
    char host[64] = {};
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK)
        return false;
    const auto device_address = srv.Address();
    if (device_address.size() <= 7)
        return false;
    const auto expected = device_address.substr(7);
    if (host != expected && host != expected + ":80")
        return false;
    if (req->method == HTTP_POST) {
        char header[8] = {};
        if (httpd_req_get_hdr_value_str(req, "X-Modui", header, sizeof(header)) != ESP_OK ||
            strcmp(header, "1") != 0)
            return false;
    }
    return true;
}

bool Authed(httpd_req_t* req) {
    auto& srv = SettingsServer::GetInstance();
    if (!LocalRequest(req))
        return false;
    const auto session = srv.session();
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len == 0 || len > 400)
        return false;
    std::string c(len + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, "Cookie", &c[0], len + 1) != ESP_OK)
        return false;
    c.resize(len);
    return ModularInput::Cookie(c, session);
}

bool ReadBody(httpd_req_t* req, std::string& body) {
    if (req->content_len == 0 || req->content_len > 2048) {
        SendError(req, "400 Bad Request", "Слишком большой или пустой запрос");
        return false;
    }
    body.assign(req->content_len, '\0');
    int got = 0;
    while (got < static_cast<int>(req->content_len)) {
        int r = httpd_req_recv(req, &body[got], req->content_len - got);
        if (r <= 0) {
            SendError(req, "400 Bad Request", "Не удалось прочитать запрос");
            return false;
        }
        got += r;
    }
    if (body.find('%') != std::string::npos) {
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] != '%')
                continue;
            if (i + 2 >= body.size() || !isxdigit(static_cast<unsigned char>(body[i + 1])) ||
                !isxdigit(static_cast<unsigned char>(body[i + 2]))) {
                SendError(req, "400 Bad Request", "Некорректный формат запроса");
                return false;
            }
            i += 2;
        }
    }
    if (UrlDecode(body).find('\0') != std::string::npos) {
        SendError(req, "400 Bad Request", "Недопустимый символ в запросе");
        return false;
    }
    return true;
}

esp_err_t SendJson(httpd_req_t* req, const std::string& json) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.c_str(), json.size());
}

esp_err_t Deny(httpd_req_t* req) {
    httpd_resp_set_status(req, "401 Unauthorized");
    return SendJson(req, "{}");
}

esp_err_t HandleRoot(httpd_req_t* req) {
    if (!LocalRequest(req))
        return Deny(req);
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Content-Security-Policy",
                       "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                       "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kPage, sizeof(kPage) - 1);
}

esp_err_t HandleLogin(httpd_req_t* req) {
    if (!LocalRequest(req))
        return Deny(req);
    auto& srv = SettingsServer::GetInstance();
    std::string body;
    if (srv.locked()) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return SendJson(req, "{}");
    }
    if (!ReadBody(req, body)) {
        return ESP_FAIL;
    }
    const auto pin = srv.pin();
    if (pin.empty() || FormField(body, "pin") != pin) {
        srv.RegisterFailure();
        httpd_resp_set_status(req, srv.locked() ? "429 Too Many Requests" : "403 Forbidden");
        return SendJson(req, "{}");
    }
    const auto session = srv.NewSession();
    if (session.empty())
        return Deny(req);
    std::string cookie = "s=" + session + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=600";
    httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t HandleState(httpd_req_t* req) {
    if (!Authed(req)) {
        return Deny(req);
    }
    std::string current;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        current = reinterpret_cast<const char*>(ap.ssid);
    }
    std::string j = "{\"wifi\":[";
    const auto list = ModularWifi::Snapshot();
    for (size_t i = 0; i < list.size(); i++) {
        if (i)
            j += ",";
        j += "{\"ssid\":\"" + JsonEscape(list[i].ssid) +
             "\",\"current\":" + (list[i].ssid == current ? "true" : "false") + "}";
    }
    std::lock_guard<std::mutex> config_lock(ModularHaConfigMutex());
    Settings s("ha", false);
    j += "],\"ha\":{\"url\":\"" + JsonEscape(s.GetString("url")) + "\",\"url_ext\":\"" +
         JsonEscape(s.GetString("url_ext")) + "\",\"label\":\"" +
         JsonEscape(s.GetString("label", "колонка")) +
         "\",\"token_set\":" + (s.GetString("token").empty() ? "false" : "true") + "}}";
    return SendJson(req, j);
}

esp_err_t HandleWifiAdd(httpd_req_t* req) {
    if (!Authed(req)) {
        return Deny(req);
    }
    std::string body;
    if (!ReadBody(req, body)) {
        return ESP_FAIL;
    }
    std::string ssid = FormField(body, "ssid");
    if (ssid.empty() || ssid.size() > 32) {
        SendError(req, "400 Bad Request", "Введите имя сети длиной до 32 байт");
        return ESP_FAIL;
    }
    std::string password = FormField(body, "password");
    if (!ModularInput::Clean(ssid, 32) || !ModularInput::Clean(password, 64) ||
        (!password.empty() && password.size() < 8)) {
        SendError(req, "400 Bad Request", "Проверьте имя сети и пароль");
        return ESP_FAIL;
    }
    if (password.size() == 64 && !std::all_of(password.begin(), password.end(),
                                              [](unsigned char c) { return std::isxdigit(c); })) {
        SendError(req, "400 Bad Request", "Пароль из 64 символов должен быть шестнадцатеричным");
        return ESP_FAIL;
    }
    ModularWifi::Add(ssid, password);
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t HandleWifiDel(httpd_req_t* req) {
    if (!Authed(req)) {
        return Deny(req);
    }
    std::string body;
    if (!ReadBody(req, body)) {
        return ESP_FAIL;
    }
    std::string ssid = FormField(body, "ssid");
    ModularWifi::Remove(ssid);
    return SendJson(req, "{\"ok\":true}");
}

esp_err_t HandleHa(httpd_req_t* req) {
    if (!Authed(req)) {
        return Deny(req);
    }
    std::string body;
    if (!ReadBody(req, body)) {
        return ESP_FAIL;
    }
    auto url = FormField(body, "url");
    auto external = FormField(body, "url_ext");
    auto token = FormField(body, "token");
    auto label = FormField(body, "label");
    if (!ModularInput::HaUrl(url, false) || !ModularInput::HaUrl(external, true) ||
        !ModularInput::Clean(token, 512) || !ModularInput::Clean(label, 128)) {
        SendError(req, "400 Bad Request",
                  "Проверьте адрес HA. Внешний адрес должен использовать HTTPS");
        return ESP_FAIL;
    }
    // Checked NVS calls: Settings::SetString aborts firmware on write errors.
    std::lock_guard<std::mutex> config_lock(ModularHaConfigMutex());
    nvs_handle_t h;
    esp_err_t err = nvs_open("ha", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "url", url.c_str());
        if (err == ESP_OK)
            err = nvs_set_str(h, "url_ext", external.c_str());
        if (err == ESP_OK && !token.empty())
            err = nvs_set_str(h, "token", token.c_str());
        if (err == ESP_OK)
            err = nvs_set_str(h, "label", label.empty() ? "колонка" : label.c_str());
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        SendError(req, "500 Internal Server Error", "Не удалось сохранить настройки");
        return ESP_FAIL;
    }
    HaClient::GetInstance().Reconnect();
    return SendJson(req, "{\"ok\":true}");
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

bool SettingsServer::IsRunning() const {
    const auto deadline = expires_us_.load();
    return requested_ && deadline != 0 && esp_timer_get_time() < deadline;
}

std::string SettingsServer::pin() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsRunning() ? pin_ : "";
}

std::string SettingsServer::session() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return IsRunning() ? session_ : "";
}

bool SettingsServer::Start() {
    if (!worker_started_.exchange(true)) {
        if (xTaskCreate([](void* arg) { static_cast<SettingsServer*>(arg)->Run(); },
                        "modui_settings", 4096, this, 2, nullptr) != pdPASS) {
            worker_started_ = false;
            return false;
        }
    }
    requested_ = true;
    return true;
}

void SettingsServer::Stop() { requested_ = false; }

void SettingsServer::Run() {
    while (true) {
        if (handle_ && (!requested_ || esp_timer_get_time() >= expires_us_)) {
            // Invalidate credentials before joining HTTP handlers.
            expires_us_ = 0;
            httpd_stop(static_cast<httpd_handle_t>(handle_));
            handle_ = nullptr;
            std::lock_guard<std::mutex> lock(mutex_);
            pin_.clear();
            session_.clear();
            requested_ = false;
        }
        if (requested_ && !handle_ && !Open())
            requested_ = false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

bool SettingsServer::Open() {
    if (Address().empty())
        return false;
    char pin[8];
    snprintf(pin, sizeof(pin), "%04u", static_cast<unsigned>(esp_random() % 10000));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pin_ = pin;
        session_.clear();
        failures_ = 0;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 3;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    config.lru_purge_enable = true;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK)
        return false;
    struct Route {
        const char* uri;
        httpd_method_t method;
        esp_err_t (*fn)(httpd_req_t*);
    };
    const Route routes[] = {
        {"/", HTTP_GET, HandleRoot},
        {"/api/login", HTTP_POST, HandleLogin},
        {"/api/state", HTTP_GET, HandleState},
        {"/api/wifi/add", HTTP_POST, HandleWifiAdd},
        {"/api/wifi/del", HTTP_POST, HandleWifiDel},
        {"/api/ha", HTTP_POST, HandleHa},
    };
    for (const auto& r : routes) {
        httpd_uri_t u = {};
        u.uri = r.uri;
        u.method = r.method;
        u.handler = r.fn;
        if (httpd_register_uri_handler(server, &u) != ESP_OK) {
            httpd_stop(server);
            return false;
        }
    }
    handle_ = server;
    expires_us_ = esp_timer_get_time() + kAutoStopUs;
    return true;
}

std::string SettingsServer::NewSession() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsRunning())
        return "";
    char buf[40];
    snprintf(buf, sizeof(buf), "%08x%08x%08x%08x", static_cast<unsigned>(esp_random()),
             static_cast<unsigned>(esp_random()), static_cast<unsigned>(esp_random()),
             static_cast<unsigned>(esp_random()));
    session_ = buf;
    return session_;
}
