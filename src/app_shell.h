#pragma once
#include <lvgl.h>
// The app shell: the "channel changer". Holds an ordered list of full-screen
// apps (each is one LVGL screen) and flips between them when the knob turns.
// Turn right -> next app, turn left -> previous app; the list wraps around.
// Per-app knob handlers. onPress = a push; onTurn = a detent while the app has
// "captured" the knob (used by menus that scroll with the knob instead of switching
// apps). An app that captures the knob keeps it until it calls next()/prev() itself.
typedef void (*app_action_t)();
typedef void (*app_turn_t)(int delta);

namespace app_shell {
    void add(lv_obj_t *screen, const char *name,
             app_action_t onPress = nullptr, app_turn_t onTurn = nullptr, bool capture = false,
             app_action_t onEnter = nullptr);
    void add_active(const char *name,
                    app_action_t onPress = nullptr, app_turn_t onTurn = nullptr, bool capture = false,
                    app_action_t onEnter = nullptr);
    void begin();                                  // show the first app (no animation)

    void next();          // advance to the next app (knob right), slides left
    void prev();          // go to the previous app (knob left), slides right
    void pressCurrent();  // knob pushed: run the current app's press handler, if any

    // App-switcher overlay: turning shows a big app-name label over a live preview;
    // the first turn opens the switcher on the current app, further turns cycle apps,
    // and a push commits (hides the overlay) into the shown app.
    void browseTurn(int delta);
    void browsePress();
    bool browsing();       // is the switcher overlay currently up?
    void openSwitcher();   // reopen the switcher on the current app (e.g. from a menu's Back)

    bool captured();               // does the current app own the knob (menu mode)?
    void setCaptured(bool on);     // an app grabs (true) / releases (false) the knob at runtime
    void turnCurrent(int delta);   // deliver a detent to the captured app

    int         count();
    int         index();
    const char *name();
}
