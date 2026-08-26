#pragma once
#include <stdint.h>   // int32_t/uint32_t (on device these come via Arduino.h; be self-contained for the sim)
// Rotary encoder (KY-040) on the 8-pin header (H2).
//
// Stage 2 scope: read the knob and PRINT what it does to the serial log so we can
// confirm the hardware works. No UI yet. Stage 3 turns these events into app
// switching + LVGL focus control.
//
// Wiring (standard / -B variant, where GPIO16/17/18 are free):
//   Encoder CLK (A)  -> GPIO18  (green)   swapped with SW to avoid a ribbon twist
//   Encoder DT  (B)  -> GPIO17  (yellow)
//   Encoder SW (push)-> GPIO16  (orange)  swapped with CLK to avoid a ribbon twist
//   Encoder +        -> 3V3   (header pin 3, NOT the 5V pin)  (red)
//   Encoder GND      -> GND   (header pin 2)  (brown/white)
namespace knob {
    void begin();   // configure pins + interrupts; call once from setup()
    void poll();    // call every loop(); prints turn/press events to Serial

    // Consumed by the app shell. Each returns pending input and then clears it,
    // so call once per loop after poll().
    int32_t takeDelta();     // net detents since last call: >0 = turned right (CW), <0 = left
    bool    takePress();     // true if the knob was pushed since last call
    bool    takeLongPress(); // true once when the knob was held long enough (recovery reboot)

    // Raw encoder tick count (not detent-divided), updated live by the ISR regardless
    // of whether poll() has run — safe to read anytime, including from inside code that
    // blocks the main loop (e.g. Spy Cam's SD bulk load), to detect a turn that happened
    // *during* the block. takeDelta()'s bookkeeping only advances when poll() runs, so
    // it can't see that; compare rawPosition() against a snapshot taken before blocking.
    int32_t rawPosition();

    // Non-consuming peek at the press flag the ISR sets — same rationale as
    // rawPosition(), for the same blocking-load use case, but for a push instead of a
    // turn. Doesn't clear it: leave that to the normal main-loop takePress() dispatch.
    bool pendingPress();

    // The Rock gesture: a leftward detent immediately followed by a rightward one.
    //
    // Detected HERE rather than by comparing two timestamps in the caller, and the reason is
    // worth keeping. Two per-direction timestamps cannot answer "which came first" when both
    // land inside the same millisecond, which they routinely do — detents are stamped in
    // poll(), and consecutive polls can share a tick. Comparing them made left-then-right and
    // right-then-left identical, so the gesture fired on any fast reversal. Order is a
    // property of the sequence, so it has to be recorded as the sequence happens.
    //
    // It also cannot be built on takeDelta(), which reports NET movement and so cancels this
    // motion to zero before anything sees it.
    //
    // lastRockMs: millis() of the rightward detent that completed the most recent reversal,
    // or 0 if there has not been one. A caller acts on it once and remembers the value; a new
    // gesture produces a new timestamp.
    // lastRockGapMs: how long that reversal took, so the caller owns the "quickly" part.
    uint32_t lastRockMs();
    uint32_t lastRockGapMs();

    uint32_t heldMs();       // how long the button has been continuously held right now (0 if up)
    uint32_t longPressMs();  // the hold duration that triggers takeLongPress() (for a countdown UI)
}
