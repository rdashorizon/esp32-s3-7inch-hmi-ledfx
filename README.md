# CrowPanel 7" HMI LedFX Controller

A standalone wall-mount touch controller for a [LedFX](https://github.com/LedFx/LedFx)
installation, built on the **Elecrow CrowPanel ESP32 HMI 7.0-inch** display
(800×480 RGB TFT, ESP32-S3-WROOM-1-N4R8, GT911 capacitive touch).

Touch the screen — fire a scene, dim the lights, randomize a virtual. No phone, no
laptop, no browser.

## What it does

| Screen | What you can do |
|---|---|
| **Scenes** | Browse every saved LedFx scene as a grid. Tap → activate. Long-press → deactivate. |
| **Virtuals** | List every virtual. Toggle each on/off. 🎲 randomize the active effect. Pause all. Clear all. |
| **Global** | Global brightness slider, flip/mirror toggles, gradient picker. |
| **Setup** | First-run captive portal for WiFi + LedFx URL + username/password. |

## Hardware

- **Board:** Elecrow CrowPanel ESP32 HMI 7.0-inch (SKU `DIS08070H`)
- **MCU:** ESP32-S3-WROOM-1-N4R8 (dual-core 240 MHz, 4 MB flash, 8 MB PSRAM)
- **LCD:** 800×480 RGB TFT, EK9716BD3 + EK73002ACGB
- **Touch:** GT911 capacitive, I2C on SDA=IO19, SCL=IO20
- **Backlight:** IO2

The pin map lives in [`src/pins.h`](src/pins.h) — single source of truth, lifted
from the [Elecrow wiki](https://www.elecrow.com/wiki/esp32-display-702727-intelligent-touch-screen-wi-fi26ble-800480-hmi-display.html).

Both V2.0 and V3.0 of the board are supported. V3.0 has a PCA9557 that controls
the GT911 reset sequence; the GT911 I2C driver is identical.

## Build

You need [PlatformIO](https://platformio.org/) (Core 6.x or PIO in VS Code).

```bash
git clone https://github.com/rdashorizon/esp32-s3-7inch-hmi-ledfx.git
cd esp32-s3-7inch-hmi-ledfx
pio run -e crowpanel-7inch
# Flash:
pio run -e crowpanel-7inch -t upload --upload-port /dev/ttyUSB0
# Monitor serial:
pio device monitor -p /dev/ttyUSB0 -b 115200
```

## First-run setup

On first power the board creates a WiFi access point named `ledfx-hmi-setup`.
Connect to it with your phone or laptop, a captive portal will pop up. Enter:

1. Your WiFi SSID and password
2. Your LedFx server URL (e.g. `http://192.168.1.20:8888`)
3. Your LedFx username and password (or leave blank if your LedFx doesn't require auth)

The board reboots, connects to WiFi, contacts LedFx, and shows the scene grid.

To reset settings, hold the **BOOT** button for 5 seconds at boot.

## Architecture

```
src/
  main.cpp          setup + loop, glue between display/touch/lvgl/net/ledfx/ui
  pins.h            RGB + GT911 + backlight pin map
  display.h/.cpp    LovyanGFX panel + backlight PWM
  touch.h/.cpp      GT911 I2C driver, LVGL input device
  lvgl_port.h/.cpp  LVGL tick/draw/loop, register display + touch
  config.h/.cpp     NVS (Preferences) persistence, captive portal
  net.h/.cpp        WiFi + HTTPClient with bearer-token auth
  ledfx.h/.cpp      REST client with named methods (getScenes, activateScene, …)
  ui.h/.cpp         LVGL screens (Scenes / Virtuals / Globals / Setup)
```

## LedFx API

All calls use the LedFx v2 REST API. Auth is bearer-token (`POST /api/auth/login`).
See [`docs/plan.md`](docs/plan.md) for the full endpoint table.

## License

MIT. Use it, fork it, ship it.
