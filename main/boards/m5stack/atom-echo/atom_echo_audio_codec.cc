#include "atom_echo_audio_codec.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cstring>
#include <vector>

#define TAG "AtomEchoCodec"

namespace {
// Keep the bus in TX mode for this long after the last written frame, so short
// gaps between decoded packets do not toggle the bus (audible clicks).
constexpr int64_t kTxHoldUs = 400 * 1000;
constexpr uint32_t kIoTimeoutMs = 200;
}

AtomEchoAudioCodec::AtomEchoAudioCodec(int input_sample_rate, int output_sample_rate,
                                       gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                                       gpio_num_t mic_clk, gpio_num_t mic_din, int mic_gain) {
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    mic_gain_ = mic_gain > 0 ? mic_gain : 1;

    // ---- Speaker: standard I2S (Philips), 32-bit mono duplicated to both slots, port 1
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(1), I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM;
    tx_chan_cfg.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM;
    tx_chan_cfg.auto_clear_after_cb = true;
    tx_chan_cfg.auto_clear_before_cb = false;
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_handle_, nullptr));

    tx_gpio_.mclk = I2S_GPIO_UNUSED;
    tx_gpio_.bclk = spk_bclk;
    tx_gpio_.ws = spk_ws;
    tx_gpio_.dout = spk_dout;
    tx_gpio_.din = I2S_GPIO_UNUSED;

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)output_sample_rate_),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = tx_gpio_,
    };
    tx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &tx_std_cfg));

    // ---- Microphone: PDM RX (only I2S0 supports PDM RX on ESP32)
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(XIAOZHI_I2S_PORT(0), I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM;
    rx_chan_cfg.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, nullptr, &rx_handle_));

    rx_gpio_.clk = mic_clk;
    rx_gpio_.din = mic_din;
    rx_gpio_.invert_flags.clk_inv = false;

    i2s_pdm_rx_config_t pdm_rx_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG((uint32_t)input_sample_rate_),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = rx_gpio_,
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx_handle_, &pdm_rx_cfg));

    ESP_LOGI(TAG, "Half-duplex codec ready (in %d Hz PDM, out %d Hz I2S, mic gain x%d)",
             input_sample_rate_, output_sample_rate_, mic_gain_);
}

AtomEchoAudioCodec::~AtomEchoAudioCodec() {
    std::lock_guard<std::mutex> lock(bus_mutex_);
    SwitchBus(BusMode::kIdle);
    if (tx_handle_ != nullptr) {
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
    }
    if (rx_handle_ != nullptr) {
        i2s_del_channel(rx_handle_);
        rx_handle_ = nullptr;
    }
}

void AtomEchoAudioCodec::SwitchBus(BusMode mode) {
    if (mode == mode_) {
        return;
    }
    if (mode_ == BusMode::kRx) {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    } else if (mode_ == BusMode::kTx) {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
    if (mode == BusMode::kTx) {
        // Re-route GPIO19/33/22 to the I2S1 TX signals.
        ESP_ERROR_CHECK(i2s_channel_reconfig_std_gpio(tx_handle_, &tx_gpio_));
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    } else if (mode == BusMode::kRx) {
        // Re-route GPIO33 to the I2S0 PDM clock and GPIO23 to PDM data in.
        ESP_ERROR_CHECK(i2s_channel_reconfig_pdm_rx_gpio(rx_handle_, &rx_gpio_));
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    }
    ESP_LOGD(TAG, "Bus mode %d -> %d", (int)mode_, (int)mode);
    mode_ = mode;
}

int AtomEchoAudioCodec::Write(const int16_t* data, int samples) {
    std::vector<int32_t> buffer(samples);
    int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;
    for (int i = 0; i < samples; i++) {
        int64_t temp = int64_t(data[i]) * volume_factor;
        if (temp > INT32_MAX) {
            buffer[i] = INT32_MAX;
        } else if (temp < INT32_MIN) {
            buffer[i] = INT32_MIN;
        } else {
            buffer[i] = static_cast<int32_t>(temp);
        }
    }

    std::lock_guard<std::mutex> lock(bus_mutex_);
    SwitchBus(BusMode::kTx);
    size_t bytes_written = 0;
    if (i2s_channel_write(tx_handle_, buffer.data(), samples * sizeof(int32_t),
                          &bytes_written, kIoTimeoutMs) != ESP_OK) {
        ESP_LOGW(TAG, "Write timeout");
    }
    last_write_us_ = esp_timer_get_time();
    return bytes_written / sizeof(int32_t);
}

int AtomEchoAudioCodec::Read(int16_t* dest, int samples) {
    {
        std::unique_lock<std::mutex> lock(bus_mutex_);
        bool playing = mode_ == BusMode::kTx &&
                       (esp_timer_get_time() - last_write_us_) < kTxHoldUs;
        if (playing) {
            // Speaker owns GPIO33: feed silence at real-time pace.
            lock.unlock();
            memset(dest, 0, samples * sizeof(int16_t));
            int ms = samples * 1000 / input_sample_rate_;
            vTaskDelay(pdMS_TO_TICKS(ms > 0 ? ms : 1));
            return samples;
        }

        SwitchBus(BusMode::kRx);
        size_t bytes_read = 0;
        // PDM demodulated data is 16-bit.
        if (i2s_channel_read(rx_handle_, dest, samples * sizeof(int16_t),
                             &bytes_read, kIoTimeoutMs) != ESP_OK) {
            return 0;
        }
        samples = bytes_read / sizeof(int16_t);
    }

    if (mic_gain_ > 1) {
        for (int i = 0; i < samples; i++) {
            int32_t v = int32_t(dest[i]) * mic_gain_;
            dest[i] = (v > INT16_MAX) ? INT16_MAX : (v < -INT16_MAX) ? -INT16_MAX : (int16_t)v;
        }
    }
    return samples;
}

void AtomEchoAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(bus_mutex_);
    if (!enable && mode_ == BusMode::kRx) {
        SwitchBus(BusMode::kIdle);
    }
    AudioCodec::EnableInput(enable);  // flag only; the bus is switched lazily
}

void AtomEchoAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(bus_mutex_);
    if (!enable && mode_ == BusMode::kTx) {
        SwitchBus(BusMode::kIdle);
    }
    AudioCodec::EnableOutput(enable);
}
