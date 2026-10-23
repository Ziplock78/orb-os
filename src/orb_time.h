#pragma once
// Reading the wall clock, once, without Arduino's retry loop.
//
// THE BUG THIS EXISTS FOR (Lerxtwood and CanadianAvenger, 2026-09-23). Arduino's
// getLocalTime(info, ms) is written as a wait:
//
//     uint32_t start = millis();
//     while ((millis() - start) <= ms) { time(&now); localtime_r(&now, info);
//                                        if (info->tm_year > (2016 - 1900)) return true;
//                                        delay(10); }
//     return false;
//
// With ms = 0, which is what every caller here wants (ask now, never block the UI task),
// the loop body runs only while `millis() - start` is still 0. If the millisecond counter
// happens to advance between those two adjacent calls, the body never runs at all and the
// function returns false WITHOUT EVER READING THE CLOCK. The clock is fine; the answer is
// a lie about it.
//
// What that looked like on the glass: hands snapping to 12 for a frame and back, or a
// digital face blanking for about a second, on any theme, at random. Worse on a sweeping
// clock only because it asks twenty-five times a second instead of once, so it loses that
// coin flip twenty-five times as often. Lerxtwood saw it about once a minute on the Swiss
// Railway dial, Drewzy on the ticking default and on Cold War, and it was read as the new
// railway stop leaking into every theme, which it was not.
//
// So: read the clock, once, and judge it by the same threshold Arduino uses. No loop, no
// timeout, nothing to lose a race with. The simulator keeps its override so the
// not-yet-set face can still be photographed.
#include <ctime>
#if !defined(ARDUINO)
#include <cstdlib>
#endif

// True when the clock has really been set (NTP or the RTC). `ti` is filled either way,
// so a caller that wants the seconds running on an unset clock still gets them.
inline bool orb_local_time(struct tm *ti) {
    *ti = {};  // keep the fallback deterministic if conversion fails
    const time_t now = time(nullptr);
    const bool read = localtime_r(&now, ti) != nullptr;
#if !defined(ARDUINO)
    if (getenv("SIM_NO_TIME")) return false;   // photograph the not-yet-set face
#endif
    // Arduino's own threshold, kept deliberately: anything at or before 2016 is the epoch
    // still sitting where it started rather than a time anybody set.
    return read && ti->tm_year > (2016 - 1900);
}
