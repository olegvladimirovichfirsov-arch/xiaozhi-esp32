# Modular UI for Waveshare ESP32-S3-Touch-LCD-1.85B

Goal: a round 360x360 touch UI where characters and visual styles are swappable
packs, Home Assistant entities appear on screen without reflashing, and voice
features (continuous dialog, barge-in, wake word) are layered on top.

## Board
- New variant, do not touch the stock `esp32-s3-touch-lcd-1.85b` (board identity affects OTA):
  `esp32-s3-touch-lcd-1.85b-modui`.
- ES8311 + ES7210 with echo reference and `CONFIG_USE_DEVICE_AEC=y`: barge-in is hardware-feasible.

## Layers
1. Core: voice, Wi-Fi manager (multi-network), HA client. No visuals.
2. Screen engine: screens are data (`screens/*.json`), not code. Screens:
   home, light, climate, scenes, wifi, settings, offline, done.
3. Style pack: colors, fonts, tile shapes, icons.
4. Character pack: one animation per state.

## Character states
idle, listening, thinking, speaking, joy, surprise, thoughtful, sad, sleep, greeting, offline, done.
The engine only asks for a state; the pack decides the art.

## Pack layout
```
packs/<id>/manifest.json
packs/<id>/<state>_<frame>.bin   (LVGL RGB565A8, <=240x240, 8-12 fps)
```
manifest.json:
```json
{
  "id": "ghost_cat",
  "type": "character",
  "name": "Привидение и кот",
  "size": [240, 240],
  "fps": 10,
  "states": {
    "idle": ["idle_0.bin", "idle_1.bin"],
    "listening": ["listen_0.bin"],
    "joy": ["joy_0.bin", "joy_1.bin"]
  },
  "fallback": "idle"
}
```
Style pack: `"type": "style"` with `colors`, `radius`, `font`, `icons`.
Missing state falls back to `fallback`.

## Where packs live
- Built-in: 2 packs in the assets partition (reuse the existing `MountAssets` mechanism).
- Downloaded: from HA `/config/www/xiaozhi/packs/<id>/` over Wi-Fi into a packs partition.
- Selected pack ids are stored in NVS (`ui.character`, `ui.style`); changeable from the
  settings screen and from HA.

## HA entities by label
- Label `колонка` on any entity/scene in HA means "show on the speaker".
- Device reads the list over the HA WebSocket API, builds tiles by domain
  (light, climate, scene, switch), and subscribes to state changes.
- Two HA URLs: local first, external as fallback.
- Credentials: dedicated non-admin HA user + long-lived token, entered by the owner on the
  device settings page. Never committed to the repo.

## Wi-Fi
- Multiple saved networks (SSID list in NVS), auto-connect to the strongest known one.
- Add network from the on-screen keyboard or from a small web page served by the device.

## Milestones
1. Board variant, screen engine, built-in ghost_cat pack, touch navigation.
2. HA client, label-based tiles, scene/light/climate control.
3. Pack download, character/style switching.
4. Wi-Fi manager screen, two HA URLs.
5. Voice: continuous dialog, barge-in tuning, wake word.

## Rules
- No flashing without the owner's explicit permission.
- Build via GitHub Actions on branch `custom-ui-185b`.
