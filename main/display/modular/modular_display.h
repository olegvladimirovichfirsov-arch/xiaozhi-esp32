#ifndef MODULAR_DISPLAY_H
#define MODULAR_DISPLAY_H

#include <memory>
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
    enum class StatusKind { kOther, kListening, kSpeaking, kIdle };

    void ApplyCharacterState();  // LVGL lock must be held
    void BuildTiles(lv_obj_t* screen);
    void BuildHomeTile();
    void BuildSettingsTile();
    void RefreshHome();

    std::unique_ptr<Character> character_;
    lv_obj_t* tileview_ = nullptr;
    lv_obj_t* tile_settings_ = nullptr;
    lv_obj_t* tile_main_ = nullptr;
    lv_obj_t* tile_home_ = nullptr;
    lv_obj_t* modular_status_label_ = nullptr;
    lv_obj_t* subtitle_label_ = nullptr;
    lv_obj_t* home_list_ = nullptr;
    lv_obj_t* home_empty_label_ = nullptr;
    lv_obj_t* char_row_label_ = nullptr;
    lv_obj_t* ha_row_label_ = nullptr;
    lv_obj_t* phone_info_label_ = nullptr;
    lv_obj_t* wifi_page_ = nullptr;
    lv_obj_t* wifi_list_ = nullptr;
    lv_obj_t* wifi_form_ = nullptr;
    lv_timer_t* ui_timer_ = nullptr;
    uint32_t ha_version_seen_ = 0xFFFFFFFF;
    StatusKind status_kind_ = StatusKind::kIdle;
    CharacterState emotion_state_ = CharacterState::kIdle;
    bool offline_ = false;
    std::string last_subtitle_;
    std::string character_id_;
};

#endif  // MODULAR_DISPLAY_H
