# M5Stack ATOM Echo

ESP32-PICO-D4, 4 MB flash, no PSRAM. Speaker NS4168 (I2S), microphone SPM1423 (PDM).

| Function | GPIO |
|---|---|
| Speaker BCLK | 19 |
| Speaker LRCK / Mic PDM CLK (shared) | 33 |
| Speaker DATA | 22 |
| Mic DATA | 23 |
| Button | 39 |
| SK6812 RGB LED | 27 |

Because GPIO33 is shared, audio is half-duplex: the device cannot listen while
speaking. Interrupt playback with the button. No wake word (needs PSRAM on ESP32).

Button: click = start/stop conversation; hold 3 s = Wi-Fi setup mode.
