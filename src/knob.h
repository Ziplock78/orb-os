#pragma once
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
    int32_t takeDelta();  // net detents since last call: >0 = turned right (CW), <0 = left
    bool    takePress();  // true if the knob was pushed since last call
}
