#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "atom_echo_audio_codec.h"

#include <esp_log.h>

#define TAG "AtomEcho"

class AtomEchoBoard : public WifiBoard {
private:
    Button button_;

    void InitializeButtons() {
        // Click: start / stop a conversation (interrupts playback too).
        button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        // Long press: re-enter Wi-Fi provisioning (hotspot mode).
        button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "Long press: entering Wi-Fi config mode");
            EnterWifiConfigMode();
        });
    }

public:
    // G39 is input-only with an external pull-up on the ATOM board; active low.
    AtomEchoBoard() : button_(BOOT_BUTTON_GPIO, false, 3000) {
        InitializeButtons();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);  // SK6812, WS2812-compatible timing
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static AtomEchoAudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_CLK, AUDIO_I2S_MIC_GPIO_DIN, AUDIO_MIC_GAIN);
        return &audio_codec;
    }
};

DECLARE_BOARD(AtomEchoBoard);
