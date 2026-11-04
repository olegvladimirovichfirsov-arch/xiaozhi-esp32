#ifndef MODULAR_DISPLAY_H
#define MODULAR_DISPLAY_H

#include <memory>
#include <string>

#include "character.h"
#include "lcd_display.h"

// Round 360x360 UI built from swappable parts: a Character (state -> art) and,
// later, style packs and data-driven screens. See docs/modular-ui/README.md.
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

private:
    enum class StatusKind { kOther, kListening, kSpeaking, kIdle };

    void ApplyCharacterState();  // LVGL lock must be held

    std::unique_ptr<Character> character_;
    lv_obj_t* modular_status_label_ = nullptr;
    lv_obj_t* subtitle_label_ = nullptr;
    StatusKind status_kind_ = StatusKind::kIdle;
    CharacterState emotion_state_ = CharacterState::kIdle;
    bool offline_ = false;
    std::string last_subtitle_;
};

#endif  // MODULAR_DISPLAY_H
