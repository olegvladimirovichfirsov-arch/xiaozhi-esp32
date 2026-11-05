// Swipeable pages, Home Assistant tiles, settings and Wi-Fi management for ModularDisplay.
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ssid_manager.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#include "board.h"
#include "audio_codec.h"
#include "backlight.h"
#include "ha_client.h"
#include "modular_display.h"
#include "settings.h"
#include "settings_server.h"

#define TAG "ModularUI"

namespace {

constexpr uint32_t kAccent = 0xD7FF1E;
constexpr uint32_t kRowOff = 0x1C1C1E;
constexpr uint32_t kRowOn = 0x34420A;

struct RowCtx {
    ModularDisplay* self;
    HaEntity entity;
};

lv_obj_t* MakeList(lv_obj_t* parent, int w, int h, int y) {
    lv_obj_t* list = lv_obj_create(parent);
    lv_obj_set_size(list, w, h);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    return list;
}

lv_obj_t* MakeRow(lv_obj_t* list, const char* left, const char* right, uint32_t color, int h = 50) {
    lv_obj_t* btn = lv_button_create(list);
    lv_obj_set_size(btn, LV_PCT(100), h);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t* l = lv_label_create(btn);
    lv_obj_set_width(l, 130);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, left);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_user_data(btn, l);
    if (right != nullptr) {
        lv_obj_t* r = lv_label_create(btn);
        lv_obj_set_width(r, 84);
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(r, LV_LABEL_LONG_DOT);
        lv_label_set_text(r, right);
        lv_obj_set_style_text_color(r, lv_color_hex(kAccent), 0);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, -6, 0);
    }
    return btn;
}

std::string StateText(const HaEntity& e) {
    char buf[48];
    if (e.state == "unavailable" || e.state == "unknown") {
        return "недоступно";
    }
    if (e.domain == "scene") {
        return "запуск";
    }
    if (e.domain == "light") {
        if (e.state == "on" && e.brightness >= 0) {
            snprintf(buf, sizeof(buf), "%d%%", (e.brightness * 100 + 127) / 255);
            return buf;
        }
        return e.state == "on" ? "вкл" : "выкл";
    }
    if (e.domain == "climate") {
        if (e.target_temp > -999) {
            snprintf(buf, sizeof(buf), "%.0f°C", e.target_temp);
            return buf;
        }
        return e.state == "off" ? "выкл" : e.state;
    }
    if (e.state == "on") return "вкл";
    if (e.state == "off") return "выкл";
    return e.state;
}

bool IsOn(const HaEntity& e) {
    return e.state != "off" && e.state != "unavailable" && e.state != "unknown" && e.domain != "scene";
}

void HomeRowCb(lv_event_t* e) {
    auto* ctx = static_cast<RowCtx*>(lv_event_get_user_data(e));
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        delete ctx;
    } else if (code == LV_EVENT_CLICKED || code == LV_EVENT_LONG_PRESSED) {
        ctx->self->OnHomeRowEvent(ctx, code == LV_EVENT_LONG_PRESSED ? 1 : 0);
    }
}

}  // namespace

// ---- Page construction ------------------------------------------------------

void ModularDisplay::BuildTiles(lv_obj_t* screen) {
    tileview_ = lv_tileview_create(screen);
    lv_obj_set_size(tileview_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(tileview_, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tileview_, LV_SCROLLBAR_MODE_OFF);

    tile_settings_ = lv_tileview_add_tile(tileview_, 0, 0, LV_DIR_RIGHT);
    tile_main_ = lv_tileview_add_tile(tileview_, 1, 0, static_cast<lv_dir_t>(LV_DIR_LEFT | LV_DIR_RIGHT));
    tile_home_ = lv_tileview_add_tile(tileview_, 2, 0, LV_DIR_LEFT);
    for (lv_obj_t* t : {tile_settings_, tile_main_, tile_home_}) {
        lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    }
    BuildHomeTile();
    BuildSettingsTile();
    lv_tileview_set_tile_by_index(tileview_, 1, 0, LV_ANIM_OFF);
}

void ModularDisplay::BuildHomeTile() {
    lv_obj_t* title = lv_label_create(tile_home_);
    lv_label_set_text(title, "Дом");
    lv_obj_set_style_text_color(title, lv_color_hex(kAccent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    home_list_ = MakeList(tile_home_, 262, 250, 50);

    home_empty_label_ = lv_label_create(tile_home_);
    lv_obj_set_width(home_empty_label_, 230);
    lv_obj_set_style_text_align(home_empty_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(home_empty_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(home_empty_label_, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(home_empty_label_, "");
}

void ModularDisplay::BuildSettingsTile() {
    lv_obj_t* title = lv_label_create(tile_settings_);
    lv_label_set_text(title, "Настройки");
    lv_obj_set_style_text_color(title, lv_color_hex(kAccent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t* list = MakeList(tile_settings_, 262, 250, 50);

    lv_obj_t* wifi = MakeRow(list, "Wi-Fi", ">", kRowOff);
    lv_obj_add_event_cb(wifi, [](lv_event_t* e) {
        static_cast<ModularDisplay*>(lv_event_get_user_data(e))->ShowWifiPage();
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* chr = MakeRow(list, "Персонаж", character_id_.c_str(), kRowOff);
    char_row_label_ = lv_obj_get_child(chr, 1);
    lv_obj_add_event_cb(chr, [](lv_event_t* e) {
        static_cast<ModularDisplay*>(lv_event_get_user_data(e))->CycleCharacter();
    }, LV_EVENT_CLICKED, this);

    // Volume
    lv_obj_t* vol_row = lv_obj_create(list);
    lv_obj_set_size(vol_row, LV_PCT(100), 50);
    lv_obj_set_style_bg_color(vol_row, lv_color_hex(kRowOff), 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_radius(vol_row, 16, 0);
    lv_obj_set_style_pad_all(vol_row, 6, 0);
    lv_obj_remove_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* vl = lv_label_create(vol_row);
    lv_label_set_text(vl, "Громкость");
    lv_obj_align(vl, LV_ALIGN_TOP_LEFT, 4, -2);
    lv_obj_t* vs = lv_slider_create(vol_row);
    lv_obj_set_size(vs, 200, 8);
    lv_obj_align(vs, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_slider_set_range(vs, 0, 100);
    lv_slider_set_value(vs, Board::GetInstance().GetAudioCodec()->output_volume(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vs, lv_color_hex(kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vs, lv_color_hex(kAccent), LV_PART_KNOB);
    lv_obj_add_flag(vs, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(vs, [](lv_event_t* e) {
        int v = lv_slider_get_value(lv_event_get_target_obj(e));
        Board::GetInstance().GetAudioCodec()->SetOutputVolume(v);
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    // Brightness
    lv_obj_t* br_row = lv_obj_create(list);
    lv_obj_set_size(br_row, LV_PCT(100), 50);
    lv_obj_set_style_bg_color(br_row, lv_color_hex(kRowOff), 0);
    lv_obj_set_style_border_width(br_row, 0, 0);
    lv_obj_set_style_radius(br_row, 16, 0);
    lv_obj_set_style_pad_all(br_row, 6, 0);
    lv_obj_remove_flag(br_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* bl = lv_label_create(br_row);
    lv_label_set_text(bl, "Яркость");
    lv_obj_align(bl, LV_ALIGN_TOP_LEFT, 4, -2);
    lv_obj_t* bs = lv_slider_create(br_row);
    lv_obj_set_size(bs, 200, 8);
    lv_obj_align(bs, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_slider_set_range(bs, 10, 100);
    lv_slider_set_value(bs, 80, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bs, lv_color_hex(kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bs, lv_color_hex(kAccent), LV_PART_KNOB);
    lv_obj_add_event_cb(bs, [](lv_event_t* e) {
        int v = lv_slider_get_value(lv_event_get_target_obj(e));
        if (auto* backlight = Board::GetInstance().GetBacklight()) {
            backlight->SetBrightness(static_cast<uint8_t>(v), true);
        }
    }, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t* ha = MakeRow(list, "Home Assistant", "", kRowOff);
    ha_row_label_ = lv_obj_get_child(ha, 1);
    lv_obj_add_event_cb(ha, [](lv_event_t* e) {
        HaClient::GetInstance().Reconnect();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* phone = MakeRow(list, "Настроить с телефона", ">", kRowOff);
    lv_obj_add_event_cb(phone, [](lv_event_t* e) {
        static_cast<ModularDisplay*>(lv_event_get_user_data(e))->ToggleSettingsServer();
    }, LV_EVENT_CLICKED, this);

    phone_info_label_ = lv_label_create(list);
    lv_obj_set_width(phone_info_label_, LV_PCT(100));
    lv_label_set_long_mode(phone_info_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(phone_info_label_, lv_color_hex(0xB0B0B0), 0);
    lv_label_set_text(phone_info_label_, "");
}

// ---- Settings actions -------------------------------------------------------

void ModularDisplay::CycleCharacter() {
    int count = CharacterPackCount();
    int cur = 0;
    for (int i = 0; i < count; i++) {
        if (character_id_ == CharacterPackId(i)) {
            cur = i;
        }
    }
    character_id_ = CharacterPackId((cur + 1) % count);
    {
        Settings s("ui", true);
        s.SetString("character", character_id_);
    }
    character_.reset();
    character_.reset(CreateCharacterById(character_id_.c_str()));
    character_->Attach(tile_main_);
    ApplyCharacterState();
    if (char_row_label_ != nullptr) {
        lv_label_set_text(char_row_label_, character_id_.c_str());
    }
}

void ModularDisplay::ToggleSettingsServer() {
    auto& server = SettingsServer::GetInstance();
    if (server.IsRunning()) {
        server.Stop();
        lv_label_set_text(phone_info_label_, "");
        return;
    }
    std::string addr = server.Address();
    if (addr.empty()) {
        lv_label_set_text(phone_info_label_, "Нет Wi-Fi");
        return;
    }
    if (!server.Start()) {
        lv_label_set_text(phone_info_label_, "Не удалось запустить");
        return;
    }
    std::string text = "Откройте на телефоне:\n" + addr + "\nPIN: " + server.pin() +
                       "\n(10 минут, нажмите ещё раз, чтобы закрыть)";
    lv_label_set_text(phone_info_label_, text.c_str());
}

// ---- Home Assistant page ----------------------------------------------------

void ModularDisplay::OnHomeRowEvent(void* row_ctx, int event_code) {
    auto* ctx = static_cast<RowCtx*>(row_ctx);
    const HaEntity& e = ctx->entity;
    auto& ha = HaClient::GetInstance();
    if (e.domain == "scene") {
        if (event_code == 0) ha.CallService("scene", "turn_on", e.id);
    } else if (e.domain == "light") {
        if (event_code == 0) {
            ha.CallService("light", "toggle", e.id);
        } else {
            int pct = e.brightness >= 0 ? (e.brightness * 100 + 127) / 255 : 0;
            int next = pct >= 100 ? 25 : (pct / 25 + 1) * 25;
            char data[40];
            snprintf(data, sizeof(data), "{\"brightness_pct\":%d}", next);
            ha.CallService("light", "turn_on", e.id, data);
        }
    } else if (e.domain == "switch" || e.domain == "input_boolean" || e.domain == "fan") {
        if (event_code == 0) ha.CallService(e.domain, "toggle", e.id);
    } else if (e.domain == "climate") {
        if (event_code == 0) {
            ha.CallService("climate", e.state == "off" ? "turn_on" : "turn_off", e.id);
        } else if (e.target_temp > -999) {
            float next = e.target_temp + 1;
            if (next > 30) next = 16;
            char data[40];
            snprintf(data, sizeof(data), "{\"temperature\":%.0f}", next);
            ha.CallService("climate", "set_temperature", e.id, data);
        }
    }
}

void ModularDisplay::RefreshHome() {
    auto& ha = HaClient::GetInstance();
    std::vector<HaEntity> entities = ha.Snapshot();
    lv_obj_clean(home_list_);
    for (const auto& e : entities) {
        std::string st = StateText(e);
        lv_obj_t* row = MakeRow(home_list_, e.name.c_str(), st.c_str(), IsOn(e) ? kRowOn : kRowOff);
        auto* ctx = new RowCtx{this, e};
        lv_obj_add_event_cb(row, HomeRowCb, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, HomeRowCb, LV_EVENT_LONG_PRESSED, ctx);
        lv_obj_add_event_cb(row, HomeRowCb, LV_EVENT_DELETE, ctx);
    }
    if (entities.empty()) {
        lv_obj_remove_flag(home_empty_label_, LV_OBJ_FLAG_HIDDEN);
        std::string msg = ha.StatusText();
        if (!ha.IsConfigured()) {
            msg = "Home Assistant не настроен.\nНастройки > Настроить с телефона";
        }
        lv_label_set_text(home_empty_label_, msg.c_str());
    } else {
        lv_obj_add_flag(home_empty_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ModularDisplay::OnUiTimer() {
    // Runs in the LVGL task, so the lock is already held.
    auto& ha = HaClient::GetInstance();
    uint32_t v = ha.version();
    if (v != ha_version_seen_) {
        ha_version_seen_ = v;
        RefreshHome();
        if (ha_row_label_ != nullptr) {
            lv_label_set_text(ha_row_label_, ha.IsConnected() ? "OK" : "нет");
        }
    }
    offline_ = false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        offline_ = true;
    }
    ApplyCharacterState();
}

// ---- Wi-Fi page -------------------------------------------------------------

void ModularDisplay::ShowWifiPage() {
    if (wifi_page_ != nullptr) {
        return;
    }
    wifi_page_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(wifi_page_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(wifi_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wifi_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_page_, 0, 0);
    lv_obj_set_style_radius(wifi_page_, 0, 0);
    lv_obj_set_style_text_color(wifi_page_, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(wifi_page_, lv_obj_get_style_text_font(lv_screen_active(), LV_PART_MAIN), 0);
    lv_obj_remove_flag(wifi_page_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(wifi_page_);
    lv_label_set_text(title, "Wi-Fi");
    lv_obj_set_style_text_color(title, lv_color_hex(kAccent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    wifi_list_ = MakeList(wifi_page_, 262, 180, 50);

    lv_obj_t* add = lv_button_create(wifi_page_);
    lv_obj_set_size(add, 120, 40);
    lv_obj_align(add, LV_ALIGN_BOTTOM_MID, -66, -34);
    lv_obj_set_style_bg_color(add, lv_color_hex(kAccent), 0);
    lv_obj_t* al = lv_label_create(add);
    lv_label_set_text(al, "С телефона");
    lv_obj_set_style_text_color(al, lv_color_black(), 0);
    lv_obj_center(al);
    lv_obj_add_event_cb(add, [](lv_event_t* e) {
        auto* self = static_cast<ModularDisplay*>(lv_event_get_user_data(e));
        self->StartScan();
    }, LV_EVENT_CLICKED, this);

    lv_obj_t* back = lv_button_create(wifi_page_);
    lv_obj_set_size(back, 120, 40);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 66, -34);
    lv_obj_set_style_bg_color(back, lv_color_hex(kRowOff), 0);
    lv_obj_t* bl = lv_label_create(back);
    lv_label_set_text(bl, "Назад");
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t* e) {
        static_cast<ModularDisplay*>(lv_event_get_user_data(e))->CloseWifiPage();
    }, LV_EVENT_CLICKED, this);

    RebuildWifiList();
}

void ModularDisplay::CloseWifiPage() {
    if (wifi_page_ != nullptr) {
        lv_obj_delete(wifi_page_);
        wifi_page_ = wifi_list_ = wifi_form_ = nullptr;
    }
}

void ModularDisplay::RebuildWifiList() {
    if (wifi_list_ == nullptr) {
        return;
    }
    lv_obj_clean(wifi_list_);
    std::string current;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        current = reinterpret_cast<const char*>(ap.ssid);
    }
    const auto& list = SsidManager::GetInstance().GetSsidList();
    for (size_t i = 0; i < list.size(); i++) {
        bool is_current = list[i].ssid == current;
        lv_obj_t* row = MakeRow(wifi_list_, list[i].ssid.c_str(), is_current ? "OK" : "x",
                                is_current ? kRowOn : kRowOff, 44);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            auto* self = static_cast<ModularDisplay*>(lv_event_get_user_data(e));
            lv_obj_t* r = lv_event_get_target_obj(e);
            lv_obj_t* l = static_cast<lv_obj_t*>(lv_obj_get_user_data(r));
            std::string ssid = lv_label_get_text(l);
            auto& mgr = SsidManager::GetInstance();
            const auto& items = mgr.GetSsidList();
            for (size_t k = 0; k < items.size(); k++) {
                if (items[k].ssid == ssid) {
                    mgr.RemoveSsid(static_cast<int>(k));
                    break;
                }
            }
            self->RebuildWifiList();
        }, LV_EVENT_LONG_PRESSED, this);
    }
    if (list.empty()) {
        lv_obj_t* l = lv_label_create(wifi_list_);
        lv_label_set_text(l, "Нет сохранённых сетей");
    } else {
        lv_obj_t* h = lv_label_create(wifi_list_);
        lv_obj_set_style_text_color(h, lv_color_hex(0xB0B0B0), 0);
        lv_label_set_text(h, "Удержать = удалить. Добавить: с телефона");
    }
}

void ModularDisplay::StartScan() {
    // Typing on a 1.85" round touch screen is miserable, so networks are added from
    // the phone: start the web page and show its address and PIN right here.
    if (wifi_page_ == nullptr || wifi_form_ != nullptr) {
        return;
    }
    auto& srv = SettingsServer::GetInstance();
    bool ok = srv.Start();
    std::string addr = srv.Address();

    wifi_form_ = lv_obj_create(wifi_page_);
    lv_obj_set_size(wifi_form_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(wifi_form_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wifi_form_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_form_, 0, 0);
    lv_obj_set_style_radius(wifi_form_, 0, 0);
    lv_obj_remove_flag(wifi_form_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(wifi_form_);
    lv_label_set_text(title, "Добавить с телефона");
    lv_obj_set_style_text_color(title, lv_color_hex(kAccent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t* body = lv_label_create(wifi_form_);
    lv_obj_set_width(body, 250);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    std::string text;
    if (!ok || addr.empty()) {
        text = "Колонка не подключена к сети. Подключитесь к любой сети и повторите.";
    } else {
        text = "Телефон и колонка в одной сети.\nОткройте в браузере:\n" + addr + "\n\nPIN: " + srv.pin();
    }
    lv_label_set_text(body, text.c_str());
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 72);

    lv_obj_t* close = lv_button_create(wifi_form_);
    lv_obj_set_size(close, 140, 40);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_bg_color(close, lv_color_hex(kRowOff), 0);
    lv_obj_t* cl = lv_label_create(close);
    lv_label_set_text(cl, "Готово");
    lv_obj_center(cl);
    lv_obj_add_event_cb(close, [](lv_event_t* e) {
        auto* self = static_cast<ModularDisplay*>(lv_event_get_user_data(e));
        lv_obj_delete(self->wifi_form_);
        self->wifi_form_ = nullptr;
        self->RebuildWifiList();
    }, LV_EVENT_CLICKED, this);
}
