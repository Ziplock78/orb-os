#pragma once
#include <lvgl.h>
// Settings app for the shell. A knob-driven menu: turn to scroll items, push to
// select. First item is Brightness (turn to adjust, push to save). "Back" leaves.
// While this app is active it CAPTURES the knob (turns scroll the menu instead of
// switching apps); selecting Back releases it and returns to the first app.
namespace settingsview {
    void      init();
    lv_obj_t* screen();
    void      onTurn(int delta);   // knob turn while captured
    void      onPress();           // knob push
    void      onEnter();           // entered from the app switcher: reset to the menu
    void      onExit();            // leaving Settings: release the text canvas's PSRAM

    // Called once from main.cpp's setup() after a fresh boot or a Reset — jumps straight
    // into WiFi setup with a first-run prompt instead of the normal menu/hint.
    void      openWifiSetupPrompt();

    // Called once from main.cpp's setup() when a Launch Kit push left a custom
    // splash active — jumps straight to the About page (the same splash art,
    // held indefinitely) so a just-pushed design doesn't just flash and vanish.
    void      openAboutPage();

    // How to reach the web config page (IP / hostname / setup AP). Shown on the About
    // page alongside the firmware version. Both moved here from the old touch-only
    // Stats screen. Safe to call every loop: it only redraws while About is open.
    void      setNetInfo(const char *line);
}
