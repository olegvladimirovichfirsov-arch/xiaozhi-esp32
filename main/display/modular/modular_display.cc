#include "modular_display.h"

#include <esp_log.h>

#include <cstring>

#include "assets/lang_config.h"
#include "lvgl_theme.h"
#include "settings.h"

#define TAG "ModularDisplay"

ModularDisplay::~ModularDisplay() {
    DisplayLockGuard lock(this);
    if (ui_timer_ != nullptr) {
        lv_timer_delete(ui_timer_);
        ui_timer_ = nullptr;
    }
    // Character widgets are children of the active screen; release our handle
    // before the base class tears the screen down.
    character_.reset();
}

void ModularDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    Display::SetupUI();  // marks SetupUI as called
    DisplayLockGuard lock(this);

    auto* theme = static_cast<LvglTheme*>(current_theme_);
    auto* text_font = theme->text_font()->font();

    lv_obj_t* screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xE6E6E6), 0);

    BuildTiles(screen);

    Settings ui_settings("ui", false);
    character_id_ = ui_settings.GetString("character", "ghost_cat");
    character_.reset(CreateCharacterById(character_id_.c_str()));
    character_->Attach(tile_main_);

    modular_status_label_ = lv_label_create(tile_main_);
    lv_obj_set_width(modular_status_label_, 220);
    lv_obj_set_style_text_align(modular_status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(modular_status_label_, lv_color_hex(0xD7FF1E), 0);
    lv_label_set_long_mode(modular_status_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(modular_status_label_, "");
    lv_obj_align(modular_status_label_, LV_ALIGN_TOP_MID, 0, 26);

    subtitle_label_ = lv_label_create(tile_main_);
    lv_obj_set_width(subtitle_label_, 230);
    lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(subtitle_label_, "");
    lv_obj_align(subtitle_label_, LV_ALIGN_BOTTOM_MID, 0, -30);

    ui_timer_ = lv_timer_create(
        [](lv_timer_t* t) {
            static_cast<ModularDisplay*>(lv_timer_get_user_data(t))->OnUiTimer();
        },
        500, this);

    ApplyCharacterState();
}

void ModularDisplay::ApplyCharacterState() {
    if (!character_) {
        return;
    }
    CharacterState state = emotion_state_;
    if (offline_) {
        state = CharacterState::kOffline;
    } else if (status_kind_ == StatusKind::kListening) {
        state = CharacterState::kListening;
    } else if (status_kind_ == StatusKind::kSpeaking) {
        state = CharacterState::kSpeaking;
    }
    character_->SetState(state);
}

void ModularDisplay::SetStatus(const char* status) {
    if (status == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (modular_status_label_ == nullptr) {
        return;
    }
    lv_label_set_text(modular_status_label_, status);

    if (strcmp(status, Lang::Strings::LISTENING) == 0) {
        status_kind_ = StatusKind::kListening;
    } else if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
        status_kind_ = StatusKind::kSpeaking;
    } else if (strcmp(status, Lang::Strings::STANDBY) == 0) {
        status_kind_ = StatusKind::kIdle;
    } else {
        status_kind_ = StatusKind::kOther;
    }
    ApplyCharacterState();
}

void ModularDisplay::ShowNotification(const char* notification, int duration_ms) {
    (void)duration_ms;
    if (notification == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (modular_status_label_ != nullptr) {
        lv_label_set_text(modular_status_label_, notification);
    }
}

void ModularDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void ModularDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    emotion_state_ = CharacterStateFromEmotion(emotion);
    ApplyCharacterState();
}

void ModularDisplay::SetChatMessage(const char* role, const char* content) {
    (void)role;
    DisplayLockGuard lock(this);
    last_subtitle_ = content != nullptr ? content : "";
    if (subtitle_label_ != nullptr) {
        lv_label_set_text(subtitle_label_, last_subtitle_.c_str());
    }
}

void ModularDisplay::RemoveChatMessage(const char* role, const char* content) {
    (void)role;
    DisplayLockGuard lock(this);
    if (content != nullptr && last_subtitle_ == content) {
        last_subtitle_.clear();
        if (subtitle_label_ != nullptr) {
            lv_label_set_text(subtitle_label_, "");
        }
    }
}

void ModularDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    last_subtitle_.clear();
    if (subtitle_label_ != nullptr) {
        lv_label_set_text(subtitle_label_, "");
    }
}

void ModularDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    (void)image;  // no camera preview on this UI
}

void ModularDisplay::SetTheme(Theme* theme) {
    if (theme != nullptr) {
        current_theme_ = theme;
    }
}

void ModularDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    if (subtitle_label_ == nullptr) {
        return;
    }
    if (hide) {
        lv_obj_add_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ModularDisplay::UpdateStatusBar(bool update_all) {
    (void)update_all;  // status bar icons are not part of this layout yet
}
