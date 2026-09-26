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
    void      openNoSdCardNotice(bool alsoNeedsWifi);   // UX-024; push dismisses

    // Called once from main.cpp's setup() when a Launch Kit push left a custom
    // splash active — jumps straight to the About page (the same splash art,
    // held indefinitely) so a just-pushed design doesn't just flash and vanish.
    void      openAboutPage();
    // The text a row of the theme picker is showing right now, or nullptr past the end.
    // For the simulator's self-test, which asserts that the rows draw their names.
    const char *designRowText(int i);

    // THE CITY SEARCH KEYBOARD, for the self-test, and for nothing else.
    //
    // searchKeys() is the strip of characters the ring offers, and searchType(c) presses
    // the one whose character is `c` and returns what the search box then holds. Together
    // they let the test press every key and read the answer, which is what would have
    // caught the comma landing on the backspace in 2.16.27: the keys were dispatched by
    // POSITION, so one new character moved every special key one place along.
    const char *searchKeys();
    const char *searchType(char c);
    // The Location page's two readouts, for the self-test: openLocationPage() puts the
    // Orb on that page through the real show_page(), and locCityText() answers what the
    // name line is showing, or nullptr when it is hidden because nothing has named the
    // position. The coordinates line is separate and has always been there.
    void openLocationPage();
    const char *locCityText();

    // How to reach the web config page (IP / hostname / setup AP). Shown on the About
    // page alongside the firmware version. Both moved here from the old touch-only
    // Stats screen. Safe to call every loop: it only redraws while About is open.
    void      setNetInfo(const char *line);

    // The centre point the scope is using, shown under the Location page's title. It moved
    // off the splash, where it was the longest of three lines and was not an address at
    // all: somebody checking whether their location took looks in Location, not at a boot
    // screen that is gone in three seconds. `set` false draws "not set yet" rather than
    // 0.00000, 0.00000, which is a real place in the Gulf of Guinea.
    void      setHomeCoords(double lat, double lon, bool set);
}
