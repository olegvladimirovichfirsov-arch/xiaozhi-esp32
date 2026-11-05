#include "settings_server.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <ssid_manager.h>
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

std::string JsonEscape(const std::string& in) {
    std::string o;
    for (unsigned char c : in) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
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
async function api(p,d){const r=await fetch(p,d?{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(d)}:{});if(r.status==401){login();throw 0}if(r.status==429){app.innerHTML='<p class="hint">Слишком много попыток. Откройте настройку на колонке заново.</p>';throw 0}return r.json()}
function login(){app.innerHTML='<h2>PIN с экрана колонки</h2><div class="card"><div class="f"><input id="pin" inputmode="numeric" maxlength="4" autofocus placeholder="····"></div></div><button id="go">Войти</button>';
const go=async()=>{const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({pin:$('#pin').value})});if(r.ok)load();else if(r.status==429)app.innerHTML='<p class="hint">Слишком много попыток. Откройте настройку на колонке заново.</p>';else{toast('Неверный PIN');$('#pin').value=''}};
$('#go').onclick=go;$('#pin').oninput=e=>{if(e.target.value.length==4)go()}}
async function load(){const s=await api('/api/state');
app.innerHTML='<h2>Сети Wi-Fi</h2><div class="card" id="nets">'+(s.wifi.length?s.wifi.map((w,i)=>'<div class="row"><b>'+E(w.ssid)+'</b>'+(w.current?'<span class="tag">подключена</span>':'<button class="l" data-d="'+E(w.ssid)+'">Удалить</button>')+'</div>').join(''):'<div class="empty">Нет сохранённых сетей</div>')+'</div>'+
'<h2>Добавить сеть</h2><div class="card"><div class="f"><label>Имя сети</label><input id="ws" autocapitalize="off" autocorrect="off"></div><div class="f"><label>Пароль</label><input id="wp" type="password" autocapitalize="off"></div></div><button id="wa">Добавить</button><p class="hint">Колонка подключится к любой из сохранённых сетей, которая окажется рядом: дом, офис, телефон в машине.</p>'+
'<h2>Home Assistant</h2><div class="card"><div class="f"><label>Адрес дома</label><input id="u" value="'+E(s.ha.url)+'" placeholder="http://192.168.1.50:8123"></div><div class="f"><label>Внешний адрес</label><input id="x" value="'+E(s.ha.url_ext)+'" placeholder="https://ha.example.com"></div><div class="f"><label>Токен доступа</label><input id="k" type="password" placeholder="'+(s.ha.token_set?'задан, пусто = не менять':'не задан')+'"></div><div class="f"><label>Метка сущностей</label><input id="l" value="'+E(s.ha.label)+'"></div></div><button id="hs">Сохранить</button><p class="hint">Всё, что помечено этой меткой в Home Assistant, появится на экране колонки.</p>';
document.querySelectorAll('[data-d]').forEach(b=>b.onclick=async()=>{await api('/api/wifi/del',{ssid:b.dataset.d});toast('Удалено');load()});
$('#wa').onclick=async()=>{if(!$('#ws').value.trim()){toast('Введите имя сети');return}await api('/api/wifi/add',{ssid:$('#ws').value.trim(),password:$('#wp').value});toast('Сеть добавлена');load()};
$('#hs').onclick=async()=>{await api('/api/ha',{url:$('#u').value.trim(),url_ext:$('#x').value.trim(),token:$('#k').value.trim(),label:$('#l').value.trim()});toast('Сохранено');load()}}
load().catch(()=>{});
</script></body></html>)HTML";

bool Authed(httpd_req_t* req) {
    auto& srv = SettingsServer::GetInstance();
    if (srv.session().empty()) {
        return false;
    }
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len == 0 || len > 400) {
        return false;
    }
    std::string c(len + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, "Cookie", &c[0], len + 1) != ESP_OK) {
        return false;
    }
    return c.find("s=" + srv.session()) != std::string::npos;
}

bool ReadBody(httpd_req_t* req, std::string& body) {
    if (req->content_len == 0 || req->content_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return false;
    }
    body.assign(req->content_len, '\0');
    int got = 0;
    while (got < static_cast<int>(req->content_len)) {
        int r = httpd_req_recv(req, &body[got], req->content_len - got);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv");
            return false;
        }
        got += r;
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
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kPage, sizeof(kPage) - 1);
}

esp_err_t HandleLogin(httpd_req_t* req) {
    auto& srv = SettingsServer::GetInstance();
    std::string body;
    if (srv.locked()) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return SendJson(req, "{}");
    }
    if (!ReadBody(req, body)) {
        return ESP_FAIL;
    }
    if (FormField(body, "pin") != srv.pin()) {
        srv.RegisterFailure();
        httpd_resp_set_status(req, srv.locked() ? "429 Too Many Requests" : "403 Forbidden");
        return SendJson(req, "{}");
    }
    std::string cookie = "s=" + srv.NewSession() + "; Path=/; HttpOnly; SameSite=Strict";
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
    const auto& list = SsidManager::GetInstance().GetSsidList();
    for (size_t i = 0; i < list.size(); i++) {
        if (i) j += ",";
        j += "{\"ssid\":\"" + JsonEscape(list[i].ssid) + "\",\"current\":" + (list[i].ssid == current ? "true" : "false") + "}";
    }
    Settings s("ha", false);
    j += "],\"ha\":{\"url\":\"" + JsonEscape(s.GetString("url")) + "\",\"url_ext\":\"" + JsonEscape(s.GetString("url_ext")) +
         "\",\"label\":\"" + JsonEscape(s.GetString("label", "колонка")) + "\",\"token_set\":" +
         (s.GetString("token").empty() ? "false" : "true") + "}}";
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
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid");
        return ESP_FAIL;
    }
    SsidManager::GetInstance().AddSsid(ssid, FormField(body, "password"));
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
    auto& mgr = SsidManager::GetInstance();
    const auto& items = mgr.GetSsidList();
    for (size_t k = 0; k < items.size(); k++) {
        if (items[k].ssid == ssid) {
            mgr.RemoveSsid(static_cast<int>(k));
            break;
        }
    }
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

bool SettingsServer::Start() {
    if (handle_ != nullptr) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(timer_));
        esp_timer_start_once(static_cast<esp_timer_handle_t>(timer_), kAutoStopUs);
        return true;
    }
    char pin[8];
    snprintf(pin, sizeof(pin), "%04u", static_cast<unsigned>(esp_random() % 10000));
    pin_ = pin;
    session_.clear();
    failures_ = 0;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return false;
    }
    struct Route { const char* uri; httpd_method_t method; esp_err_t (*fn)(httpd_req_t*); };
    const Route routes[] = {
        {"/", HTTP_GET, HandleRoot},
        {"/api/login", HTTP_POST, HandleLogin},
        {"/api/state", HTTP_GET, HandleState},
        {"/api/wifi/add", HTTP_POST, HandleWifiAdd},
        {"/api/wifi/del", HTTP_POST, HandleWifiDel},
        {"/api/ha", HTTP_POST, HandleHa},
    };
    for (const auto& r : routes) {
        httpd_uri_t u = {.uri = r.uri, .method = r.method, .handler = r.fn, .user_ctx = nullptr};
        httpd_register_uri_handler(server, &u);
    }
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
        session_.clear();
    }
}

std::string SettingsServer::NewSession() {
    char buf[40];
    snprintf(buf, sizeof(buf), "%08x%08x", static_cast<unsigned>(esp_random()), static_cast<unsigned>(esp_random()));
    session_ = buf;
    return session_;
}
