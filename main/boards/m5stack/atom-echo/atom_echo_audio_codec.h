#ifndef _ATOM_ECHO_AUDIO_CODEC_H_
#define _ATOM_ECHO_AUDIO_CODEC_H_

#include "codecs/no_audio_codec.h"

#include <driver/i2s_std.h>
#include <driver/i2s_pdm.h>
#include <mutex>

// Half-duplex codec for M5Stack ATOM Echo.
// Speaker (NS4168, I2S std, port 1) and microphone (SPM1423, PDM RX, port 0)
// share GPIO33. Only one channel is enabled at a time; before enabling a
// channel its GPIOs are re-routed so it owns GPIO33. Playback has priority:
// while audio is being played, reads return silence instead of stealing the bus.
class AtomEchoAudioCodec : public NoAudioCodec {
public:
    AtomEchoAudioCodec(int input_sample_rate, int output_sample_rate,
                       gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                       gpio_num_t mic_clk, gpio_num_t mic_din, int mic_gain);
    virtual ~AtomEchoAudioCodec();

protected:
    virtual int Write(const int16_t* data, int samples) override;
    virtual int Read(int16_t* dest, int samples) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;

private:
    enum class BusMode { kIdle, kRx, kTx };

    void SwitchBus(BusMode mode);  // caller must hold bus_mutex_

    std::mutex bus_mutex_;
    BusMode mode_ = BusMode::kIdle;
    int64_t last_write_us_ = 0;
    int mic_gain_ = 1;
    i2s_std_gpio_config_t tx_gpio_ = {};
    i2s_pdm_rx_gpio_config_t rx_gpio_ = {};
};

#endif // _ATOM_ECHO_AUDIO_CODEC_H_
