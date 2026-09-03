#pragma once

#include <stdint.h>

// A mainspring, virtual.
//
// The Orb has a knob, which is the one thing on it that no other desk object has, and until
// now that knob has only ever been a menu selector. Winding is the one interaction that
// makes it feel like the mechanism it is drawn as: Steam Punk and Aviator are pictures of
// wound objects, and a wound object that never needs winding is a picture of one.
//
// The idea, and the reason it is worth the code, comes from Zion's vintage radio: the thing
// people talked about was not that it played MP3s, it was the AM static that faded in when
// you switched stations. A small sensory detail that asks something of you is what makes an
// object feel alive rather than displayed.
//
// WHAT IT DOES NOT DO. It never shows the wrong time. A real watch that runs down freezes
// its hands and quietly lies until somebody notices; this one stops its hands and says so
// in words, because a clock silently showing the wrong time is the single thing this
// firmware has always refused to do. And winding does not ask anyone to set the time: the
// PCF85063 keeps running and the network keeps agreeing with it, so the correct time is
// never actually lost. A completed wind resumes at the true time immediately.
//
// OFF BY DEFAULT, and per theme. A stopped clock reads as a broken clock to anybody who did
// not switch this on themselves, so it is never a surprise: a design asks for it.

namespace clock_wind {

// The encoder's own resolution. Fixed by the hardware, unlike the number of turns.
constexpr int DETENTS_PER_TURN = 20;

// How many turns of the crown a full wind takes. The theme's, because it is a FEEL and the
// only way to judge it is to wind one: too few and the gesture is a flick, too many and it is
// a chore, and the right answer probably differs between a pocket watch and a chronometer.
int turnsForFullWind();
int detentsForFullWind();

// Read the stored wind off NVS. Call once at boot, after Preferences is usable.
void begin();

// What the active theme asks for. Called whenever a theme is applied, so switching to a
// design with no mainspring stops the clock ever being stopped.
void applyTheme(bool on, int seconds, int turns, bool sound, bool notice);

bool enabled();          // this theme has a mainspring at all
bool soundOn();          // click while winding
bool noticeOn();         // put the full-screen "please wind" panel up when it stops

// Wound down: enabled, and the wind has run out. False whenever the theme has no
// mainspring, so every caller can ask this one question without also asking `enabled()`.
bool stopped();

// How much wind is left, 0..1. 1 immediately after a wind, 0 at the moment it stops. Used
// by a design that wants to draw a charge indicator on the face.
float charge();

// The time the hands should show. The true time while it is running; the instant it ran
// down once it has stopped, so the hands freeze where a real one would have.
int64_t handsTime();

// --- winding ---------------------------------------------------------------------------
// One detent of the knob. Only the winding direction counts: turning the other way is a
// crown that slips, which is what a real one does and which leaves the rock gesture free to
// mean what it means everywhere else. Returns true if this detent was accepted, so the
// caller knows whether to click.
bool turn(int delta);

int  progress();         // detents into the current wind, 0..detentsForFullWind()
bool justWound();        // and clears: true once, on the detent that completed a wind

}  // namespace clock_wind
