// QMI8658 accelerometer over I2C (Arduino). Shares the bus with the touch (SDA/SCL
// in config.h). We only need gravity on the Z axis to detect a face-down board.
#include "imu_qmi8658.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>

#define QMI_WHOAMI  0x00   // -> 0x05
#define QMI_CTRL1   0x02   // serial interface / addr auto-increment
#define QMI_CTRL2   0x03   // accel: full-scale + ODR
#define QMI_CTRL7   0x08   // sensor enable
#define QMI_AZ_L    0x39   // accel Z low byte (high byte at 0x3A)

// ±2g full scale -> 16384 LSB/g.
#define FACEDOWN_THRESHOLD (9000)   // ~0.55 g past zero on the side opposite the resting one

static uint8_t s_addr = 0x6B;
static bool    s_ok   = false;

static bool wr(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(s_addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool rd(uint8_t reg, uint8_t *buf, uint8_t len) {
    Wire.beginTransmission(s_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;   // repeated start
    if (Wire.requestFrom(s_addr, len) < len) return false;
    for (uint8_t i = 0; i < len; ++i) buf[i] = Wire.read();
    return true;
}

bool imu_begin() {
    // DO NOT REMOVE, and do not move imu_begin() later in setup(). This is now the ONLY
    // Wire.begin() in the firmware, so it is what brings up the shared I2C bus for every
    // other device on it: AXP2101 (battery), PCF85063 (RTC), ES8311 (audio). It used to
    // be a harmless duplicate because touch_begin() ran first and did the same thing, but
    // touch was removed (see docs/ARCHITECTURE.md) and took that call with it. main.cpp's
    // setup() calls imu_begin() immediately before battery_begin()/rtc_begin()/
    // audio_begin(), which is what keeps the ordering correct. Note this still runs even
    // when no IMU is fitted: the probe below fails, the bus stays up.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
    const uint8_t addrs[2] = { 0x6B, 0x6A };
    for (uint8_t i = 0; i < 2; ++i) {
        s_addr = addrs[i];
        uint8_t who = 0;
        if (rd(QMI_WHOAMI, &who, 1) && who == 0x05) {
            wr(QMI_CTRL1, 0x40);   // address auto-increment
            wr(QMI_CTRL2, 0x06);   // accel: ±2 g, ~125 Hz
            wr(QMI_CTRL7, 0x01);   // enable accelerometer
            Serial.printf("[imu] QMI8658 ready at 0x%02X\n", s_addr);
            s_ok = true;
            return true;
        }
    }
    Serial.println("[imu] QMI8658 not found");
    s_ok = false;
    return false;
}

// 1 = face-down, 0 = not, -1 = couldn't read (shared I2C bus is noisy — a failed read
// must NOT be treated as "not face-down", or it keeps resetting the debounce counter and
// the screen never sleeps).
int imu_facedown() {
    if (!s_ok) return -1;
    uint8_t b[2];
    if (!rd(QMI_AZ_L, b, 2)) return -1;
    const int16_t az = (int16_t)((b[1] << 8) | b[0]);

    // Orientation-agnostic: we don't assume which Z sign is "up" (it depends on how the
    // board sits in the case, and it isn't the same on every unit). Track a slow baseline
    // of the *resting* az and call it face-down when gravity swings to the far OPPOSITE
    // side — i.e. the device was flipped over. Works whatever the sensor's sign.
    static int32_t ref = 0; static bool haveRef = false;
    if (!haveRef) { ref = az; haveRef = true; }
    const bool down = (ref < 0) ? (az > FACEDOWN_THRESHOLD) : (az < -FACEDOWN_THRESHOLD);
    if (!down) ref += (az - ref) >> 4;   // EMA toward the current (not-face-down) orientation
    return down ? 1 : 0;
}
