#pragma once
#include <lvgl.h>

// Standalone Intel app for the shell: the world's headlines, three at a time by default,
// on a screen that is only ever read at a glance. Reached by the knob like Clock/Weather.
namespace intelview {
    void      init();       // build the screen (core 1 / LVGL)
    lv_obj_t* screen();
    void      onPress();    // knob push: fetch now rather than waiting out the poll

    // Network step, driven from adsb_task (core 0). Does NO LVGL work.
    // Returns true when a fresh set landed, so the caller can ask for a redraw.
    bool fetchStep();

    void onHeadlinesReady();  // core 1 (from loop()): a fresh set is in the store
    void tick();              // core 1: refresh the "x min ago" line
}
