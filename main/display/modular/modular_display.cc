#include "modular_display.h"

#include <esp_log.h>

#include <cstring>

#include "assets/lang_config.h"
#include "ha_client.h"
#include "lvgl_theme.h"
#include "settings.h"
#include "ui_style.h"

#define TAG "ModularDisplay"

ModularDisplay::~ModularDisplay() {
    UiLock lock(this, 0);  // teardown must join LVGL before freeing callbacks
    if (ui_timer_ != nullptr) {
        lv_timer_delete(ui_timer_);
        ui_timer_ = nullptr;
    }
    // Character widgets are children of the active screen; release our handle
    // before the base class tears the screen down.
    character_.reset();
    CloseWifiPage();
    if (tileview_)
        lv_obj_delete(tileview_);
}

LV_FONT_DECLARE(ui_font_20);
LV_FONT_DECLARE(ui_font_28);

void ModularDisplay::SetupUI() {
    UiLock lock(this, 5000);
    if (!lock || setup_ui_called_ || display_ == nullptr)
        return;
    Display::SetupUI();
    Settings ui_settings("ui", false);
    character_id_ = ui_settings.GetString("character", "ghost_cat");
    const lv_font_t* text_font = &ui_font_20;  // Latin + Cyrillic

    lv_obj_t* screen = lv_display_get_screen_active(display_);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(ModularStyle::kText), 0);

    BuildTiles(screen);

    character_.reset(CreateCharacterById(character_id_.c_str()));
    if (character_)
        character_->Attach(tile_main_);

    modular_status_label_ = lv_label_create(tile_main_);
    lv_obj_set_width(modular_status_label_, 180);
    lv_obj_set_style_text_align(modular_status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(modular_status_label_, lv_color_hex(0xD7FF1E), 0);
    lv_label_set_long_mode(modular_status_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(modular_status_label_, "");
    lv_obj_align(modular_status_label_, LV_ALIGN_TOP_MID, 0, 30);

    subtitle_label_ = lv_label_create(tile_main_);
    lv_obj_set_width(subtitle_label_, 200);
    lv_obj_set_height(subtitle_label_, 26);
    lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(subtitle_label_, "");
    lv_obj_align(subtitle_label_, LV_ALIGN_BOTTOM_MID, 0, -34);

    ui_timer_ = lv_timer_create(
        [](lv_timer_t* t) { static_cast<ModularDisplay*>(lv_timer_get_user_data(t))->OnUiTimer(); },
        50, this);

    if (!ui_timer_)
        ESP_LOGE(TAG, "UI timer allocation failed");
    ApplyCharacterState();
    HaClient::GetInstance().Start();
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
    if (dimmed_)
        state = CharacterState::kSleep;
    if (!has_character_state_ || applied_state_ != state) {
        applied_state_ = state;
        has_character_state_ = true;
        character_->SetState(state);
    }
}

// Public setters may run on application/network tasks. Keep only the latest
// value, then apply it on LVGL. No display wait and no unbounded event queue.
void ModularDisplay::SetStatus(const char* status) {
    if (!status)
        return;
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.status = status;
    pending_.flags |= kStatus;
    wake_requested_ = true;
}

void ModularDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!notification)
        return;
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.notice = notification;
    pending_.notice_duration = duration_ms > 0 ? static_cast<uint32_t>(duration_ms) : 1;
    pending_.flags |= kNotice;
    wake_requested_ = true;
}

void ModularDisplay::ShowNotification(const std::string& text, int duration_ms) {
    ShowNotification(text.c_str(), duration_ms);
}

void ModularDisplay::SetEmotion(const char* emotion) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.emotion = CharacterStateFromEmotion(emotion);
    pending_.flags |= kEmotion;
}

void ModularDisplay::SetChatMessage(const char* role, const char* content) {
    (void)role;
    std::lock_guard<std::mutex> lock(pending_mutex_);
    last_subtitle_ = content ? content : "";
    pending_.flags |= kChat;
}

void ModularDisplay::RemoveChatMessage(const char* role, const char* content) {
    (void)role;
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (content && last_subtitle_ == content) {
        last_subtitle_.clear();
        pending_.flags |= kChat;
    }
}

void ModularDisplay::ClearChatMessages() {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    last_subtitle_.clear();
    pending_.flags |= kChat;
}

void ModularDisplay::FlushPendingUi() {
    PendingUi update;
    std::string subtitle;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (!pending_.flags)
            return;
        update = pending_;
        if (update.flags & kChat)
            subtitle = last_subtitle_;
        pending_.flags = 0;
    }
    if (update.flags & kStatus) {
        status_text_ = update.status;
        if (update.status == Lang::Strings::LISTENING)
            status_kind_ = StatusKind::kListening;
        else if (update.status == Lang::Strings::SPEAKING)
            status_kind_ = StatusKind::kSpeaking;
        else if (update.status == Lang::Strings::STANDBY)
            status_kind_ = StatusKind::kIdle;
        else
            status_kind_ = StatusKind::kOther;
        if (!notification_duration_)
            lv_label_set_text(modular_status_label_, status_text_.c_str());
    }
    if (update.flags & kEmotion)
        emotion_state_ = update.emotion;
    if (update.flags & kChat)
        lv_label_set_text(subtitle_label_, subtitle.c_str());
    if (update.flags & kNotice) {
        lv_label_set_text(modular_status_label_, update.notice.c_str());
        notification_started_ = lv_tick_get();
        notification_duration_ = update.notice_duration;
    }
    if (update.flags & kHide) {
        if (update.hide)
            lv_obj_add_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_remove_flag(subtitle_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ModularDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    (void)image;  // no camera preview on this UI
}

void ModularDisplay::SetTheme(Theme* theme) {
    UiLock lock(this);
    if (!lock)
        return;
    if (theme != nullptr) {
        current_theme_ = theme;
    }
}

void ModularDisplay::SetHideSubtitle(bool hide) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.hide = hide;
    pending_.flags |= kHide;
}

void ModularDisplay::UpdateStatusBar(bool update_all) {
    (void)update_all;  // status bar icons are not part of this layout yet
}

void ModularDisplay::SetPowerSaveMode(bool on) {
    allow_dim_ = on;
    if (!on)
        wake_requested_ = true;
}
