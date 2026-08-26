#pragma once
#include <lvgl.h>
// A simple full-screen clock, living on its very own LVGL screen so it stays
// completely independent of the radar UI. App two in the shell.
namespace clockview {
    void      init();      // build the clock screen; call once after display::begin()
    lv_obj_t* screen();    // the clock's LVGL screen (hand this to app_shell::add)
    void      onExit();    // shell is switching away: drop the custom face's decoded PSRAM
}
