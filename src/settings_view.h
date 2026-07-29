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
}
