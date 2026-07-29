#pragma once
#include <lvgl.h>
// Standalone Weather app for the shell: an ANIMATED precipitation radar (loops the
// last ~10 RainViewer frames) centred on the home location, plus current temp.
// Reached by the knob like Clock/Settings — no touch-swipe needed.
namespace weatherview {
    void      init();       // build screen + start the animation timer (core 1 / LVGL)
    lv_obj_t* screen();
    void      onPress();    // knob push (reserved)

    // Incremental fetch, driven from adsb_task (core 0). Does NO LVGL work.
    // One network step per call so it never stalls the live ADS-B feed.
    // Returns: 0 = nothing/failed, 1 = made progress (call again soon), 2 = full set ready.
    int  fetchStep(double lat, double lon);

    void onFramesReady();   // core 1 (from loop()): a fresh frame set is ready
}
