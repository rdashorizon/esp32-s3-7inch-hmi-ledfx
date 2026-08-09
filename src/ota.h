// ---------------------------------------------------------------------------
// ota.h — network firmware updates (ArduinoOTA / espota)
//
// Tier 4. A wall-mounted panel is awkward to reach with a USB cable, so this
// lets you reflash it over WiFi:
//
//   pio run -e crowpanel-7inch-ota -t upload --upload-port <device-ip>
//
// (or pick the "ledfx-hmi-<hex>" target in the Arduino IDE's network-ports
// list). The upload is password-protected with the per-device OTA password
// (see config_ota_password()); both the hostname and password are printed to
// Serial on boot and shown on the Global tab.
//
// Threading: ota_loop() is polled from the Arduino loop() on core 1 — the same
// thread that runs LVGL — so the progress overlay it draws is safe to build
// from the ArduinoOTA callbacks. During an update the background network
// worker (core 0) is suspended so it isn't hammering the WiFi stack while the
// image is written to flash. Requires an OTA-capable partition table (two app
// slots) — see partitions_ota.csv.
// ---------------------------------------------------------------------------
#pragma once

// Configure ArduinoOTA (hostname, password, progress callbacks). Call once in
// setup(), after the display/UI exist so the progress overlay can be drawn.
// This does NOT start the listener — that waits until WiFi is actually up,
// which ota_loop() handles lazily.
void ota_setup();

// Poll from loop(). Brings the OTA listener up once WiFi connects, then
// services incoming update requests. Cheap when idle (a non-blocking UDP
// poll); blocks for the duration of an actual update, driving the on-screen
// progress bar from the ArduinoOTA callbacks.
void ota_loop();
