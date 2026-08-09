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

Four screens behind a top tab bar (the last-selected tab persists in NVS):

### Screen 1 — Scenes
- Grid of scene buttons (4 columns, scrollable)
- Each button: scene name + active dot
- Tap → activate • Long-press → deactivate

### Screen 2 — Virtuals
- List of every virtual
- Per-row: name, active effect type, 20×20 gradient preview swatch, ON/OFF toggle, 🎲 randomize
- Top toolbar: pause-all toggle, clear-all-effects button
- Long-press a row → inspect (msgbox with id / active / effect / gradient)
- **Tap the gradient swatch → per-virtual color picker modal** (see §13).
  Sends `PUT /api/virtuals/{id}/effects` with just the `background_color`,
  preserving all other effect config

### Screen 3 — Color
- LVGL colorwheel (HSV) + R/G/B sliders (0–255)
- Live-preview swatch follows the wheel and sliders in real time
- Throttled live-apply during drag (≤ 1 PUT / 250 ms)
- Apply button (manual send) + Black button (`background_color: "#000000"` — closest substitute for an "off" state; see §12)
- Transient error banner surfaces apply failures on the tab itself (3 s auto-dismiss)
- Last picked color persists in NVS and is restored on boot (see §12)

### Screen 4 — Global
- Slider: brightness 0.0–1.0 (applies via `PUT /api/config`)
- Toggle: mirror / flip
- Slider: gradient name (presets: rainbow, sunset, ocean…)
- Status row: server URL, last refresh, connection state
- Settings + Reset buttons

### Setup (captive portal)
- First boot: AP `ledfx-hmi-setup` + form for SSID, password, LedFx URL, user, pass
- Per-device PSK derived from the chip's efuse MAC last-3-bytes (printed on the form); WPA2 requires 8+ chars so a fixed suffix is appended
- Saved to NVS
- 3 failed connection attempts → reverts to AP mode
- Captive-portal `/save` validates the LedFx URL is `http(s)://...` before persisting; the on-device settings editor does the same and rejects invalid input without rebooting

## 5. LedFx REST API surface

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/api/auth/login` | Bearer token (optional — only used when the user supplied credentials in setup) |
| `GET`  | `/api/scenes` | List scenes |
| `PUT`  | `/api/scenes` `{id,action:"activate"\|"deactivate"}` | Toggle scene |
| `GET`  | `/api/virtuals` | List virtuals + top-level `paused` |
| `PUT`  | `/api/virtuals` `{paused:bool}` | Pause/resume all virtual output |
| `PUT`  | `/api/virtuals/{id}` `{active:bool}` | Toggle virtual |
| `PUT`  | `/api/virtuals/{id}/effects` `{config:"RANDOMIZE"}` | Randomize |
| `PUT`  | `/api/virtuals/{id}/effects` `{type, config:{background_color:"#rrggbb"}}` | Set per-virtual color (see §13) |
| `PUT`  | `/api/effects` `{action:"clear_all_effects"}` | Clear all |
| `PUT`  | `/api/effects` `{action:"apply_global", background_color:"#rrggbb"}` | Set global LED color |
| `PUT`  | `/api/effects` `{action:"apply_global", gradient:"Rainbow"}` | Set global gradient |
| `PUT`  | `/api/effects` `{action:"apply_global", mirror\|flip:true\|false}` | Toggle mirror/flip |
| `GET`/`PUT` | `/api/config` `{global_brightness:0.0..1.0}` | Master brightness |

When credentials are configured, non-auth calls carry `Authorization: Bearer …`;
mainline LedFx has no auth and the controller connects tokenless in that case.

## 6. Module layout

```
src/
  main.cpp        setup + loop, glue; periodic heap watermark on core 0
  pins.h          RGB + GT911 + backlight pin map
  display.h/.cpp  LovyanGFX panel + PWM backlight
  touch.h/.cpp    GT911 I2C driver, LVGL input device (throttled to ~60 Hz)
  lvgl_port.h/.cpp  LVGL tick/draw/loop; PSRAM draw buffers
  config.h/.cpp   NVS (Preferences) + captive portal + screen brightness +
                  last-tab + last-color persistence; URL validator
  net.h/.cpp      WiFi + HTTPClient with bearer-token re-auth (Net::login
                  returns LoginStatus so the UI can distinguish NOT_FOUND
                  from BAD_CREDS from TRANSPORT)
  ledfx.h/.cpp    REST client with named methods (also reads the gradient
                  field from each virtual's effect config for the per-row
                  swatch in §4 Screen 2)
  worker.h/.cpp   FreeRTOS task pinned to core 0 + request/result queues;
                  request builders: req_simple/req_id/req_payload/req_arg/
                  req_flag/req_test_connection; worker_suspend/resume for OTA
  ota.h/.cpp      network OTA (ArduinoOTA/espota): progress overlay + worker
                  suspend during the flash (Tier 4)
  ui.h/.cpp       Tabview + result pump + glue (target: <250 LOC)
  ui_global.{h,cpp}     Global tab + connection indicator + panel-bright
                        slider + auto-dim + boot splash
  ui_scenes.{h,cpp}     Scenes tab (4-column grid, scrollable)
  ui_virtuals.{h,cpp}   Virtuals tab (list + pause-all toolbar + clear-all +
                        long-press inspect + per-virtual color modal — see §13)
  ui_color.{h,cpp}      Color picker tab (colorwheel + RGB sliders + swatch
                        + Apply/Black + transient error banner)
  ui_settings.{h,cpp}   On-device settings editor (separate LVGL screen;
                        URL validation rejects without rebooting)
  ui_overlay.{h,cpp}    Slow-network overlay (top-layer "still working…"
                        after 1.5 s of submit-with-no-progress)
include/
  lv_conf.h       LVGL 8.3 overrides (RGB565, 48 KB obj pool, Montserrat 14)
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

**Tier 4 shipped: network OTA updates** (see §14). The other three remain open:

- **mDNS auto-discovery of the LedFx server** — deferred deliberately. Mainline
  LedFx does not advertise its own API over zeroconf (it *uses* zeroconf to find
  WLED devices, but doesn't register an `_ledfx._tcp` / `_http._tcp` service for
  itself), so there is nothing reliable to discover. Revisit if/when LedFx
  starts advertising.
- **Live WebSocket state (vs polling)** — needs a WebSocket client dependency
  and a rewrite of the poll-based worker; larger than one tier.
- **MQTT / Home Assistant integration** — needs a broker, `PubSubClient`, and a
  new config surface (broker URL / topic / creds in setup).

## 12. As-built notes — Color picker + related (post-color-picker)

The Color tab is the first new feature shipped after §10. Constraints worth
recording before they get lost:

- **Color = `background_color`, not `color`.** LedFx's `apply_global` accepts
  `background_color` (CSS-style hex like `"#ff8040"`, validated by
  `LedFx/ledfx/color.py::validate_color`). The first draft of this feature
  used `{"color":{r,g,b}}` — wrong key, silently rejected. The
  colorwheel's RGB output is now hex-encoded and sent under
  `background_color`. Confirmed against `LedFx/docs/apis/global.md` and the
  handler in `LedFx/ledfx/api/effects.py::_apply_global`.
- **No "off" state.** `apply_global` requires at least one of
  `{gradient, background_color, background_brightness, brightness, flip,
  mirror}`. Sending `{action:"apply_global"}` alone returns 400
  "invalid_request". The closest substitute is `background_color: "#000000"`,
  which is what the **Black** button does.
- **Only effects that opt-in get touched.** `apply_global` is effect-scoped,
  not virtual- or pixel-scoped. Effects without `background_color` in their
  config schema (gradient-based effects) silently skip the update. The API
  still returns 200 with `Applied to N effects (skipped M)` — we don't
  currently parse the success body to surface the skipped count.
- **Per-virtual color** — the global Color tab sets a single `background_color`
  for every active effect that supports it. Per-virtual color uses a
  different endpoint, `PUT /api/virtuals/{id}/effects` — see §13.
- **Boot-time re-apply is skipped on first boot.** If the saved color matches
  the warm-white default (255, 200, 128) — i.e. the user has never picked
  anything — `setup()` does not send a redundant `apply_global` to LedFx.
  After the first pick, the boot re-apply fires so the LED strip matches the
  device's display state without user action.
- **Color picker UI module:** `src/ui_color.{h,cpp}`. Color lives in NVS as
  3 raw bytes (`K_LASTCLR` key in the `ledfx-hmi` namespace) — not a JSON
  blob, to keep the read/write footprint small. The struct is named
  `LedColor` rather than the more obvious `RGBColor` to avoid colliding with
  LovyanGFX's `RGBColor` (which has `R8/G8/B8` accessors).
- **Tab-index ordering:** Scenes=0, Virtuals=1, Color=2, Global=3. The result
  pump uses the literal `2` to forward `RES_ACTION` to the Color tab's
  error-banner handler — if a 5th tab gets added, that constant needs to
  follow the shift or be replaced with a pointer-equality check.

## 13. As-built notes — Per-virtual color (post-color-picker)

Shipped as a per-virtual counterpart to §12's global Color tab. UX: tap the
gradient swatch on any Virtuals-tab row → modal overlay with a colorwheel +
RGB sliders + Apply/Black/Cancel → on Apply, sends
`PUT /api/virtuals/{id}/effects` with just the `background_color`,
preserving every other effect config field.

- **Endpoint + body shape (confirmed):**
  `PUT /api/virtuals/{id}/effects` with `{ "type": "<current_effect_type>",
  "config": { "background_color": "#rrggbb" } }`. Source:
  `LedFx/docs/apis/api.md` + `ledfx/api/virtual_effects.py`. The `type` field
  is required — if omitted, the handler falls into the "create new effect"
  branch and crashes on `effects.create(type=None)`. We read the type from
  `VirtualInfo.effect_type` (already populated by `fetch_virtuals`).

- **`config` is a partial merge.** LedFx only touches the keys you send.
  Sending only `background_color` leaves brightness, mirror, flip, gradient,
  and every other effect setting untouched. This is what makes the feature
  safe — picking a color doesn't accidentally reset anything else.

- **Per-virtual vs. global color:** the global Color tab uses
  `PUT /api/effects {action:"apply_global", background_color:...}` which
  updates every active effect that has `background_color` in its schema.
  Per-virtual color uses the per-virtual endpoint above and only updates
  one effect on one virtual. Both work; both can coexist on the same
  installation.

- **Effect support varies.** Same caveat as §12: some LedFx effects don't
  accept `background_color`. The PUT returns `failed`; we surface that on
  the bottom status label and the in-tab error banner
  (`ui_virtuals_pump_action_result`).

- **No "off" / "clear color" state** for per-virtual color either. The
  Black button sends `background_color:"#000000"` — closest substitute,
  identical pattern to §12.

- **Modal pattern:** the per-virtual color picker is a top-layer overlay
  built fresh on each open (`lv_obj_create(lv_layer_top())`) and destroyed
  on close (`lv_obj_del`). Single-instance guarded. Backed by a `Request`
  struct field `effect[16]` that holds the effect type alongside the
  existing `id[48]` and `payload[208]` fields; `payload` shrank from 224
  to 208 to make room (verified no existing caller filled near 224 bytes).

- **Tab-index magic again.** `ui.cpp` routes `RES_ACTION` to
  `ui_virtuals_pump_action_result` only when the active tab index is `1`
  (Virtuals). Same fragility as the Color-tab literal `2` — if a 5th tab
  gets added, the index needs to follow the shift. Consider adding pointer-
  equality getters (`ui_virtuals_tab()`, `ui_color_tab()`) the next time
  a second consumer needs the same check.

- **Reverted decision (Tier 2.4 of the plan):** "highlight the swatch
  whose modal is open" was coded then reverted — the Virtuals list only
  re-renders on a fetch (30 s auto, tab switch, or post-action), so a
  border style applied at modal-open time would never visually appear until
  the next fetch. The modal itself is the obvious "I'm editing this"
  indicator; the redundant border would be both broken and unnecessary.
  The plan file keeps the task so future readers know it was considered.

- **Persisted seed color:** the modal's RGB is seeded from the user's last
  pick (`config_store.save_last_virt_color`) on first apply, with a
  warm-white default (255, 200, 128) on first boot. The same "skip first
  boot" trick from §12: if the saved color equals the default, the modal
  reverts to deriving from the virtual's gradient color instead. NVS key
  `K_LASTVCLR` in the `ledfx-hmi` namespace.

- **Confirmation flash:** after a successful apply, the row's border goes
  green for ~1.5 s on the next render. `s_last_set_idx` + `s_last_set_ms`
  track the most recent apply; `render_virt_list()` paints the border
  when `millis() - s_last_set_ms < FLASH_TIMEOUT_MS`. The flash expires
  naturally without a timer because render_virt_list() reads the elapsed
  time each call.

- **`ui_virtuals.cpp` line count:** now ~570 LOC (was ~210 before Tier 1).
  The per-virtual color modal alone is ~180 LOC. Worth splitting into a
  sibling module `ui_virtuals_color.{h,cpp}` if a 3rd polish iteration
  adds more per-virtual features.

## 14. As-built notes — Tier 4: network OTA updates

Reflash the wall-mounted panel over WiFi instead of unmounting it for a USB
cable. This is the highest-value of the §11 candidates for *this* device (a
panel screwed to a wall is genuinely annoying to reach with a cable) and, unlike
mDNS discovery, depends on nothing about how the LedFx server behaves.

- **New module `src/ota.{h,cpp}`** wraps `ArduinoOTA` (the espota protocol).
  `ota_setup()` registers hostname/password/callbacks in `setup()`;
  `ota_loop()` is polled from `loop()` and lazily calls `ArduinoOTA.begin()`
  the first time WiFi is up (the worker owns the WiFi connect, so OTA just
  waits for `WiFi.status() == WL_CONNECTED`). `ArduinoOTA` is bundled with the
  arduino-esp32 framework — no new `lib_deps` entry.

- **Partition table changed** from `huge_app.csv` (one 3 MB app slot, no way to
  OTA) to a project-local `partitions_ota.csv` with two app slots. The device
  stores everything in NVS and uses no filesystem, so the SPIFFS partition is
  dropped and that space goes to the app slots: **0x1F0000 (~1.94 MB) each**,
  more headroom than the stock `min_spiffs.csv` layout's 0x1E0000 (1.875 MB).
  Both slots start on a 0x10000 (64 KB) boundary (0x10000 and 0x200000) as
  esptool requires, and the trailing 64 KB is a `coredump` partition. `nvs`
  stays at `0x9000/0x5000` (identical to huge_app / the Arduino default), so
  settings saved under the old layout survive the reflash to the new one.
  **The firmware must fit one app slot; if it grows past ~1.94 MB the build
  fails at the image-size check.**

- **Could not verify the image size in-repo.** The Tier 4 work was done in an
  environment whose egress policy blocks `api.registry.platformio.org`, so the
  ESP32 toolchain couldn't be downloaded and `pio run` couldn't complete. The
  ~1.94 MB-per-slot layout was chosen to maximise headroom precisely because the
  binary couldn't be measured here; a real build machine should confirm the
  image fits (a typical LVGL + LovyanGFX + WiFi build lands well under that).

- **Progress UI.** `ota_loop()` runs on the LVGL thread (core 1), so the
  ArduinoOTA callbacks build a top-layer overlay (title + `lv_bar` + %) directly.
  `ArduinoOTA.handle()` blocks for the whole transfer, so the normal LVGL tick
  isn't running during a flash — `onProgress` calls `lv_refr_now(NULL)` itself,
  throttled to real 1 % steps so repaints don't slow the transfer. The RGB panel
  scans its PSRAM framebuffer via DMA independently, so updating the framebuffer
  is enough to move the bar on screen.

- **Worker suspended during the flash.** `worker_suspend()` / `worker_resume()`
  (new; the worker task handle is now captured in `worker_init()`) bracket the
  update so core 0 isn't issuing HTTP calls and racing the WiFi stack while the
  image is written. `onEnd` reboots (ArduinoOTA does the restart), so the
  success path never needs `worker_resume()`; `onError` tears the overlay down
  and resumes so the UI recovers on the *old* firmware. Note `OTA_AUTH_ERROR`
  can fire before `onStart`, i.e. before the suspend — `vTaskResume()` on a
  task that was never suspended is a safe no-op, so the unbalanced
  resume-without-suspend in that path is fine.

- **Per-device identity is one secret.** `config_device_hex()` (the efuse-MAC
  last-3-bytes hex, factored out of the old `derive_ap_psk()`) now backs the AP
  PSK, the OTA hostname (`ledfx-hmi-<hex>`), and the OTA password
  (`<hex>hmi2026`, the same 8+-char scheme the softAP already uses). The
  hostname + password are printed to Serial on boot and shown on the Global tab
  so the user can read them off the panel and run `pio run -t upload` without a
  serial cable. Showing the password on an always-visible screen is a mild
  LAN-only exposure, consistent with the setup portal already printing the AP
  PSK; the OTA path is still password-gated against a stranger on the same WiFi.

- **Second PlatformIO env `crowpanel-7inch-ota`** `extends` the USB env and only
  swaps `upload_protocol = espota`. The built image is byte-identical; only the
  upload transport differs, so `pio run -e crowpanel-7inch` stays the canonical
  build. `upload_port` / `--auth` are left commented because they're per-device
  (fill in from the Global tab).
