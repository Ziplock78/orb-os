#pragma once
#include "config.h"   // APPS_LAUNCH_ONE — which apps this build carries
#include <lvgl.h>
// The app shell: the "channel changer". Holds an ordered list of full-screen
// apps (each is one LVGL screen) and flips between them when the knob turns.
// Turn right -> next app, turn left -> previous app; the list wraps around.
// Per-app knob handlers. onPress = a push; onTurn = a detent while the app has
// "captured" the knob (used by menus that scroll with the knob instead of switching
// apps). An app that captures the knob keeps it until it calls next()/prev() itself.
// onExit fires right before the shell switches away to a different app — the
// counterpart to onEnter — so an app can drop memory it only needs while shown
// (e.g. a custom clock face's decoded PSRAM sprites) and reclaim it for whichever
// app is coming to the front.
typedef void (*app_action_t)();
typedef void (*app_turn_t)(int delta);

// --- residency: which screens exist, as opposed to which one is shown ---------------
//
// An app registered with addLazy() does not exist until somebody looks at it. `build`
// constructs the screen and returns it — the same work the old init() did at boot — and
// `destroy` deletes it and puts every pointer it kept back to null. build may be called
// again after destroy, so it has to be re-runnable rather than once-only.
//
// Why: every screen used to be built at boot and kept for the life of the device. Measured
// 2026-10-06 on a themed Orb, the four screens held 1,936 live LVGL allocations and 71 KB
// of INTERNAL RAM, of which exactly one screen was ever on the dial. Settings alone was 909
// allocations and 24 KB, for the screen reached least often. The onEnter/onExit contract in
// docs/memory.md already releases a screen's ART on exit and works — but art is PSRAM, and
// PSRAM is not the pool that runs out. The object trees are internal, and nothing released
// them. That contract now extends to the objects.
//
// `pinned` keeps a screen resident for ever: that is the Clock, because it is the screen
// people come back to and rebuilding it would be felt. Everything else is cached, newest
// first, up to RESIDENT_CACHE — so the app you were just on is still built when you turn
// back to it, and the one before that has been given back. See the note over evict().
typedef lv_obj_t *(*app_build_t)();

namespace app_shell {
    // Non-pinned screens kept built at once, the current one included.
    //
    // ONE, not two. Two was the first answer and it was sized for a roster that does not
    // exist: with APPS_LAUNCH_ONE the four apps are Clock and Flight (both pinned — the
    // Clock by choice, Flight because its screen is the one ui_create() builds at boot and
    // Weather shares) plus Headlines and Settings. That is two evictable screens against a
    // budget of two, so the count never exceeded the limit and nothing was ever actually
    // evicted: opening Settings cost its 24 KB back and kept it until reboot, which is the
    // thing this was built to stop.
    //
    // At one, the lazy screens take turns: whichever of Headlines or Settings you are on is
    // built and the other is not. Going home to the Clock still keeps the one you just
    // left, because a pinned screen is not counted here — so "the last other screen stays
    // warm" still holds, which was the point of the cache.
    constexpr int RESIDENT_CACHE = 1;
}

namespace app_shell {
    // The menu's running order, written down once.
    //
    // These used to be numbers typed at each call site, and the numbers then dictated the
    // menu: the Intel screen had to be registered AFTER Settings, leaving Settings stranded
    // mid-list, because moving it would have turned selectApp(4) into the wrong screen on a
    // factory-reset boot. The running order of a menu should be a design decision, not a
    // consequence of which integers somebody already wrote down.
    //
    // Registration in main.cpp and sim_main.cpp follows this order, and every jump names a
    // slot. To move an app in the menu, move it here and move its add() call to match.
    // Settings stays last: it is the drawer everything else is not.
    //
    // APP_TICKER was missing from here for six weeks, and this is the second time this list
    // has gone out of step with the registration order. The Stock Ticker was added as a
    // seventh app in 1.58.0 and registered between News and Settings, but nobody added it
    // to this enum — so APP_SETTINGS stayed 5, which by then was the Ticker. Every
    // selectApp(APP_SETTINGS) in the tree quietly went to the wrong screen, including the
    // one at main.cpp's boot that is supposed to open WiFi setup on a device with no
    // network. The result was an Orb that came up after a factory reset showing the
    // Ticker's background plate with no quotes on it, no WiFi prompt anywhere, and a knob
    // captured by a screen nobody could see. Its owner concluded his own product was
    // frozen. See CUT-03 / UX-022.
    //
    // The comment above already warned that moving an app "would have turned selectApp(4)
    // into the wrong screen on a factory-reset boot", and sim_main.cpp records the same
    // fault happening once before with APP_INTEL. Two warnings in prose, two occurrences.
    // So this is now checked at boot rather than trusted: see app_shell::verifySlots().
    // APPS_LAUNCH_ONE (config.h) takes Weather, Surveillance and the Ticker off the
    // roster for launch one. The slots go with them rather than being left as holes:
    // a slot naming an app nobody registers is the fault verifySlots() exists to catch,
    // and leaving three of them deliberately would make the check cry wolf for ever.
    enum Slot {
        APP_CLOCK = 0,
        APP_FLIGHT,
#if !APPS_LAUNCH_ONE
        APP_WEATHER,
        APP_SURVEILLANCE,
#endif
        APP_INTEL,
#if !APPS_LAUNCH_ONE
        APP_TICKER,
#endif
        APP_SETTINGS,
        APP_COUNT,
    };

    // `hidden` apps stay registered (so app indices and selectApp(n) never shift)
    // but are skipped when the knob cycles the menu — a Launch Kit theme flash uses
    // this to ship only the apps that theme includes, without renumbering the rest.
    void add(lv_obj_t *screen, const char *name,
             app_action_t onPress = nullptr, app_turn_t onTurn = nullptr, bool capture = false,
             app_action_t onEnter = nullptr, app_action_t onExit = nullptr, bool hidden = false);
    void add_active(const char *name,
                    app_action_t onPress = nullptr, app_turn_t onTurn = nullptr, bool capture = false,
                    app_action_t onEnter = nullptr, app_action_t onExit = nullptr, bool hidden = false);

    // Same app, built on demand. `pinned` opts out of eviction entirely.
    //
    // The screen pointer is NOT available until the app has been shown once, which is the
    // one way a lazy app differs from an eager one for its caller: anything that needs the
    // pointer up front (verifySlots' settingsScreen check) must use screenAt() and cope
    // with nullptr, and anything that reaches into a screen's widgets from outside has to
    // tolerate the screen not being there yet.
    void addLazy(const char *name, app_build_t build, app_action_t destroy,
                 app_action_t onPress = nullptr, app_turn_t onTurn = nullptr, bool capture = false,
                 app_action_t onEnter = nullptr, app_action_t onExit = nullptr, bool hidden = false,
                 bool pinned = false);

    // The screen of an app, or nullptr when a lazy app is not currently built. Never
    // builds anything: asking must not be the thing that allocates.
    lv_obj_t *screenAt(int idx);

    // Is this app's screen built right now? For diagnostics (`?orb apps`) and tests.
    bool residentAt(int idx);

    void begin();                                  // show the first app (no animation)

    // Check that the Slot enum above still describes the roster that actually registered,
    // and shout if it does not. Call once after the last add(), before anything jumps to a
    // slot. Returns true when the two agree.
    //
    // This exists because the enum has drifted from the registration order twice, both
    // times silently, and both times the prose comment that warned about it was already
    // there. A wrong slot number does not crash and does not log: it just quietly shows the
    // wrong screen, which is indistinguishable from a broken device to the person holding
    // it. `settingsScreen` is checked by POINTER rather than by name, because a theme may
    // relabel Settings to anything it likes and the label is therefore worthless as proof.
    // Pass the screen pointer for an eagerly-built Settings, or its build function when
    // Settings is registered with addLazy() and has no screen yet. Exactly one of the two.
    bool verifySlots(lv_obj_t *settingsScreen, app_build_t settingsBuild = nullptr);

    void next();          // advance to the next app (knob right), slides left
    void prev();          // go to the previous app (knob left), slides right
    // Knob pushed: run the current app's press handler. Returns whether there WAS one,
    // so the caller can tell a press that did something from a press that vanished. The
    // clock registers none, which is the dead end knob_help exists to answer.
    bool pressCurrent();
    void selectApp(int idx);  // jump straight to an app by index, no slide (e.g. forced setup at boot)

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
    // By index, for anything enumerating the shell from outside it (orb_link's `apps`).
    // Returns "" for an index that does not exist, so a caller cannot walk off the end.
    const char *nameAt(int idx);
    bool        hiddenAt(int idx);
}
