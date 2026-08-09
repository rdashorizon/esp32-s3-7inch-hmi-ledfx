# Plan — CrowPanel 7" HMI LedFX Controller

## 1. Goal

Standalone wall-controller for a LedFX installation. Touch the screen, fire a
scene, dim the room, randomize a virtual. No phone, no laptop, no browser.

## 2. Hardware (confirmed from Elecrow wiki)

| Item | Value |
|---|---|
| Board | CrowPanel ESP32 HMI 7.0-inch (SKU `DIS08070H`) |
| MCU | ESP32-S3-WROOM-1-N4R8 (dual-core 240 MHz, 4 MB flash, 8 MB PSRAM) |
| LCD | 800×480 RGB TFT, EK9716BD3 + EK73002ACGB |
| Touch | GT911 capacitive, I2C on SDA=IO19, SCL=IO20 |
| Backlight | IO2 |
| RGB data | B0–B4: 15,7,6,5,4 • G0–G5: 9,46,3,8,16,1 • R0–R4: 14,21,47,48,45 |
| Sync | DE=41 • VSYNC=40 • HSYNC=39 • PCLK=0 |
| RGB timing | pclk_neg=1, pclk=15 MHz, hsync porch 40/48/40, vsync porch 1/31/13 |

Variants: V2.0 (no touch reset PCF) and V3.0 (PCA9557 controls touch reset).
Detection: read GT911 reset line state at boot; I2C/GT911 driver is identical.

## 3. Toolchain

- **Framework:** PlatformIO + Arduino-ESP32 v3.x (≈ 2.0.17)
- **Graphics:** LVGL 8.3 + LovyanGFX (`lovyan03/LovyanGFX`)
- **JSON:** ArduinoJson 6.x
- **HTTP:** built-in `HTTPClient`
- **Storage:** `Preferences` (NVS)

## 4. UI (hybrid scope)

Three screens behind a top tab bar:

### Screen 1 — Scenes
- Grid of scene buttons (4 columns, scrollable)
- Each button: scene name + active dot
- Tap → activate • Long-press → deactivate

### Screen 2 — Virtuals
- List of every virtual
- Per-row: name, active effect type, ON/OFF toggle, 🎲 randomize
- Top toolbar: pause-all toggle, clear-all-effects button

### Screen 3 — Global
- Slider: brightness 0.0–1.0 (applies via `PUT /api/effects`)
- Toggle: mirror / flip
- Slider: gradient name (presets: rainbow, sunset, ocean…)
- Status row: server URL, last refresh, connection state

### Setup (captive portal)
- First boot: AP `ledfx-hmi-setup` + form for SSID, password, LedFx URL, user, pass
- Saved to NVS
- 3 failed connection attempts → reverts to AP mode

## 5. LedFx REST API surface

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/api/auth/login` | Bearer token |
| `GET`  | `/api/scenes` | List scenes |
| `PUT`  | `/api/scenes` `{id,action:"activate"\|"deactivate"}` | Toggle scene |
| `GET`  | `/api/virtuals` | List virtuals |
| `PUT`  | `/api/virtuals/{id}` `{active:bool}` | Toggle virtual |
| `PUT`  | `/api/virtuals/{id}/effects` `{config:"RANDOMIZE"}` | Randomize |
| `PUT`  | `/api/effects` `{action:"clear_all_effects"}` | Clear all |
| `PUT`  | `/api/effects` `{action:"apply_global", brightness, …}` | Globals |

All non-auth calls carry `Authorization: Bearer <token>`.

## 6. Module layout

```
src/
  main.cpp       setup + loop, glue
  pins.h         RGB + touch pin map
  display.h/.cpp LovyanGFX panel + backlight
  touch.h/.cpp   GT911 I2C driver, LVGL indev
  lvgl_port.h/.cpp LVGL tick/draw/loop
  config.h/.cpp  NVS + captive portal
  net.h/.cpp    WiFi + HTTPClient with bearer
  ledfx.h/.cpp  REST client, named methods
  ui.h/.cpp     LVGL screens
```

## 7. Memory

LVGL 800×480 RGB565 single-buffer = 768 KB → PSRAM. Draw buffers = two
`1/10`-line splits (≈ 77 KB each) in normal RAM.

## 8. Out of scope

- Per-effect parameter sliders (use LedFx web UI)
- Preset CRUD (schema validation; want a real client)
- New virtual creation
- OTA updates (add later in 30 lines if needed)
- WLED multi-node control (belongs in LedFx, not this HUD)

## 9. Verification

- `pio run` succeeds
- Hand-test loop: flash → captive portal → connect → list scenes → toggle
- 30-min burn-in on the scene screen with auto-refresh

## 10. As-built notes (corrections to the plan above)

Verified against real hardware and the LedFx source; a few assumptions in
sections 5–8 turned out wrong:

- **Auth is optional, not bearer-token by default.** Mainline LedFx has no
  `POST /api/auth/login`. The controller connects on WiFi alone and only
  attempts login when credentials are provided; a real 401/403 triggers one
  re-auth + retry.
- **Global brightness = `PUT /api/config {"global_brightness": …}`** (and `GET`
  to read it). The effect-config `brightness` is a separate multiplier; the
  master brightness the LedFx UI drives is `global_brightness`.
- **Gradient / mirror / flip = `PUT /api/effects {"action":"apply_global", …}`**
  (real endpoint; see LedFx `docs/apis/global.md`). Gradients must use LedFx's
  built-in keys (Rainbow, Ocean, Viridis, …). These bulk actions return HTTP 200
  even on failure with `{"status":"failed"}`, so the client parses that.
- **Global pause = top-level `paused` in `GET /api/virtuals`**; the Global
  controls seed from live server state on load.
- **Networking is dual-core async** (FreeRTOS worker on core 0 + request/result
  queues), so the UI never blocks on the network; WiFi auto-reconnects.
- **No BOOT-button reset** — GPIO0 is the LCD pixel clock. Setup is re-entered
  via on-screen **Global → Settings** (edit in place) or **Reset** (wipe +
  portal). Panel brightness has an on-device slider + auto-dim and persists.

## 11. Out of scope — still open (Tier 4 candidates)

mDNS auto-discovery of the LedFx server, live WebSocket state (vs polling), OTA
updates, and MQTT / Home Assistant integration.
