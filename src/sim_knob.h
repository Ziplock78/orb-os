#pragma once
#include <stdint.h>
// Simulator-only encoder backend. The device reads a KY-040 via GPIO interrupts
// (knob.cpp, Arduino-only). On the desktop we can't do that, so this file provides
// the SAME knob:: consumed-input API (declared in knob.h) driven by injected SDL
// events instead. sim_main.cpp feeds it turns/pushes from the event loop; the app
// shell then consumes it through the identical knob:: + input_router path the
// device uses, so behaviour matches.
//
// Only compiled in the `native` PlatformIO env (excluded from the device build,
// which uses the real knob.cpp).
namespace simknob {
    void injectTurn(int detents);              // +1 = one detent right/CW, -1 = left/CCW
    void injectPress(bool down, uint32_t now_ms);  // button edge (press fires on the down edge, like hardware)
    void tick(uint32_t now_ms);                // advance hold timing; call once per frame for long-press
}
