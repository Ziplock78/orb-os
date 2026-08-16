#pragma once
#include <lvgl.h>
// "Spy Cam" app for the shell: plays a looping JPEG-frame sequence full-screen,
// like a security-camera feed. Frames live on the microSD card's /spycam_frames/
// folder (see tools/spycam_export.sh) as cam0_000.jpg, cam0_001.jpg, ...
// Only decodes/advances while this screen is active (see spycam_view.cpp).
namespace spycamview {
    void      init();      // check the SD card, build the screen + canvas, start the playback timer
    lv_obj_t* screen();    // the app's LVGL screen (hand to app_shell::add)
    void      onPress();   // knob push: switch to the next camera feed
}
