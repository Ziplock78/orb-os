#pragma once

// "Please wind the clock using the knob."
//
// The panel that goes up when a theme's mainspring has run out. It is the visible half of
// clock_wind: that file holds the spring, this one asks for it to be wound and shows how
// far round you have got.
//
// WHY A PANEL AND NOT A BADGE ON THE FACE. A stopped clock has to be unmistakable. A small
// indicator would be read as decoration by exactly the person this is aimed at, somebody who
// glances at the Orb from across a room, and the failure mode is that they believe the time.
// The hands stop and the screen says so in words.
//
// NOBODY IS EVER TRAPPED HERE. Winding takes five turns, which is a deliberate amount of
// effort, and a screen that demanded it with no way out would be the exact thing CUT-05
// forbids. A rock still opens the app menu, because winding only counts detents in one
// direction and a reversal is left free to mean what it means everywhere else on the device.

namespace wind_notice {
    // Up on the glass right now.
    bool showing();
    // Called every frame from the main loop: raises the panel when the clock is the app on
    // screen and its mainspring has run out, lowers it when either stops being true.
    void tick();
    // One detent while the panel is up. Winds, clicks, and takes the panel down when the
    // fifth turn lands.
    void turn(int delta);
    // Take it down and repaint what was underneath.
    void dismiss();
}
