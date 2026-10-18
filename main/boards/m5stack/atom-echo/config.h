#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// M5Stack ATOM Echo (SKU: C008-C), ESP32-PICO-D4, 4 MB flash, no PSRAM.
// Pin map from the official M5Stack documentation (docs.m5stack.com/en/atom/atomecho):
//   NS4168 amp  : BCLK G19, LRCK G33, DATA G22
//   SPM1423 mic : CLK  G33 (shared with amp LRCK), DATA G23   (PDM)
//   Button G39 (input-only, external pull-up), SK6812 RGB LED G27
//
// G33 is shared between the speaker word-select and the microphone PDM clock,
// so playback and capture can never run at the same time. The board uses a
// half-duplex codec (atom_echo_audio_codec.*) that switches the bus.

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_SPK_GPIO_BCLK  GPIO_NUM_19
#define AUDIO_I2S_SPK_GPIO_LRCK  GPIO_NUM_33
#define AUDIO_I2S_SPK_GPIO_DOUT  GPIO_NUM_22

#define AUDIO_I2S_MIC_GPIO_CLK   GPIO_NUM_33
#define AUDIO_I2S_MIC_GPIO_DIN   GPIO_NUM_23

// Software gain for the PDM microphone (integer factor). Tune on hardware.
#define AUDIO_MIC_GAIN           4

#define BOOT_BUTTON_GPIO         GPIO_NUM_39
#define BUILTIN_LED_GPIO         GPIO_NUM_27

#endif // _BOARD_CONFIG_H_
