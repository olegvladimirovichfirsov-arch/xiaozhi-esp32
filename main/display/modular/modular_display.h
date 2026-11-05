#ifndef MODULAR_DISPLAY_H
#define MODULAR_DISPLAY_H

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "character.h"
#include "lcd_display.h"

// Round 360x360 UI built from swappable parts. Three swipeable pages:
//   left: settings (Wi-Fi, character, volume, brightness, HA setup)
//   center: character + status + subtitle
//   right: Home Assistant entities (lights, climate, scenes)
// See docs/modular-ui/README.md.
class ModularDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;
    ~ModularDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void RemoveChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    void SetTheme(Theme* theme) override;
    void SetHideSubtitle(bool hide) override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetPowerSaveMode(bool on) override;

    // Used by static LVGL callbacks (modular_ui.cc).
    void OnHomeRowEvent(void* row_ctx, int event_code);
    void CycleCharacter();
    void ShowWifiPage();
    void CloseWifiPage();
    void RebuildWifiList();
    void StartScan();
    void ToggleSettingsServer();
    void OnUiTimer();

private:
    class UiLock {
    public:
        explicit UiLock(ModularDisplay* self, int timeout_ms = 50)
            : self_(self), locked_(self->Lock(timeout_ms)) {}
        ~UiLock() {
            if (locked_)
                self_->Unlock();
        }
        explicit operator bool() const { return locked_; }
        UiLock(const UiLock&) = delete;
        UiLock& operator=(const UiLock&) = delete;

    private:
        ModularDisplay* self_;
        bool locked_;
    };
    enum class StatusKind { kOther, kListening, kSpeaking, kIdle };

    void ApplyCharacterState();  // LVGL lock must be held
    void BuildTiles(lv_obj_t* screen);
    void BuildHomeTile();
    void BuildSettingsTile();
    void RefreshHome();
    void FlushPendingUi();

    std::unique_ptr<Character> character_;
    lv_obj_t* tileview_ = nullptr;
    lv_obj_t* tile_settings_ = nullptr;
    lv_obj_t* tile_main_ = nullptr;
    lv_obj_t* tile_home_ = nullptr;
    lv_obj_t* active_tile_seen_ = nullptr;
    lv_obj_t* modular_status_label_ = nullptr;
    lv_obj_t* subtitle_label_ = nullptr;
    lv_obj_t* home_list_ = nullptr;
    lv_obj_t* home_empty_label_ = nullptr;
    lv_obj_t* home_spinner_ = nullptr;
    lv_obj_t* home_connection_label_ = nullptr;
    lv_obj_t* char_row_label_ = nullptr;
    lv_obj_t* ha_row_label_ = nullptr;
    lv_obj_t* phone_info_label_ = nullptr;
    lv_obj_t* wifi_page_ = nullptr;
    lv_obj_t* wifi_list_ = nullptr;
    lv_obj_t* wifi_form_ = nullptr;
    lv_obj_t* wifi_info_label_ = nullptr;
    lv_timer_t* ui_timer_ = nullptr;
    uint32_t ha_version_seen_ = 0xFFFFFFFF;
    uint32_t wifi_version_seen_ = 0;
    StatusKind status_kind_ = StatusKind::kIdle;
    CharacterState emotion_state_ = CharacterState::kIdle;
    bool offline_ = false;
    std::string last_subtitle_;
    std::string character_id_;
    std::map<std::string, lv_obj_t*> home_rows_;
    std::vector<std::string> home_order_;
    std::string status_text_;
    std::string phone_text_;
    uint32_t notification_started_ = 0;
    uint32_t notification_duration_ = 0;
    std::atomic<bool> allow_dim_{false};
    std::atomic<bool> wake_requested_{false};
    bool dimmed_ = false;
    bool has_character_state_ = false;
    CharacterState applied_state_ = CharacterState::kIdle;
    enum PendingFlag : unsigned { kStatus = 1, kEmotion = 2, kChat = 4, kNotice = 8, kHide = 16 };
    struct PendingUi {
        unsigned flags = 0;
        std::string status;
        std::string notice;
        CharacterState emotion = CharacterState::kIdle;
        uint32_t notice_duration = 0;
        bool hide = false;
    };
    std::mutex pending_mutex_;
    PendingUi pending_;
    uint32_t maintenance_at_ = 0;
};

#endif  // MODULAR_DISPLAY_H
