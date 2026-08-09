# CrowPanel 7" HMI LedFX Controller

A standalone wall-mount touch controller for a [LedFX](https://github.com/LedFx/LedFx)
installation, built on the **Elecrow CrowPanel ESP32 HMI 7.0-inch** display
(800×480 RGB TFT, ESP32-S3-WROOM-1-N4R8, GT911 capacitive touch).

Touch the screen — fire a scene, dim the lights, randomize a virtual. No phone, no
laptop, no browser.

## What it does

| Screen | What you can do |
|---|---|
| **Scenes** | Browse every saved LedFx scene as a scrollable grid. Tap → activate. Long-press → deactivate. The active scene is highlighted. |
| **Virtuals** | List every virtual with its active effect. Toggle each on/off. 🎲 randomize the active effect. Pause all. Clear all. |
| **Global** | Master brightness (LedFx `global_brightness`), mirror/flip, gradient picker, on-device **screen brightness**, plus **Settings** and **Reset** buttons. |
| **Setup** | First-run captive portal for WiFi + LedFx URL + username/password. |

The Global controls seed from the live server state, and the front data screen
auto-refreshes so the HUD tracks changes made from the LedFx web UI. All LedFx
network I/O runs on a background core, so touch and rendering never freeze while
a request is in flight, and the link self-heals after a WiFi drop. A WiFi
indicator (top-right) shows the connection state; the panel auto-dims after a
minute of no touch.

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

1. Your WiFi SSID and password (2.4 GHz WPA2 — the ESP32-S3 has no 5 GHz radio)
2. Your LedFx server URL (e.g. `http://192.168.1.20:8888`)
3. Your LedFx username and password — **optional**; leave blank for a default
   LedFx install (mainline LedFx has no login, and the controller only attempts
   authentication when you provide credentials)

The board reboots, connects to WiFi, contacts LedFx, and shows the scene grid.

## Changing or resetting settings

Everything is done on the device — there is no BOOT-button reset (GPIO0 is the
LCD pixel clock on this board, so it can't double as a button):

- **Global → Settings** edits WiFi / LedFx URL / credentials in place (on-screen
  keyboard), then saves and reboots.
- **Global → Reset WiFi / settings** wipes the saved config and reboots into the
  `ledfx-hmi-setup` captive portal.

## Architecture

```
src/
  main.cpp          setup + loop; start worker, then UI
  pins.h            RGB + GT911 + backlight pin map
  display.h/.cpp    LovyanGFX RGB panel + PWM backlight (Light_PWM)
  touch.h/.cpp      GT911 I2C driver, LVGL input device
  lvgl_port.h/.cpp  LVGL tick/draw/loop, register display + touch
  config.h/.cpp     NVS (Preferences) persistence, captive portal, screen brightness
  net.h/.cpp        WiFi + HTTPClient; optional bearer auth with re-auth on 401
  ledfx.h/.cpp      REST client with named methods (fetch_scenes, activate_scene, …)
  worker.h/.cpp     FreeRTOS network task (core 0) + request/result queues
  ui.h/.cpp         LVGL screens (Scenes / Virtuals / Global / Settings)
```

Threading: LVGL renders and reads touch on core 1; all LedFx REST calls run on a
FreeRTOS task pinned to core 0. The UI enqueues a request, the worker performs
the HTTP call and posts a result, and a UI-side timer drains results and
repaints — the worker never touches LVGL and the UI never touches the network
client.

## LedFx API

Uses the LedFx REST API over plain HTTP. **Authentication is optional** —
mainline LedFx has no login endpoint, so the controller talks to it tokenless
unless you supply credentials (in which case it does `POST /api/auth/login` and
re-authenticates on a 401). Endpoints used: `/api/scenes`, `/api/virtuals`,
`/api/virtuals/{id}`, `/api/virtuals/{id}/effects`, `/api/effects`
(`action: apply_global` for gradient/mirror/flip), and `/api/config`
(`global_brightness`). See [`docs/plan.md`](docs/plan.md) for more.

## License

MIT. Use it, fork it, ship it.
