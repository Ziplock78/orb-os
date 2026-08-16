#pragma once
// Shared encoder routing: turns one poll's worth of knob input (a signed detent
// delta + a push flag) into app-shell actions. This is the SINGLE source of truth
// for "what the knob does", used identically by the device (main.cpp, real KY-040)
// and the desktop simulator (sim_main.cpp, virtual knob). Keeping it in one place
// is what makes "it behaved right in the sim" mean "it behaves right on the Orb".
//
// Deliberately depends only on app_shell (portable LVGL), so it compiles for both
// the Arduino and the native PlatformIO environments. Screen-wake and diagnostic
// logging stay in the device loop around this call, since they're hardware-only.
namespace input_router {
    // delta: net detents since last poll (>0 = turned right/CW, <0 = left/CCW).
    // pressed: true if the knob was pushed since last poll.
    void dispatch(int delta, bool pressed);
}
