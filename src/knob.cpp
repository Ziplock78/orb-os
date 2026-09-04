#include <Arduino.h>
#include "knob.h"
#include <esp_timer.h>   // esp_timer_get_time() — IRAM-safe, unlike millis() from an ISR
#include "display.h"     // orb_log_quiet(): see the note there on what printing costs

// --- Wiring -----------------------------------------------------------------
// See knob.h for the full header pinout. These three GPIOs are unused by the
// board's onboard peripherals on the standard / -B variant.
static constexpr uint8_t PIN_KNOB_A  = 18;   // encoder CLK / A  (green wire)
static constexpr uint8_t PIN_KNOB_B  = 17;   // encoder DT  / B  (yellow wire)
static constexpr uint8_t PIN_KNOB_SW = 16;   // encoder push switch (active low, orange wire)

// Most KY-040 encoders emit 4 quadrature edges per physical click ("detent").
// If turns feel doubled or halved on real hardware, tune this to 2 or 1.
static constexpr int32_t KNOB_STEPS_PER_DETENT = 4;

// How many detents the leftward half of a rock may span before it stops being a rock.
// Two, so a slightly heavy flick still counts, but a deliberate scroll does not.
static constexpr int ROCK_MAX_RUN = 2;

// A rock has to be humanly possible. Below the minimum it is contact bounce; above the
// maximum it is two separate decisions. input_router applies no timing of its own.
static constexpr uint32_t ROCK_MIN_GAP_MS = 45;
static constexpr uint32_t ROCK_MAX_GAP_MS = 900;

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

// The Rock, detected IN THE ISR.
//
// It used to be worked out in poll(), from the net change in position since the last call.
// That threw whole rocks away. poll() sees one number, and a left detent followed by a right
// one between two calls nets to zero: the position is back where it started, `detent !=
// s_lastDetent` is false, and the rock never happened as far as anything downstream knows.
// The interrupt had caught both detents perfectly; poll() collapsed them.
//
// Which made it a frame-rate bug wearing an input bug's clothes, and explains the report
// that it worked from every app except the Clock. The Clock rotates a 539 KB minute hand and
// a 294 KB hour hand with shadows every frame, so its loop iteration is the longest on the
// device and poll() runs least often there. Widest window, most rocks swallowed.
//
// Here every detent is seen as it happens, with its own timestamp, and nothing can cancel
// out. esp_timer_get_time rather than millis() because this runs from IRAM.
static volatile int      s_isrLastDir   = 0;
static volatile uint32_t s_isrLastDirMs = 0;
// How many detents the current direction has run for. A rock is a small deliberate wiggle,
// so the turn INTO it has to be small: without this, five detents left followed by one
// right counted as a rock, which is just ordinary browsing and is why the menu appeared to
// open on any turn at all.
static volatile int      s_isrRunLen    = 0;
static volatile int32_t  s_anchor       = 0;   // rawPos at the last COMMITTED detent
static volatile int32_t  s_detent       = 0;   // committed detents; the one true stream
static volatile uint32_t s_rockMs       = 0;   // the detent that completed a reversal, either way
static volatile uint32_t s_rockGapMs    = 0;
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

// ONE detent stream, committed with hysteresis, feeding everything.
//
// This is the fix for a menu that opened by itself. The detent used to be derived as
//
//     const int32_t det = s_rawPos / KNOB_STEPS_PER_DETENT;
//
// which has no hysteresis. A knob resting ON a boundary needs only one step of mechanical
// or electrical dither to flip that value back and forth, 8 -> 7 -> 8, and each flip looked
// like a change of direction a couple of milliseconds apart with a run length of one: the
// exact shape of a rock. So stopping mid-turn opened the app menu, which is what "I turned
// it three times and it went to the menu" was. Truncating division is also asymmetric about
// zero (both 0..3 and -3..0 map to 0), a second bug in the same line.
//
// This never mattered before the Rock existed, because poll() only ever looked at NET
// movement since the last call and that quietly absorbed the dither. Moving detection into
// the ISR to stop losing whole rocks removed the filter along with the problem.
//
// So: a detent is committed only after a FULL detent of travel from the last committed
// position, in either direction. Dither of one to three steps commits nothing, ever. And
// poll() reads the same counter, so the stream that decides a rock and the stream that
// moves an app can no longer disagree about what happened.
static void IRAM_ATTR on_detent(int dir) {
    s_detent += dir;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // A rock is a SHORT turn one way, then straight back, as one gesture.
    //   run length : the turn into the reversal was a flick, not a scroll
    //   gap        : one gesture rather than two decisions
    //   minimum gap: a hand cannot reverse in five milliseconds. Anything faster is the
    //                encoder, not the person, and treating it as input is what let a
    //                resting knob open the menu.
    //
    // EITHER direction. This used to require left-then-right specifically, on the reasoning
    // that one fixed order halves the reversals that can trigger it for no cost in how hard
    // the gesture is to perform. The cost turned out to be real and just unmeasured: the
    // owner reaches for the menu without thinking about which way his hand goes first, so
    // half of his attempts did nothing and the gesture read as unreliable. Changed
    // 2026-08-29 to satisfy UX-011, which is written the way the hand actually moves.
    //
    // The margin that was given up is covered by what remains: the run-length rule, which is
    // what actually stopped ordinary browsing from qualifying, the gap window at both ends,
    // and the fact that input_router does not test for a rock at all while the menu is
    // already open, so a false positive can only ever happen inside an app.
    if (s_isrLastDir != 0 && dir != s_isrLastDir && s_isrRunLen <= ROCK_MAX_RUN) {
        const uint32_t gap = now - s_isrLastDirMs;
        if (gap >= ROCK_MIN_GAP_MS && gap <= ROCK_MAX_GAP_MS) {
            s_rockGapMs = gap;
            s_rockMs    = now ? now : 1;   // never 0, which means "never happened"
        }
    }
    s_isrRunLen    = (dir == s_isrLastDir) ? s_isrRunLen + 1 : 1;
    s_isrLastDir   = dir;
    s_isrLastDirMs = now;
}

static void IRAM_ATTR knob_isr() {
    uint8_t a  = (uint8_t)digitalRead(PIN_KNOB_A);
    uint8_t b  = (uint8_t)digitalRead(PIN_KNOB_B);
    uint8_t ab = (uint8_t)((a << 1) | b);
    uint8_t idx = (uint8_t)(((s_prevAB << 2) | ab) & 0x0F);
    s_rawPos += kQuadTable[idx];
    s_prevAB  = ab;

    while (s_rawPos - s_anchor >= KNOB_STEPS_PER_DETENT) { s_anchor += KNOB_STEPS_PER_DETENT; on_detent(+1); }
    while (s_anchor - s_rawPos >= KNOB_STEPS_PER_DETENT) { s_anchor -= KNOB_STEPS_PER_DETENT; on_detent(-1); }
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
    // The SAME committed stream the ISR builds, not a second derivation from rawPos. Two
    // derivations meant two answers about what the knob had done, and the one that decided
    // rocks had no hysteresis.
    static int32_t s_lastDetent = 0;
    const int32_t detent = s_detent;
    if (detent != s_lastDetent) {
        int32_t delta = detent - s_lastDetent;
        s_lastDetent = detent;
        s_pendingDelta += delta;
        // The rock is NOT decided here any more; the ISR owns it. This is only the log,
        // and the log is deliberately kept at poll resolution so the two can be compared:
        // a reversal the ISR recorded that never appears here is a rock this code used to
        // lose entirely.
        const int dir = delta > 0 ? 1 : -1;
        const uint32_t now = millis();
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
            // Annotated from the GAP, which poll() owns. Direction no longer narrows
            // anything, and the other gate is the ISR's run length, which cannot be read
            // from here without racing the detent that produced this line. The gap is the
            // number this log exists to expose anyway: it is what gets tuned.
            const uint32_t gap = now - s_lastDirMs;
            Serial.printf("[knob] REVERSAL %s->%s gap=%lums%s\n",
                          s_lastDir > 0 ? "R" : "L", dir > 0 ? "R" : "L",
                          (unsigned long)gap,
                          (gap >= ROCK_MIN_GAP_MS && gap <= ROCK_MAX_GAP_MS)
                              ? "  (in the rock window; run length decides)" : "");
        }
        s_lastDir   = dir;
        s_lastDirMs = now;
        if (!orb_log_quiet())
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
