#pragma once
#include <lvgl.h>
// Location Info app for the shell. Shows where the device is set to: city / state /
// country, coordinates, temperature, barometric pressure, elevation, and a one-line
// "INTEL" fun fact. Styled to match the WWII Aviator clock face (baked cream dial +
// live text drawn on top). Data is fetched on core0 (adsb_task) and drawn on core1.
namespace locationview {
    // Portable snapshot -- same role as WeatherSnapshot in weather.h. On-device this
    // is filled in by the HTTPS fetch cycle below; the desktop simulator has no
    // network, so it fills one in directly via debugSet().
    struct LocInfo {
        char   city[40];
        char   region[40];
        char   country[8];
        double lat, lon;
        int    tempF;
        float  pressInHg;
        int    altFt;
        char   intel[140];
        bool   hasWx;
        bool   valid;
    };

    void      init();                         // build the screen + canvas (after display::begin())
    lv_obj_t* screen();                       // the app's LVGL screen (hand to app_shell::add)
    void      onEnter();                      // app shown: request a refresh, repaint
    void      onPress();                      // knob push while in-app: force a refresh

    // Fetching runs one HTTPS call per invocation (the device calls pump() once per
    // adsb_task cycle; the desktop simulator has no such heap-pressure constraint and
    // can just call pumpUntilDone()). The three real endpoints (bigdatacloud/open-meteo/
    // wikivoyage) are hit on both platforms — only the transport differs internally.
    void      startRefresh();                 // begin a fetch cycle (geocode -> weather -> intel)
    void      pump(double lat, double lon);   // advance the running cycle by one step
    void      pumpUntilDone(double lat, double lon);   // desktop simulator: run a full cycle synchronously
    bool      hasData();                      // true once we have a valid reading to show
    bool      takeRefresh();                  // consume a pending refresh request (from onEnter/onPress)

    void      debugSet(const LocInfo &info);  // inject a reading directly (used for the initial "ACQUIRING"-skip paint)
}
