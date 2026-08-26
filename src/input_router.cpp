#include "input_router.h"
#include "update_ui.h"
#include <lvgl.h>       // lv_tick_get — a millisecond clock both targets have
#if defined(ESP_PLATFORM)
#include "display.h"   // markInput — input-to-glass timing
#endif
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
// Measured, not guessed. 320 was a guess and it was wrong by half.
//
// Nine rocks captured off the real knob on 2026-08-26, gap between the last left detent and
// the first right one:
//
//     96, 150, 624, 624, 625, 654, 655, 644, 625 ms
//
// The two fast ones are what a deliberate, already-failed-once attempt looks like. The
// natural motion is the cluster at 620-660, every one of which the old 320 window threw
// away, which is exactly the reported "works sometimes". 800 clears the cluster with about
// 20% of headroom and is still far short of anything a person would call a pause.
//
// The cost of being generous is small and bounded: while the switcher is up this check is
// skipped entirely, so a false positive can only happen INSIDE an app, and of the apps that
// read a turn at all the worst outcome is that scrolling Intel back and forth opens the
// menu. If that ever becomes the complaint, this number is the dial, and the measurement is
// still in the firmware to re-run it.
constexpr uint32_t ROCK_WINDOW_MS = 800;

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
    // The "Ready" notice owns the knob until it is acknowledged, and the press that clears
    // it is SWALLOWED rather than passed on. Letting it through would mean the press that
    // means "yes, I see it" also opens whatever the clock does with a press, which is the
    // sort of thing that teaches people not to trust a confirmation.
    if (update_ui::awaitingAck()) {
        if (pressed) update_ui::ackReady();
        return;
    }
    // Stamped here rather than in the menu, because "how long until I see it" is a question
    // worth being able to ask of any screen. The first attempt timed only the switcher, on
    // the assumption that the switcher was the problem, which is the assumption being
    // tested. Cleared by whichever frame lands next; see display::markInput.
    // Device only. The simulator draws through its own SDL path and has no display.cpp, and
    // "how long until the panel shows it" is not a question a desktop window can answer
    // anyway. lv_tick_get rather than millis() because this file has no Arduino header;
    // lv_conf.h maps LVGL's tick straight onto millis() on the device, so it is the same
    // counter the flush reads.
#if defined(ESP_PLATFORM)
    if (delta != 0 || pressed) display::markInput(lv_tick_get());
#endif

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
