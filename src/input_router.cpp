#include "input_router.h"
#include "app_shell.h"
#include "knob.h"

// What the knob does, in one place, shared by the device (main.cpp) and the simulator
// (sim_main.cpp) so "it behaved right in the sim" means "it behaves right on the Orb".
//
// THE ROCK
//
// Turning used to open the app switcher. That made the knob useless for anything else:
// every app that wanted a turn of its own had to first be given the knob by a button press,
// and this button takes real force to click. So the knob had one job and the button had all
// the others, which is backwards for a device whose only control is a knob.
//
// Now a turn belongs to whatever app is on screen, and the switcher is opened by ROCKING the
// knob: a quick turn LEFT immediately followed by a quick turn RIGHT. Ordinary use never
// looks like that. Scrolling a list back and forth does, but slowly — it is the speed that
// separates the gesture from someone changing their mind, which is why the window is tight.
//
// Direction order is deliberate. Left-then-right fires; right-then-left does not. Requiring
// one specific order halves the number of accidental reversals that can trigger it, for no
// cost in how hard the gesture is to perform.
//
// There is no fallback way in, by choice. If this proves unreliable on real hardware the
// window is the thing to tune, and if it cannot be made reliable then a fallback should come
// back rather than the tuning being fudged.
namespace {

// How long after a leftward detent a rightward one still counts as the same gesture. Short
// enough that a deliberate reversal has to be quick; long enough to be performable. Tuned
// on hardware, not derived — if this needs to move, this is the number.
constexpr uint32_t ROCK_WINDOW_MS = 320;

// The reversal this router has already acted on, so one gesture cannot fire twice. Stored
// as the timestamp rather than a flag: a second rock produces a new one, so it fires again
// with nothing to arm or reset.
uint32_t s_firedAt = 0;

bool rocked() {
    const uint32_t at = knob::lastRockMs();
    if (at == 0) return false;                       // no reversal has ever happened
    if (at == s_firedAt) return false;               // already acted on this one
    if (knob::lastRockGapMs() > ROCK_WINDOW_MS) {    // a reversal, but an unhurried one
        s_firedAt = at;                              // consumed, so it cannot fire later
        return false;
    }
    s_firedAt = at;
    return true;
}

}  // namespace

void input_router::dispatch(int delta, bool pressed) {
    // The switcher owns everything while it is up: turning cycles apps, pressing commits.
    // Leaving it is the 2 s settle or a press, never the gesture, so a rock performed while
    // browsing just cycles two apps and lands back where it started.
    if (app_shell::browsing()) {
        if (delta != 0) app_shell::browseTurn(delta);
        if (pressed)    app_shell::browsePress();
        return;
    }

    // Checked before the turn is delivered, so the detents that MADE the gesture are not
    // also handed to the app underneath. Without this, rocking out of the flight tracker
    // would select an aircraft on the way past.
    if (rocked()) {
        app_shell::openSwitcher();
        return;
    }

    if (delta != 0) app_shell::turnCurrent(delta);
    if (pressed)    app_shell::pressCurrent();
}
