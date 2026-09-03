#include "input_router.h"
#include "knob_help.h"
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
// knob: a quick turn one way immediately followed by a quick turn back. Ordinary use never
// looks like that. Scrolling a list back and forth does, but slowly, and it is the speed that
// separates the gesture from someone changing their mind, which is why the window is tight.
//
// EITHER direction fires. Left-then-right only was the rule until 2026-08-29, on the reasoning
// that one fixed order halves the accidental reversals for no cost in performing it. There was
// a cost: a hand reaching for the menu does not decide which way to go first, so half the
// attempts did nothing. See knob.cpp for what carries the margin now, and UX-011.
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
//
// 800 was not enough either, and the reason is worth writing down because it will happen
// again to whoever measures this next. The 620-660 cluster was captured while the gesture
// was FAILING, and a person whose rock has just been ignored rocks the next one harder and
// faster. Measured again once it worked, on an unrelated errand, the same hand produced:
//
//     1096, 95, 1045, 5196 ms
//
// A natural rock is around a second; the fast ones are retries. Measuring a gesture at the
// moment it is broken measures the frustration, not the gesture. 1400 covers the natural
// motion and still rejects the 5196, which was a change of mind rather than a rock.
//
// This is also what "it works on Aviator but not on Steam Punk" turned out to be. Nothing
// in this path knows what theme is loaded: the detection is in the knob ISR and the
// dispatch is here. The rocks on that day were simply slower than the window.
// Back to 800 alongside the run-length rule in knob.cpp, which is the change that actually
// mattered. Widening this to 1400 without it made ordinary browsing open the menu, because
// any left-then-right counted however far the left half had run. The two together are the
// gesture: a SHORT turn back, then forward, reasonably promptly.
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
    // The "Ready" notice owns the knob until it is acknowledged, and ANY input clears it:
    // a turn either way or a press. It used to demand a press specifically, which made a
    // notice that exists to say "the knob is yours again" the one screen where most of the
    // knob did nothing. Reaching for a control and having it ignore you is the exact
    // feeling this notice is here to end.
    //
    // Whichever input clears it is SWALLOWED rather than passed on. Letting it through
    // would mean the gesture that means "yes, I see it" also does whatever the clock does
    // with it, which is the sort of thing that teaches people not to trust a confirmation.
    if (update_ui::awaitingAck()) {
        if (pressed || delta != 0) update_ui::ackReady();
        return;
    }
    // What the knob does, put up because a press had nowhere to go. Same contract as the
    // notice above and for the same reason: any input clears it, and that input is
    // SWALLOWED. Letting the dismissing press through would hand it straight back to the
    // screen that ignored it, which is the silence this panel exists to break.
    if (knob_help::showing()) {
        if (pressed || delta != 0) knob_help::dismiss();
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
    //
    // An app that has CAPTURED the knob owns it, rock included. Settings is the case: its
    // wheel scrolls with the knob, and scrolling a list back and forth is an ordinary thing
    // to do in it. Leaving the rock live there threw you out to the app menu, with nothing
    // chosen, in the middle of reading a list. Capturing already means "this screen is using
    // the knob for something else", and the rock was the one input ignoring that.
    //
    // Nothing gets trapped: a capturing screen carries its own way out. Settings has Back.
    if (!app_shell::captured() && rocked()) {
        app_shell::openSwitcher();
        return;
    }

    if (delta != 0) app_shell::turnCurrent(delta);
    // A press the current screen had no use for is not nothing happening, it is somebody
    // asking what this control does. The clock is the case that matters: it registers no
    // press handler, so on the first screen a new Orb ever shows, the most obvious thing to
    // try has always done nothing at all.
    //
    // Asked of the shared path rather than of the clock, so it cannot drift. Any screen that
    // ignores a press gets the same answer, and a screen that grows a handler stops giving
    // it without anybody having to remember this line exists.
    //
    // `count() > 0` is not belt and braces, it is the difference between the two ways a
    // press can go unanswered. Before the roster registers there is no screen that could
    // have ignored anything: the boot splash is still on the glass and the device has not
    // yet asked to be driven, so a press then is EARLY rather than lost, and covering the
    // splash with instructions would be answering a question nobody asked. Caught by the
    // --knobshot harness, which pressed at 2500 ms, got the panel, and passed while proving
    // nothing at all about the clock.
    if (pressed && !app_shell::pressCurrent() && app_shell::count() > 0) knob_help::show();
}
