#include "input_router.h"
#include "app_shell.h"

// The three knob "modes", verbatim from the device loop() so behaviour can never
// drift between hardware and the simulator:
//   - switcher overlay up  -> turn cycles apps, push commits
//   - a menu owns the knob  -> turn scrolls it, push selects (Settings)
//   - inside an app         -> turn opens the switcher, push runs the app action
void input_router::dispatch(int delta, bool pressed) {
    if (app_shell::browsing()) {                 // switcher overlay is up
        if (delta != 0) app_shell::browseTurn(delta);
        if (pressed)    app_shell::browsePress();
    } else if (app_shell::captured()) {          // a menu owns the knob (Settings)
        if (delta != 0) app_shell::turnCurrent(delta);
        if (pressed)    app_shell::pressCurrent();
    } else {                                      // inside an app
        if (delta != 0) app_shell::browseTurn(delta);
        if (pressed)    app_shell::pressCurrent();
    }
}
