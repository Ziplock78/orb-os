#include <Arduino.h>
#include "knob.h"

// --- Wiring -----------------------------------------------------------------
// See knob.h for the full header pinout. These three GPIOs are unused by the
// board's onboard peripherals on the standard / -B variant.
static constexpr uint8_t PIN_KNOB_A  = 18;   // encoder CLK / A  (green wire)
static constexpr uint8_t PIN_KNOB_B  = 17;   // encoder DT  / B  (yellow wire)
static constexpr uint8_t PIN_KNOB_SW = 16;   // encoder push switch (active low, orange wire)

// Most KY-040 encoders emit 4 quadrature edges per physical click ("detent").
// If turns feel doubled or halved on real hardware, tune this to 2 or 1.
static constexpr int32_t KNOB_STEPS_PER_DETENT = 4;

static constexpr uint32_t SW_DEBOUNCE_MS = 25;

// Raw quadrature counter, updated only inside the ISR. A 32-bit read is atomic
// on the ESP32 (32-bit core), so poll() can read it without a critical section.
static volatile int32_t s_rawPos  = 0;
static volatile uint8_t s_prevAB  = 0;

// Pending input for the app shell to consume (written + read on the loop thread).
static int32_t s_pendingDelta = 0;
static bool    s_pendingPress = false;

// Standard rotary-encoder quadrature decode table (Ben Buxton style). Index is
// (previous 2-bit AB state << 2) | (current AB state); value is -1, 0 or +1.
static const int8_t kQuadTable[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};

static void IRAM_ATTR knob_isr() {
    uint8_t a  = (uint8_t)digitalRead(PIN_KNOB_A);
    uint8_t b  = (uint8_t)digitalRead(PIN_KNOB_B);
    uint8_t ab = (uint8_t)((a << 1) | b);
    uint8_t idx = (uint8_t)(((s_prevAB << 2) | ab) & 0x0F);
    s_rawPos += kQuadTable[idx];
    s_prevAB  = ab;
}

void knob::begin() {
    pinMode(PIN_KNOB_A,  INPUT_PULLUP);
    pinMode(PIN_KNOB_B,  INPUT_PULLUP);
    pinMode(PIN_KNOB_SW, INPUT_PULLUP);

    // Seed the decoder with the current pin state so the first turn is clean.
    uint8_t a = (uint8_t)digitalRead(PIN_KNOB_A);
    uint8_t b = (uint8_t)digitalRead(PIN_KNOB_B);
    s_prevAB = (uint8_t)((a << 1) | b);

    attachInterrupt(digitalPinToInterrupt(PIN_KNOB_A), knob_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_KNOB_B), knob_isr, CHANGE);

    Serial.println("[knob] ready on GPIO18(A)/17(B)/16(SW) — turn + push");
}

void knob::poll() {
    // --- Rotation: one log line per detent ----------------------------------
    static int32_t s_lastDetent = 0;
    int32_t detent = s_rawPos / KNOB_STEPS_PER_DETENT;
    if (detent != s_lastDetent) {
        int32_t delta = detent - s_lastDetent;
        s_lastDetent = detent;
        s_pendingDelta += delta;
        Serial.printf("[knob] turned %s  (pos=%ld)\n",
                      delta > 0 ? "RIGHT (CW)" : "LEFT (CCW)", (long)detent);
    }

    // --- Button: report a press (active low) with debounce ------------------
    static uint8_t  s_swStable    = HIGH;
    static uint8_t  s_swLast      = HIGH;
    static uint32_t s_swChangedMs = 0;
    uint8_t sw = (uint8_t)digitalRead(PIN_KNOB_SW);
    if (sw != s_swLast) {
        s_swLast = sw;
        s_swChangedMs = millis();
    }
    if ((millis() - s_swChangedMs) > SW_DEBOUNCE_MS && sw != s_swStable) {
        s_swStable = sw;
        if (s_swStable == LOW) {
            s_pendingPress = true;
            Serial.println("[knob] pushed");
        }
    }
}

int32_t knob::takeDelta() {
    int32_t d = s_pendingDelta;
    s_pendingDelta = 0;
    return d;
}

bool knob::takePress() {
    bool p = s_pendingPress;
    s_pendingPress = false;
    return p;
}
