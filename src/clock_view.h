#pragma once
#include <lvgl.h>
// A simple full-screen clock, living on its very own LVGL screen so it stays
// completely independent of the radar UI. App two in the shell.
namespace clockview {
    void      init();      // build the clock screen; call once after display::begin()
    lv_obj_t* screen();    // the clock's LVGL screen (hand this to app_shell::add)
    // The art contract every screen on this device is supposed to keep, and this one did
    // not: take PSRAM when the app is shown, give it back when it is not.
    //
    // The clock's canvas and its plate-rotation cache are 466x466 buffers, and they were
    // allocated in init() and held for the life of the device. Measured at boot, clockview
    // took 2,460 KB before anything had been looked at, and the weather radar, which
    // initialises last, was left with 444 KB and could allocate ONE of its five frame
    // buffers. A screen nobody is looking at should not be holding the memory a screen
    // somebody IS looking at needs.
    void      onEnter();   // shell is switching to us: take the canvas back
    void      onExit();    // shell is switching away: give it up, plus the decoded face
    void      refresh();   // redraw the face now, for coming back from something that covered it
}
