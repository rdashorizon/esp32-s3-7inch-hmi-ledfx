// ---------------------------------------------------------------------------
// touch.cpp — GT911 I2C driver for the CrowPanel 7" HMI
//
// Notes:
// - The boards are sold in V2.0 and V3.0 variants. The GT911 I2C address
//   and SDA/SCL pins are identical (SDA=19, SCL=20, addr=0x14). The only
//   difference is the reset sequence: V2.0 has no reset line, while V3.0
//   uses a PCA9557 IO expander to drive the GT911 reset. We handle both
//   by simply letting the GT911 firmware settle after a short delay.
// - INT is not wired to a GPIO on V2.0, so we use I2C polling
//   (touch_read manually queries the GT911 status register).
// ---------------------------------------------------------------------------
#include "touch.h"
#include "pins.h"
#include <Wire.h>

static uint16_t touch_last_x = 0;
static uint16_t touch_last_y = 0;
static bool     touch_pressed = false;

static uint8_t gt911_addr = TOUCH_ADDR;

static void gt911_write(uint16_t reg, const uint8_t *data, size_t len) {
    Wire.beginTransmission(gt911_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    for (size_t i = 0; i < len; ++i) Wire.write(data[i]);
    Wire.endTransmission();
}

static void gt911_read(uint16_t reg, uint8_t *buf, size_t len) {
    Wire.beginTransmission(gt911_addr);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    Wire.endTransmission();
    Wire.requestFrom((int)gt911_addr, (int)len);
    for (size_t i = 0; i < len; ++i) {
        if (Wire.available()) buf[i] = Wire.read();
    }
}

static bool gt911_probe(void) {
    Wire.beginTransmission((int)gt911_addr);
    if (Wire.endTransmission() != 0) return false;
    uint8_t buf[4] = {0};
    gt911_read(0x8140, buf, 4);
    return (buf[0] != 0x00) || (buf[1] != 0x00) || (buf[2] != 0x00) || (buf[3] != 0x00);
}

void touch_init(void) {
    Wire.begin(TOUCH_I2C_SDA, TOUCH_I2C_SCL, TOUCH_I2C_HZ);
    delay(50);  // GT911 cold-boot settle

    // Some clones ship at 0x5D; try the alternate address if 0x14 is dead.
    if (!gt911_probe()) {
        gt911_addr = 0x5D;
        if (!gt911_probe()) {
            // Neither worked — leave init soft-failed; touch_read will
            // just return "released" forever, the UI is still usable.
            return;
        }
    }

    // Soft-reset the GT911 controller.
    uint8_t cmd = 0x02;
    gt911_write(0x8040, &cmd, 1);
    delay(50);
    cmd = 0x00;
    gt911_write(0x8040, &cmd, 1);
}

void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    // Throttle the I2C poll: GT911 INT isn't wired on V2.0, so we have to
    // poll, but 30 Hz is overkill for finger tracking and burns the I2C bus.
    // 60 Hz is the canonical human-input sample rate; if no new frame is
    // available the GT911 status register returns 0x00 and we keep the last
    // known state.
    static uint32_t last_poll_ms = 0;
    uint32_t now = millis();
    if (now - last_poll_ms < 16) {
        data->point.x = touch_last_x;
        data->point.y = touch_last_y;
        data->state = touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return;
    }
    last_poll_ms = now;

    uint8_t status = 0;
    gt911_read(0x814E, &status, 1);
    // Bit 7 (0x80) = "buffer ready": the GT911 has a fresh frame for us. It
    // raises this on both touch and release (release reports 0 points), and
    // will not report the next frame until we acknowledge this one by writing
    // 0 back to the status register. Clearing it only on touch (points > 0)
    // left the flag set forever after the first release, wedging the input as
    // permanently pressed.
    if (status & 0x80) {
        uint8_t points = status & 0x0F;
        if (points > 0) {
            uint8_t buf[8] = {0};
            gt911_read(0x8150, buf, 8);
            // Point 1 registers: 0x8150 x_low, 0x8151 x_high, 0x8152 y_low,
            // 0x8153 y_high (the track id lives at 0x814F, which we skip).
            // GT911 returns 800x480; LVGL panel is also 800x480.
            touch_last_x = ((uint16_t)buf[1] << 8) | buf[0];
            touch_last_y = ((uint16_t)buf[3] << 8) | buf[2];
            touch_pressed = true;
        } else {
            touch_pressed = false;  // finger lifted
        }
        // Always acknowledge the frame so the GT911 reports the next one.
        uint8_t clear = 0;
        gt911_write(0x814E, &clear, 1);
    }
    // No new frame: keep the previous pressed state / coordinates.

    data->point.x = touch_last_x;
    data->point.y = touch_last_y;
    data->state = touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}
