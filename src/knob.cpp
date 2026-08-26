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

static constexpr uint32_t SW_DEBOUNCE_MS = 200;  // min time between accepted presses. Wide on purpose:
                                                  //   this switch bounces heavily, and 200ms is still far
                                                  //   faster than anyone deliberately selects menu items,
                                                  //   so one physical click can't become two/three.
static constexpr uint32_t LONG_PRESS_MS  = 8000;   // hold to force a recovery reboot (long enough
                                                    // that incidental contact while touching the
                                                    // screen next to the knob can't trigger it)

// Raw quadrature counter, updated only inside the ISR. A 32-bit read is atomic
// on the ESP32 (32-bit core), so poll() can read it without a critical section.
static volatile int32_t s_rawPos  = 0;
static volatile uint8_t s_prevAB  = 0;

// Pending input for the app shell to consume (written + read on the loop thread).
static int32_t s_pendingDelta = 0;
static int      s_lastDir    = 0;    // -1 left, +1 right, 0 = nothing turned yet
static uint32_t s_lastDirMs  = 0;
static uint32_t s_rockMs     = 0;    // the rightward detent that completed a left->right
static uint32_t s_rockGapMs  = 0;
static volatile bool s_pendingPress = false;
static volatile bool s_pendingLong  = false;

// Button edge + debounce lives in the ISR (like rotation), so a quick tap is caught
// the instant it happens even if the main loop is busy (e.g. Spy Cam decoding a
// frame). Polling digitalRead() once per loop() can miss a press-and-release that
// both happen inside one slow loop iteration; a hardware interrupt cannot.
static volatile uint32_t s_swLastEdgeMs  = 0;
static volatile uint8_t  s_swLevel       = HIGH;
static volatile uint32_t s_swPressStartMs = 0;
static volatile bool     s_swLongFired    = false;

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

// Debounced in the ISR. A RELEASE (rising edge) is always accepted so s_swLevel can
// never get "stuck" LOW (an earlier bug counted a phantom hold toward the long-press
// reboot) — but it MUST also stamp s_swLastEdgeMs. That's the key: without stamping
// the release, the switch's release bounce (open/closed/open) produced falling edges
// that landed >SW_DEBOUNCE_MS after the original press and each registered as a NEW
// press, so one physical click typed 2-3 characters. A PRESS (falling edge) is only
// accepted once SW_DEBOUNCE_MS has passed since ANY edge (press or release), which
// rejects both press bounce and the tail of a just-released button.
static void IRAM_ATTR knob_sw_isr() {
    const uint32_t now = millis();
    const uint8_t lvl = (uint8_t)digitalRead(PIN_KNOB_SW);
    if (lvl == HIGH) {
        s_swLevel = HIGH;           // release: trust instantly, never gets "stuck"...
        s_swLastEdgeMs = now;       // ...but timestamp it so the release bounce is debounced too
        return;
    }
    if (now - s_swLastEdgeMs < SW_DEBOUNCE_MS) return;   // reject bounce (press- and release-side)
    s_swLastEdgeMs = now;
    s_swLevel = LOW;
    s_pendingPress    = true;
    s_swPressStartMs  = now;
    s_swLongFired     = false;
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

    s_swLevel = (uint8_t)digitalRead(PIN_KNOB_SW);
    attachInterrupt(digitalPinToInterrupt(PIN_KNOB_SW), knob_sw_isr, CHANGE);

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
        // Watch for a left-then-right reversal as it happens. See knob.h for why this is
        // recorded in sequence rather than reconstructed from timestamps afterwards.
        const int dir = delta > 0 ? 1 : -1;
        const uint32_t now = millis();
        if (s_lastDir == -1 && dir == 1) {
            s_rockGapMs = now - s_lastDirMs;
            s_rockMs    = now ? now : 1;   // never 0, which means "never happened"
        }
        // EVERY reversal, with the gap that decides whether it counts as a Rock.
        //
        // ROCK_WINDOW_MS was a guess, and a guess is exactly the wrong kind of number for
        // this: too tight and a real rock is ignored, too loose and ordinary browsing opens
        // the menu by accident. Both failures were reported. This prints the measurement so
        // the threshold can be set from what this owner's hand and this knob actually do,
        // rather than from what felt plausible in an editor.
        //
        // Only on a reversal, so it is quiet during ordinary turning in one direction.
        if (s_lastDir != 0 && dir != s_lastDir) {
            Serial.printf("[knob] REVERSAL %s->%s gap=%lums%s\n",
                          s_lastDir > 0 ? "R" : "L", dir > 0 ? "R" : "L",
                          (unsigned long)(now - s_lastDirMs),
                          (s_lastDir == -1 && dir == 1) ? "  (counts as a rock candidate)" : "");
        }
        s_lastDir   = dir;
        s_lastDirMs = now;
        Serial.printf("[knob] turned %s  (pos=%ld)\n",
                      delta > 0 ? "RIGHT (CW)" : "LEFT (CCW)", (long)detent);
    }

    // --- Button: press is detected in the ISR (see knob_sw_isr); here we only
    // watch for a sustained hold, which by definition spans many poll() calls,
    // so sampling it once per loop is fine (unlike catching the initial edge).
    if (s_swLevel == LOW && !s_swLongFired && (millis() - s_swPressStartMs) >= LONG_PRESS_MS) {
        s_swLongFired = true;
        s_pendingLong = true;
        Serial.println("[knob] long-press (reboot)");
    }
}

int32_t knob::takeDelta() {
    int32_t d = s_pendingDelta;
    s_pendingDelta = 0;
    return d;
}

int32_t knob::rawPosition() { return s_rawPos; }

uint32_t knob::lastRockMs()    { return s_rockMs; }
uint32_t knob::lastRockGapMs() { return s_rockGapMs; }

bool knob::takePress() {
    bool p = s_pendingPress;
    s_pendingPress = false;
    return p;
}

bool knob::pendingPress() { return s_pendingPress; }

bool knob::takeLongPress() {
    bool p = s_pendingLong;
    s_pendingLong = false;
    return p;
}

uint32_t knob::heldMs() {
    return (s_swLevel == LOW) ? (millis() - s_swPressStartMs) : 0;
}

uint32_t knob::longPressMs() { return LONG_PRESS_MS; }
