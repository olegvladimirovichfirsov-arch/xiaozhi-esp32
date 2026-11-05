// Swipeable pages, Home Assistant tiles, settings and Wi-Fi management for ModularDisplay.
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "application.h"
#include "ui_style.h"
#include "wifi_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <set>

#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "ha_client.h"
#include "modular_display.h"
#include "settings.h"
#include "settings_server.h"

#define TAG "ModularUI"

namespace {

constexpr uint32_t kAccent = ModularStyle::kAccent;
constexpr uint32_t kRowOff = ModularStyle::kSurface;
constexpr uint32_t kRowOn = ModularStyle::kActive;

struct RowCtx {
    ModularDisplay* self;
    HaEntity entity;
    bool long_pressed = false;
};

const char* CharacterName(const std::string& id) {
    if (id == "ghost_cat")
        return "Кот-призрак";
    if (id == "gradient")
        return "Градиент";
    return "Фигура";
}

lv_obj_t* MakeList(lv_obj_t* parent, int w, int h, int y) {
    lv_obj_t* list = lv_obj_create(parent);
    lv_obj_set_size(list, w, h);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(list, &ui_font_20, 0);
    lv_obj_set_style_text_color(list, lv_color_hex(ModularStyle::kText), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_style_pad_all(list, 2, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(list, ModularStyle::ScrollEasing, LV_EVENT_SCROLL_BEGIN, nullptr);
    return list;
}

lv_obj_t* MakeRow(lv_obj_t* list, const char* left, const char* right, uint32_t color, int h = 50) {
    lv_obj_t* btn = lv_button_create(list);
    lv_obj_set_size(btn, LV_PCT(100), h);
    lv_obj_set_style_radius(btn, ModularStyle::kRadius, 0);
    lv_obj_set_style_pad_hor(btn, 12, 0);
    lv_obj_set_style_pad_ver(btn, 8, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    ModularStyle::PressFeedback(btn);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t* l = lv_label_create(btn);
    const bool stacked = h >= 68 && right != nullptr;
    lv_obj_set_width(l, stacked || right == nullptr ? 216 : (strcmp(right, ">") == 0 ? 192 : 140));
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_height(l, 26);
    lv_label_set_text(l, left);
    lv_obj_align(l, stacked ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_user_data(btn, l);
    if (right != nullptr) {
        lv_obj_t* r = lv_label_create(btn);
        lv_obj_set_width(r, stacked ? 216 : (strcmp(right, ">") == 0 ? 18 : 68));
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(r, LV_LABEL_LONG_DOT);
        lv_obj_set_height(r, 26);
        lv_label_set_text(r, right);
        lv_obj_set_style_text_color(r, lv_color_hex(kAccent), 0);
        lv_obj_align(r, stacked ? LV_ALIGN_BOTTOM_RIGHT : LV_ALIGN_RIGHT_MID, 0, 0);
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
            snprintf(buf, sizeof(buf), "%.1f°C", e.target_temp);
            return buf;
        }
        return e.state == "off" ? "выкл" : e.state;
    }
    if (e.state == "on")
        return "вкл";
    if (e.state == "off")
        return "выкл";
    return e.state;
}

bool IsOn(const HaEntity& e) {
    return e.state != "off" && e.state != "unavailable" && e.state != "unknown" &&
           e.domain != "scene";
}

void HomeRowCb(lv_event_t* e) {
    auto* ctx = static_cast<RowCtx*>(lv_event_get_user_data(e));
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        delete ctx;
    } else if (code == LV_EVENT_PRESSED) {
        ctx->long_pressed = false;
    } else if (code == LV_EVENT_LONG_PRESSED) {
        ctx->long_pressed = true;
        ctx->self->OnHomeRowEvent(ctx, 1);
    } else if (code == LV_EVENT_CLICKED && !ctx->long_pressed) {
        ctx->self->OnHomeRowEvent(ctx, 0);
    }
}

}  // namespace

// ---- Page construction ------------------------------------------------------

void ModularDisplay::BuildTiles(lv_obj_t* screen) {
    tileview_ = lv_tileview_create(screen);
    lv_obj_set_size(tileview_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(tileview_, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tileview_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(tileview_, ModularStyle::ScrollEasing, LV_EVENT_SCROLL_BEGIN, nullptr);
    lv_obj_add_event_cb(
        tileview_,
        [](lv_event_t* event) {
            auto* self = static_cast<ModularDisplay*>(lv_event_get_user_data(event));
            if (lv_event_get_target_obj(event) == self->tileview_) {
                // Finish a swipe with one complete invalidation of the transparent
                // tileview; avoid stale glyph fragments at partial-render edges.
                lv_obj_invalidate(self->tileview_);
            }
        },
        LV_EVENT_SCROLL_END, this);

    tile_settings_ = lv_tileview_add_tile(tileview_, 0, 0, LV_DIR_RIGHT);
    tile_main_ =
        lv_tileview_add_tile(tileview_, 1, 0, static_cast<lv_dir_t>(LV_DIR_LEFT | LV_DIR_RIGHT));
    tile_home_ = lv_tileview_add_tile(tileview_, 2, 0, LV_DIR_LEFT);
    for (lv_obj_t* t : {tile_settings_, tile_main_, tile_home_}) {
        lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    }
    BuildHomeTile();
    BuildSettingsTile();
    lv_tileview_set_tile_by_index(tileview_, 1, 0, LV_ANIM_OFF);
}

void ModularDisplay::BuildHomeTile() {
    lv_obj_t* title = lv_label_create(tile_home_);
    lv_label_set_text(title, "Дом");
    lv_obj_set_style_text_font(title, &ui_font_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(kAccent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    home_list_ = MakeList(tile_home_, ModularStyle::kListWidth, ModularStyle::kListHeight,
                          ModularStyle::kListTop);

    home_empty_label_ = lv_label_create(tile_home_);
    lv_obj_set_width(home_empty_label_, 230);
    lv_obj_set_style_text_align(home_empty_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(home_empty_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(home_empty_label_, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(home_empty_label_, "Загрузка HA…");
#if LV_USE_SPINNER
    home_spinner_ = lv_spinner_create(tile_home_);
    lv_obj_set_size(home_spinner_, 32, 32);
    lv_obj_align(home_spinner_, LV_ALIGN_CENTER, 0, -56);
    lv_spinner_set_anim_params(home_spinner_, 1000, 80);
    lv_obj_add_flag(home_spinner_, LV_OBJ_FLAG_HIDDEN);
#endif
    home_connection_label_ = lv_label_create(tile_home_);
    lv_obj_set_width(home_connection_label_, 180);
    lv_obj_set_style_text_align(home_connection_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(home_connection_label_, lv_color_hex(ModularStyle::kSecondary), 0);
    lv_label_set_long_mode(home_connection_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(home_connection_label_, "");
    lv_obj_align(home_connection_label_, LV_ALIGN_BOTTOM_MID, 0, -30);
}

void ModularDisplay::BuildSettingsTile() {
    lv_obj_t* title = lv_label_create(tile_settings_);
    lv_label_set_text(title, "Настройки");
    lv_obj_set_style_text_font(title, &ui_font_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(kAccent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t* list = MakeList(tile_settings_, ModularStyle::kListWidth, ModularStyle::kListHeight,
                              ModularStyle::kListTop);

    lv_obj_t* wifi = MakeRow(list, "Wi-Fi", ">", kRowOff);
    lv_obj_add_event_cb(
        wifi,
        [](lv_event_t* e) {
            static_cast<ModularDisplay*>(lv_event_get_user_data(e))->ShowWifiPage();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* chr = MakeRow(list, "Персонаж", CharacterName(character_id_), kRowOff, 72);
    char_row_label_ = lv_obj_get_child(chr, 1);
    lv_obj_add_event_cb(
        chr,
        [](lv_event_t* e) {
            static_cast<ModularDisplay*>(lv_event_get_user_data(e))->CycleCharacter();
        },
        LV_EVENT_CLICKED, this);

    // Volume
    lv_obj_t* vol_row = lv_obj_create(list);
    lv_obj_set_size(vol_row, LV_PCT(100), 90);
    lv_obj_set_style_bg_color(vol_row, lv_color_hex(kRowOff), 0);
    lv_obj_set_style_text_color(vol_row, lv_color_hex(ModularStyle::kText), 0);
    lv_obj_set_style_text_font(vol_row, &ui_font_20, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_radius(vol_row, 16, 0);
    lv_obj_set_style_pad_all(vol_row, 6, 0);
    lv_obj_remove_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* vl = lv_label_create(vol_row);
    lv_label_set_text(vl, "Громкость");
    lv_obj_align(vl, LV_ALIGN_TOP_LEFT, 4, -2);
    lv_obj_t* vs = lv_slider_create(vol_row);
    lv_obj_set_size(vs, 196, 8);
    lv_obj_set_ext_click_area(vs, 18);
    lv_obj_align(vs, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_slider_set_range(vs, 0, 80);  // above ~80% the small speaker distorts
    auto* codec = Board::GetInstance().GetAudioCodec();
    lv_slider_set_value(vs, codec ? std::min(codec->output_volume(), 80) : 0, LV_ANIM_OFF);
    if (!codec)
        lv_obj_add_state(vs, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(vs, lv_color_hex(kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vs, lv_color_hex(kAccent), LV_PART_KNOB);
    lv_obj_add_flag(vs, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(
        vs,
        [](lv_event_t* e) {
            int v = lv_slider_get_value(lv_event_get_target_obj(e));
            Application::GetInstance().Schedule([v]() {
                if (auto* codec = Board::GetInstance().GetAudioCodec())
                    codec->SetOutputVolume(v);
            });
        },
        LV_EVENT_RELEASED, nullptr);

    // Brightness
    lv_obj_t* br_row = lv_obj_create(list);
    lv_obj_set_size(br_row, LV_PCT(100), 90);
    lv_obj_set_style_bg_color(br_row, lv_color_hex(kRowOff), 0);
    lv_obj_set_style_text_color(br_row, lv_color_hex(ModularStyle::kText), 0);
    lv_obj_set_style_text_font(br_row, &ui_font_20, 0);
    lv_obj_set_style_border_width(br_row, 0, 0);
    lv_obj_set_style_radius(br_row, 16, 0);
    lv_obj_set_style_pad_all(br_row, 6, 0);
    lv_obj_remove_flag(br_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* bl = lv_label_create(br_row);
    lv_label_set_text(bl, "Яркость");
    lv_obj_align(bl, LV_ALIGN_TOP_LEFT, 4, -2);
    lv_obj_t* bs = lv_slider_create(br_row);
    lv_obj_set_size(bs, 196, 8);
    lv_obj_set_ext_click_area(bs, 18);
    lv_obj_align(bs, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_slider_set_range(bs, 10, 100);
    Settings display_settings("display", false);
    lv_slider_set_value(bs, display_settings.GetInt("brightness", 75), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bs, lv_color_hex(kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bs, lv_color_hex(kAccent), LV_PART_KNOB);
    lv_obj_add_event_cb(
        bs,
        [](lv_event_t* e) {
            int v = lv_slider_get_value(lv_event_get_target_obj(e));
            Application::GetInstance().Schedule([v]() {
                if (auto* backlight = Board::GetInstance().GetBacklight()) {
                    backlight->SetBrightness(static_cast<uint8_t>(v), true);
                }
            });
        },
        LV_EVENT_RELEASED, nullptr);

    lv_obj_t* ha = MakeRow(list, "Умный дом", "", kRowOff);
    ha_row_label_ = lv_obj_get_child(ha, 1);
    lv_obj_add_event_cb(
        ha, [](lv_event_t*) { HaClient::GetInstance().Reconnect(); }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* phone = MakeRow(list, "С телефона", ">", kRowOff);
    lv_obj_add_event_cb(
        phone,
        [](lv_event_t* e) {
            static_cast<ModularDisplay*>(lv_event_get_user_data(e))->ToggleSettingsServer();
        },
        LV_EVENT_CLICKED, this);

    phone_info_label_ = lv_label_create(list);
    lv_obj_set_width(phone_info_label_, LV_PCT(100));
    lv_label_set_long_mode(phone_info_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(phone_info_label_, lv_color_hex(0xB0B0B0), 0);
    lv_label_set_text(phone_info_label_, "");
}

// ---- Settings actions -------------------------------------------------------

void ModularDisplay::CycleCharacter() {
    int count = CharacterPackCount();
    if (count <= 0)
        return;
    int cur = 0;
    for (int i = 0; i < count; i++) {
        if (character_id_ == CharacterPackId(i)) {
            cur = i;
        }
    }
    const std::string id = CharacterPackId((cur + 1) % count);
    std::unique_ptr<Character> next(CreateCharacterById(id.c_str()));
    if (!next)
        return;
    character_id_ = id;
    Application::GetInstance().Schedule([id]() {
        nvs_handle_t h;
        if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
            if (nvs_set_str(h, "character", id.c_str()) == ESP_OK)
                nvs_commit(h);
            nvs_close(h);
        }
    });
    character_ = std::move(next);
    character_->Attach(tile_main_);
    has_character_state_ = false;
    lv_obj_move_foreground(modular_status_label_);
    lv_obj_move_foreground(subtitle_label_);
    ApplyCharacterState();
    if (char_row_label_ != nullptr) {
        lv_label_set_text(char_row_label_, CharacterName(character_id_));
    }
}

void ModularDisplay::ToggleSettingsServer() {
    auto& server = SettingsServer::GetInstance();
    if (server.IsRequested()) {
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
    lv_label_set_text(phone_info_label_, "Запуск…");
}

// ---- Home Assistant page ----------------------------------------------------

void ModularDisplay::OnHomeRowEvent(void* row_ctx, int event_code) {
    auto* ctx = static_cast<RowCtx*>(row_ctx);
    const HaEntity& e = ctx->entity;
    if (e.state == "unavailable" || e.state == "unknown")
        return;
    auto& ha = HaClient::GetInstance();
    if (e.domain == "scene") {
        if (event_code == 0)
            ha.CallService("scene", "turn_on", e.id);
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
        if (event_code == 0)
            ha.CallService(e.domain, "toggle", e.id);
    } else if (e.domain == "climate") {
        if (event_code == 0) {
            ha.CallService("climate", e.state == "off" ? "turn_on" : "turn_off", e.id);
        } else if (e.target_temp > -999 && e.min_temp > -999 && e.max_temp >= e.min_temp &&
                   e.temp_step > 0) {
            float next = e.target_temp + e.temp_step;
            if (next > e.max_temp)
                next = e.min_temp;
            next = std::clamp(next, e.min_temp, e.max_temp);
            char data[40];
            snprintf(data, sizeof(data), "{\"temperature\":%.2f}", next);
            ha.CallService("climate", "set_temperature", e.id, data);
        }
    }
}

void ModularDisplay::RefreshHome() {
    auto& ha = HaClient::GetInstance();
    std::vector<HaEntity> entities = ha.Snapshot();
    std::vector<std::string> order;
    for (const auto& entity : entities)
        order.push_back(entity.id);
    if (order != home_order_) {
        const int32_t scroll = lv_obj_get_scroll_y(home_list_);
        lv_obj_clean(home_list_);
        home_rows_.clear();
        home_order_ = order;
        for (const auto& entity : entities) {
            const auto state = StateText(entity);
            auto* row = MakeRow(home_list_, entity.name.c_str(), state.c_str(),
                                IsOn(entity) ? kRowOn : kRowOff, 72);
            auto* ctx = new (std::nothrow) RowCtx{this, entity};
            if (!ctx) {
                lv_obj_delete(row);
                lv_obj_clean(home_list_);
                home_rows_.clear();
                home_order_.clear();
                lv_label_set_text(home_empty_label_, "Мало памяти");
                lv_obj_remove_flag(home_empty_label_, LV_OBJ_FLAG_HIDDEN);
                return;
            }
            lv_obj_set_user_data(row, ctx);
            lv_obj_add_event_cb(row, HomeRowCb, LV_EVENT_PRESSED, ctx);
            lv_obj_add_event_cb(row, HomeRowCb, LV_EVENT_CLICKED, ctx);
            lv_obj_add_event_cb(row, HomeRowCb, LV_EVENT_LONG_PRESSED, ctx);
            lv_obj_add_event_cb(row, HomeRowCb, LV_EVENT_DELETE, ctx);
            home_rows_[entity.id] = row;
        }
        lv_obj_update_layout(home_list_);
        lv_obj_scroll_to_y(home_list_, scroll, LV_ANIM_OFF);
    } else {
        for (const auto& entity : entities) {
            auto* row = home_rows_.at(entity.id);
            auto* ctx = static_cast<RowCtx*>(lv_obj_get_user_data(row));
            if (ctx->entity.name != entity.name) {
                lv_label_set_text(lv_obj_get_child(row, 0), entity.name.c_str());
            }
            const auto state = StateText(entity);
            if (StateText(ctx->entity) != state) {
                lv_label_set_text(lv_obj_get_child(row, 1), state.c_str());
            }
            if (IsOn(ctx->entity) != IsOn(entity)) {
                lv_obj_set_style_bg_color(row, lv_color_hex(IsOn(entity) ? kRowOn : kRowOff), 0);
            }
            ctx->entity = entity;
        }
    }
    for (const auto& entity : entities) {
        auto* row = home_rows_.at(entity.id);
        const bool disabled =
            !ha.IsConnected() || entity.state == "unavailable" || entity.state == "unknown";
        if (disabled)
            lv_obj_add_state(row, LV_STATE_DISABLED);
        else
            lv_obj_remove_state(row, LV_STATE_DISABLED);
    }
    if (home_spinner_)
        lv_obj_add_flag(home_spinner_, LV_OBJ_FLAG_HIDDEN);
    if (entities.empty()) {
        lv_obj_remove_flag(home_empty_label_, LV_OBJ_FLAG_HIDDEN);
        std::string msg = ha.StatusText();
        if (!ha.IsConfigured()) {
            msg = "Home Assistant не настроен.\nНастройки > Настроить с телефона";
        }
        lv_label_set_text(home_empty_label_, msg.c_str());
        if (home_spinner_ && msg.find("подключаюсь") != std::string::npos &&
            lv_tileview_get_tile_active(tileview_) == tile_home_) {
            lv_obj_remove_flag(home_spinner_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(home_empty_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ModularDisplay::OnUiTimer() {
    // LVGL task owns all widgets. Work performed here is bounded.
    FlushPendingUi();
    auto* active_tile = lv_tileview_get_tile_active(tileview_);
    if (active_tile != active_tile_seen_) {
        active_tile_seen_ = active_tile;
        ha_version_seen_ = 0xFFFFFFFF;
        if (home_spinner_ && active_tile != tile_home_)
            lv_obj_add_flag(home_spinner_, LV_OBJ_FLAG_HIDDEN);
    }
    const bool wake = wake_requested_.exchange(false);
    if (wake)
        lv_display_trigger_activity(display_);
    const bool dim = allow_dim_ && !SettingsServer::GetInstance().IsRequested() &&
                     lv_display_get_inactive_time(display_) >= 60000;
    if (dim != dimmed_) {
        dimmed_ = dim;
        if (auto* backlight = Board::GetInstance().GetBacklight()) {
            if (dim)
                backlight->SetBrightness(15);
            else
                backlight->RestoreBrightness();
        }
    }
    if (notification_duration_ && lv_tick_elaps(notification_started_) >= notification_duration_) {
        notification_duration_ = 0;
        lv_label_set_text(modular_status_label_, status_text_.c_str());
    }
    ApplyCharacterState();
    if (character_)
        character_->SetActive(!dimmed_ && !wifi_page_ &&
                              lv_tileview_get_tile_active(tileview_) == tile_main_);
    if (lv_tick_elaps(maintenance_at_) < 500)
        return;
    maintenance_at_ = lv_tick_get();
    auto& ha = HaClient::GetInstance();
    const uint32_t version = ha.version();
    bool pressed = false;
    for (auto* indev = lv_indev_get_next(nullptr); indev; indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED)
            pressed = true;
    }
    if (wifi_page_ && !pressed && ModularWifi::Version().load() != wifi_version_seen_) {
        RebuildWifiList();
    }
    if (version != ha_version_seen_ && !pressed) {
        ha_version_seen_ = version;
        RefreshHome();
        const auto connection_status = ha.StatusText();
        lv_label_set_text(home_connection_label_,
                          connection_status == "подключено" ? "" : connection_status.c_str());
        if (ha_row_label_)
            lv_label_set_text(ha_row_label_, ha.IsConnected() ? "OK" : "нет");
    }
    wifi_ap_record_t ap = {};
    offline_ = esp_wifi_sta_get_ap_info(&ap) != ESP_OK;
    ApplyCharacterState();
    if (character_)
        character_->SetActive(!dimmed_ && !wifi_page_ &&
                              lv_tileview_get_tile_active(tileview_) == tile_main_);
    auto& server = SettingsServer::GetInstance();
    std::string phone;
    if (server.IsRunning())
        phone = server.Address() + "\nPIN: " + server.pin() + "\nДоступ: 10 минут";
    else if (server.IsRequested())
        phone = "Запуск…";
    else if (offline_)
        phone = "Нет Wi-Fi. Подключите колонку к сети.";
    else
        phone = "Настройка с телефона закрыта";
    if (phone != phone_text_) {
        phone_text_ = phone;
        if (phone_info_label_)
            lv_label_set_text(phone_info_label_, phone.c_str());
        if (wifi_info_label_)
            lv_label_set_text(wifi_info_label_, phone.c_str());
    }
}

// ---- Wi-Fi page -------------------------------------------------------------

void ModularDisplay::ShowWifiPage() {
    if (wifi_page_ != nullptr) {
        return;
    }
    wifi_page_ = lv_obj_create(lv_display_get_layer_top(display_));
    lv_obj_set_size(wifi_page_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(wifi_page_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wifi_page_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_page_, 0, 0);
    lv_obj_set_style_radius(wifi_page_, 0, 0);
    lv_obj_set_style_pad_all(wifi_page_, 0, 0);
    lv_obj_set_style_text_color(wifi_page_, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(
        wifi_page_,
        lv_obj_get_style_text_font(lv_display_get_screen_active(display_), LV_PART_MAIN), 0);
    lv_obj_remove_flag(wifi_page_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(wifi_page_);
    lv_label_set_text(title, "Wi-Fi");
    lv_obj_set_style_text_color(title, lv_color_hex(kAccent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    wifi_list_ = MakeList(wifi_page_, 248, 180, 68);

    lv_obj_t* add = lv_button_create(wifi_page_);
    lv_obj_set_size(add, 120, 48);
    lv_obj_align(add, LV_ALIGN_BOTTOM_MID, -56, -44);
    lv_obj_set_style_bg_color(add, lv_color_hex(kAccent), 0);
    lv_obj_t* al = lv_label_create(add);
    lv_label_set_text(al, "Добавить");
    lv_obj_set_style_text_color(al, lv_color_black(), 0);
    lv_obj_center(al);
    lv_obj_add_event_cb(
        add,
        [](lv_event_t* e) {
            auto* self = static_cast<ModularDisplay*>(lv_event_get_user_data(e));
            self->StartScan();
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* back = lv_button_create(wifi_page_);
    lv_obj_set_size(back, 100, 48);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 64, -44);
    lv_obj_set_style_bg_color(back, lv_color_hex(kRowOff), 0);
    lv_obj_t* bl = lv_label_create(back);
    lv_label_set_text(bl, "Назад");
    lv_obj_center(bl);
    lv_obj_add_event_cb(
        back,
        [](lv_event_t* e) {
            static_cast<ModularDisplay*>(lv_event_get_user_data(e))->CloseWifiPage();
        },
        LV_EVENT_CLICKED, this);

    ModularStyle::PressFeedback(add);
    ModularStyle::PressFeedback(back);
    ModularStyle::Fade(wifi_page_);
    RebuildWifiList();
}

void ModularDisplay::CloseWifiPage() {
    if (wifi_page_ != nullptr) {
        lv_obj_delete(wifi_page_);
        wifi_page_ = wifi_list_ = wifi_form_ = wifi_info_label_ = nullptr;
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
    wifi_version_seen_ = ModularWifi::Version().load();
    const auto list = ModularWifi::Snapshot();
    for (size_t i = 0; i < list.size(); i++) {
        bool is_current = list[i].ssid == current;
        lv_obj_t* row = MakeRow(wifi_list_, list[i].ssid.c_str(), is_current ? "OK" : "x",
                                is_current ? kRowOn : kRowOff, 44);
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                lv_obj_t* r = lv_event_get_target_obj(e);
                lv_obj_t* l = static_cast<lv_obj_t*>(lv_obj_get_user_data(r));
                std::string ssid = lv_label_get_text(l);
                Application::GetInstance().Schedule([ssid]() {
                    const bool removed = ModularWifi::Remove(ssid);
                    if (auto* display = Board::GetInstance().GetDisplay()) {
                        display->ShowNotification(removed ? "Сеть Wi-Fi удалена"
                                                          : "Сеть не найдена");
                    }
                });
            },
            LV_EVENT_LONG_PRESSED, this);
    }
    if (list.empty()) {
        lv_obj_t* l = lv_label_create(wifi_list_);
        lv_label_set_text(l, "Нет сохранённых сетей");
    } else {
        lv_obj_t* h = lv_label_create(wifi_list_);
        lv_obj_set_style_text_color(h, lv_color_hex(0xB0B0B0), 0);
        lv_obj_set_width(h, 216);
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
        lv_label_set_text(h, "Удержать: удалить. Добавить: с телефона.");
    }
}

void ModularDisplay::StartScan() {
    // Typing on a 1.85" round touch screen is miserable, so networks are added from
    // the phone: start the web page and show its address and PIN right here.
    if (wifi_page_ == nullptr || wifi_form_ != nullptr) {
        return;
    }
    auto& srv = SettingsServer::GetInstance();
    bool ok = !srv.Address().empty() && srv.Start();
    std::string addr = srv.Address();

    wifi_form_ = lv_obj_create(wifi_page_);
    lv_obj_set_size(wifi_form_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(wifi_form_, lv_color_black(), 0);
    lv_obj_set_style_text_font(wifi_form_, &ui_font_20, 0);
    lv_obj_set_style_text_color(wifi_form_, lv_color_hex(ModularStyle::kText), 0);
    lv_obj_set_style_bg_opa(wifi_form_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_form_, 0, 0);
    lv_obj_set_style_radius(wifi_form_, 0, 0);
    lv_obj_set_style_pad_all(wifi_form_, 0, 0);
    lv_obj_remove_flag(wifi_form_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(wifi_form_);
    lv_label_set_text(title, "С телефона");
    lv_obj_set_style_text_color(title, lv_color_hex(kAccent), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t* body = lv_label_create(wifi_form_);
    wifi_info_label_ = body;
    phone_text_.clear();
    lv_obj_set_width(body, 220);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    std::string text;
    if (!ok || addr.empty()) {
        text = "Колонка не подключена к сети. Подключитесь к любой сети и повторите.";
    } else {
        text = "Запуск настройки…";
    }
    lv_label_set_text(body, text.c_str());
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 72);

    lv_obj_t* close = lv_button_create(wifi_form_);
    ModularStyle::PressFeedback(close);
    lv_obj_set_size(close, 140, 48);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_bg_color(close, lv_color_hex(kRowOff), 0);
    lv_obj_t* cl = lv_label_create(close);
    lv_label_set_text(cl, "Готово");
    lv_obj_center(cl);
    lv_obj_add_event_cb(
        close,
        [](lv_event_t* e) {
            auto* self = static_cast<ModularDisplay*>(lv_event_get_user_data(e));
            lv_obj_delete(self->wifi_form_);
            self->wifi_form_ = self->wifi_info_label_ = nullptr;
            self->RebuildWifiList();
        },
        LV_EVENT_CLICKED, this);
    ModularStyle::Fade(wifi_form_);
}
