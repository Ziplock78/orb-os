#pragma once
#include <lvgl.h>
// Location Info app for the shell. Shows where the device is set to: city / state /
// country, coordinates, temperature, barometric pressure, elevation, and a one-line
// "INTEL" fun fact. Styled to match the WWII Aviator clock face (baked cream dial +
// live text drawn on top). Data is fetched on core0 (adsb_task) and drawn on core1.
namespace locationview {
    void      init();                         // build the screen + canvas (after display::begin())
    lv_obj_t* screen();                       // the app's LVGL screen (hand to app_shell::add)
    void      onEnter();                      // app shown: request a refresh, repaint
    void      onPress();                      // knob push while in-app: force a refresh

    // Fetching runs on core0 (adsb_task), one HTTPS call per cycle:
    void      startRefresh();                 // begin a fetch cycle (geocode -> weather -> intel)
    void      pump(double lat, double lon);   // advance the running cycle by one step
    bool      hasData();                      // true once we have a valid reading to show
    bool      takeRefresh();                  // consume a pending refresh request (from onEnter/onPress)
}
