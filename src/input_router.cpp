#include "input_router.h"
#include "knob_help.h"
#include "wind_notice.h"
#include "update_ui.h"
#include <lvgl.h>       // lv_tick_get â€” a millisecond clock both targets have
#if defined(ESP_PLATFORM)
#include "display.h"   // markInput â€” input-to-glass timing
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

// A quick reversal opens the menu immediately, regardless of travel on either side.
// Keep the driver's timing window as a final check on the recorded reversal.
constexpr uint32_t ROCK_WINDOW_MS = 250;
uint32_t s_firedAt = 0;

// Absorb only the immediate gesture tail. The deadline is fixed when the menu
// opens: movement cannot extend it, and expiry needs no input-free dispatch.
// Once it expires every detent belongs to navigation, including slow turns.
constexpr uint32_t ROCK_TAIL_MS = 350;
bool s_drainingRock = false;
uint32_t s_rockOpenedMs = 0;

void open_from_rock() {
    s_drainingRock = true;
    s_rockOpenedMs = lv_tick_get();
    app_shell::openSwitcher();
}

bool drain_rock_tail() {
    if (!s_drainingRock) return false;
    if ((uint32_t)(lv_tick_get() - s_rockOpenedMs) >= ROCK_TAIL_MS) {
        s_drainingRock = false;
    }
    return s_drainingRock;
}

bool take_rock() {
    const uint32_t at = knob::lastRockMs();
    if (at == 0 || at == s_firedAt) return false;
    s_firedAt = at;
    return knob::lastRockGapMs() <= ROCK_WINDOW_MS;
}

}  // namespace

void input_router::tick() {
    if (knob::lastRockMs() != 0 && knob::lastRockMs() != s_firedAt) dispatch(0, false);
}

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

    // A wound-down clock asking to be wound. Turning winds it; a press does nothing,
    // because five turns is the price and a press would be a way to skip it.
    //
    // The rock is checked FIRST and deliberately still works, so this screen can always be
    // left. Winding counts detents in one direction only, which is what leaves a reversal
    // free to keep meaning "open the app menu" here as everywhere else. Without that, a
    // theme could strand somebody on a screen that will not take no for an answer, which is
    // the thing CUT-05 exists to forbid.
    bool rock = take_rock();
    if (drain_rock_tail()) {
        delta = 0;
        rock = false;
    }

    if (wind_notice::showing()) {
        if (rock) { open_from_rock(); return; }
        if (delta != 0) wind_notice::turn(delta);
        return;
    }

    // The switcher owns input while open. Reversals here only browse apps.
    if (app_shell::browsing()) {
        if (delta != 0) app_shell::browseTurn(delta);
        if (pressed)    app_shell::browsePress();
        return;
    }

    // Checked before the turn is delivered, so the detents that MADE the gesture are not
    // also handed to the app underneath. Without this, rocking out of the flight tracker
    // would select an aircraft on the way past.
    //
    // The rock works EVERYWHERE, captured screens included. Reversed 2026-09-11.
    //
    // Tick count is deliberately unrestricted: a quick turn back in either direction
    // means the same thing on every app, including Settings.
    //
    // Safe to allow: load() sets the captured flag from the app being entered and runs the
    // outgoing app's exit hook on every real switch, so a screen rocked out of leaves neither
    // its capture nor its state behind.
    if (rock) {
        open_from_rock();
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
