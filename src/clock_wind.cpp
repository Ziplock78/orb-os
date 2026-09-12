#include "clock_wind.h"

#include <time.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#endif

namespace {

// From the theme, replaced whole every time one is applied.
bool s_on     = false;
int  s_secs   = 86400;
int  s_turns  = 5;
bool s_sound  = true;
bool s_notice = true;

// The wind itself. Epoch seconds at which it runs out; 0 means "never wound", which reads
// as stopped so a design that switches this on starts where a real one would: needing a
// wind before it will run.
int64_t s_woundUntil = 0;

int  s_progress = 0;
bool s_justWound = false;

// Under a minute of drift either way is not a stopped clock, but an epoch that has not been
// set yet is a different thing entirely: the device boots at 1970 until the RTC or the
// network hands it a real one, and comparing a stored deadline against 1970 would report
// every mainspring as run down for the first few seconds of every boot. So nothing is
// judged stopped until the clock is plausibly real, which is the same threshold main.cpp
// uses before it will trust time for anything else.
constexpr int64_t EPOCH_IS_REAL = 1700000000LL;

int64_t now_epoch() { return (int64_t)time(nullptr); }
bool    time_known() { return now_epoch() > EPOCH_IS_REAL; }

#ifdef ARDUINO
// The old namespace, deliberately. See the note in main.cpp: the product was renamed and
// the NVS namespace was not, because renaming it would strand every setting on every device
// already in the field.
constexpr const char *NVS_NS  = "capsuleradar";
constexpr const char *NVS_KEY = "windUntil";
#endif

void save() {
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putLong64(NVS_KEY, (int64_t)s_woundUntil);
    p.end();
#endif
}

}  // namespace

void clock_wind::begin() {
#ifdef ARDUINO
    Preferences p;
    if (p.begin(NVS_NS, true)) {
        s_woundUntil = p.getLong64(NVS_KEY, 0);
        p.end();
    }
    Serial.printf("[wind] stored wind runs to %lld\n", (long long)s_woundUntil);
#endif
}

int clock_wind::turnsForFullWind()   { return s_turns; }
int clock_wind::detentsForFullWind() { return s_turns * DETENTS_PER_TURN; }

void clock_wind::applyTheme(bool on, int seconds, int turns, bool sound, bool notice) {
    s_on     = on;
    // One turn is the fewest that is still a winding gesture rather than a nudge; twenty is
    // past the point anybody would choose and exists only so a bad file cannot ask for a
    // thousand.
    s_turns  = turns < 1 ? 1 : (turns > 20 ? 20 : turns);
    // Clamped rather than trusted. The value arrives from a file on a removable card, and a
    // zero would make a clock that is wound down the instant it is wound. Ten seconds is the
    // floor because Studio offers it: it is the setting that makes this feature possible to
    // try at all, rather than a two day wait per attempt.
    s_secs   = seconds < 10 ? 10 : (seconds > 14 * 24 * 3600 ? 14 * 24 * 3600 : seconds);
    s_sound  = sound;
    s_notice = notice;
    // The winding gesture does not survive a theme change. Half a wind on the design you
    // just left is not half a wind on this one.
    s_progress  = 0;
    s_justWound = false;
}

bool clock_wind::enabled()  { return s_on; }
bool clock_wind::soundOn()  { return s_sound; }
bool clock_wind::noticeOn() { return s_notice; }

// A spring can hold no more than one full wind OF THIS DESIGN. The deadline in NVS was set
// by whatever design was wound last, and it survives a theme install and a reboot, which is
// right for the same design and wrong the moment the design changes its run time: a clock
// wound for a day and then reinstalled with "runs for 10 seconds" sat there running for the
// rest of the day, and the wind screen never came. Zion, 2026-09-12: "it should be on the
// wind screen by now." Clamped here, on every read, rather than once in applyTheme, because
// the time may not be known yet when the theme is applied at boot.
static void clamp_to_full_wind() {
    if (!s_on || !time_known()) return;
    const int64_t most = now_epoch() + (int64_t)s_secs;
    if (s_woundUntil > most) {
        s_woundUntil = most;
        save();
#ifdef ARDUINO
        Serial.printf("[wind] stored wind was longer than this design allows; now runs to %lld\n", (long long)s_woundUntil);
#endif
    }
}

bool clock_wind::stopped() {
    if (!s_on) return false;
    if (!time_known()) return false;   // see EPOCH_IS_REAL
    clamp_to_full_wind();
    return now_epoch() >= s_woundUntil;
}

float clock_wind::charge() {
    if (!s_on || !time_known()) return 1.0f;
    clamp_to_full_wind();
    const int64_t full = (int64_t)s_secs;
    const int64_t left = s_woundUntil - now_epoch();
    if (left <= 0)   return 0.0f;
    if (left >= full) return 1.0f;
    return (float)left / (float)full;
}

int64_t clock_wind::handsTime() {
    // Frozen where it ran down, which is what a real one does, and honest because the panel
    // over the top says in words that it has stopped. Never a silent wrong time.
    if (stopped()) return s_woundUntil;
    return now_epoch();
}

bool clock_wind::turn(int delta) {
    // One direction only. A crown that slips the other way is what a real one does, and it
    // leaves a left-then-right reversal free to still mean "open the app menu", which is the
    // gesture the knob hint teaches and the only way off this screen.
    if (delta <= 0) return false;
    if (!s_on) return false;
    s_progress += delta;
    if (s_progress < detentsForFullWind()) return true;

    s_progress  = 0;
    s_justWound = true;
    // Wound from NOW, not from whenever it ran out. Anything else would quietly punish
    // somebody for not noticing it had stopped.
    s_woundUntil = now_epoch() + (int64_t)s_secs;
    save();
#ifdef ARDUINO
    Serial.printf("[wind] wound: runs %d s, to %lld\n", s_secs, (long long)s_woundUntil);
#endif
    return true;
}

int clock_wind::progress() { return s_progress; }

bool clock_wind::justWound() {
    const bool v = s_justWound;
    s_justWound = false;
    return v;
}
