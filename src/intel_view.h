#pragma once
#include <lvgl.h>

// Standalone Intel app for the shell: the world's headlines, three at a time by default,
// on a screen that is only ever read at a glance. Reached by the knob like Clock/Weather.
namespace intelview {
    void      init();       // build the screen (core 1 / LVGL)
    lv_obj_t* screen();
    // Knob push. When the headlines all fit: fetch now rather than waiting out the poll.
    // When the theme's type size overflows the dial: toggle scroll mode instead — the
    // same press-to-own-the-knob grammar the Flight Tracker's aircraft selection uses.
    void      onPress();
    // A detent while this screen owns the knob (scroll mode only): step the window.
    void      onTurn(int delta);
    // Entering from the switcher: back to the top, scroll mode released.
    void      onEnter();

    // Network step, driven from adsb_task (core 0). Does NO LVGL work.
    // Returns true when a fresh set landed, so the caller can ask for a redraw.
    bool fetchStep();

    void onHeadlinesReady();  // core 1 (from loop()): a fresh set is in the store
    void tick();              // core 1: refresh the "x min ago" line

    // Scroll window readout, for the simulator's selftest: first visible item, how many
    // fit, and how many the snapshot holds. Nothing on the device calls this.
    void scrollState(int &first, int &visible, int &count);
}
