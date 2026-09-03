#pragma once

// What the knob does, said once, at the moment somebody asks.
//
// The Orb has one control and three gestures, and exactly one of them is guessable. Turning
// is obvious. Pushing is obvious enough to try. The ROCK, a short turn back and then
// forward, is the only way to reach the app menu, and nothing about a knob suggests it.
//
// A person who has just built one lands on the clock and presses the knob, because that is
// what you do with a button. On the clock that press has always done nothing at all: the
// clock registers no press handler (see main.cpp), so `app_shell::pressCurrent()` finds
// nothing to run and the device sits there. That silence is the whole problem. It is not a
// missing feature, it is the one dead end on the first-run path, and it is the point at
// which somebody decides the thing is broken.
//
// So the dead press becomes the teaching moment. This panel is shown when, and only when, a
// press had nowhere to go: see input_router::dispatch. Anybody who already knows the rock
// never presses aimlessly and therefore never sees it, and it needs no "have they learned
// it yet" flag in NVS to stay out of the way, which is a state that could only ever be
// wrong. Any input dismisses it, the same contract the "Ready" notice uses.
//
// UNTHEMED, deliberately, and in the same family as the WiFi setup and no-SD-card screens:
// white on black in the built-in face on every Orb. A theme lives on the SD card and this
// is a recovery screen by another name, so it must not be able to depend on one. It is also
// the one screen whose job is to explain the device to somebody who does not yet trust it,
// which is not a place for a design to be able to make the words illegible.

namespace knob_help {
    // The panel is up and owns the knob.
    bool showing();
    // Put it up. Safe to call when it is already showing.
    void show();
    // Take it down and repaint what was underneath.
    void dismiss();
}
