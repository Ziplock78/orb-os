#pragma once
// THE SUBDIAL ARITHMETIC, and nothing else. No LVGL, no Arduino, no SD: this header is
// plain C++ so the host compiler can run it against the same cases Orb Studio runs, which
// is the whole reason it is split out from the drawing.
//
// Orb Studio owns the editor's answer to two questions, in theme-forge.ts: what a dial
// READS right now, and where on its own arc that reading puts the hand. The note above
// complicationTurn() there says the firmware mirrors it "so a subdial cannot mean one
// thing on the screen and another on the glass". That sentence is the contract this file
// exists to keep. A designer places a hand against the preview; if the device computes a
// different angle from the same numbers, every theme is subtly wrong and nothing on the
// screen says so.
//
// Mirrored from theme-forge.ts: COMPLICATION_SOURCES, complicationConvert,
// moonAgeDays, complicationValue, complicationTurnOf, complicationDeg. Changing either
// side without the other is the bug this is shaped to make loud, so tests/complications_test.cpp
// pins the values both halves have to agree on.
#include <stdint.h>
#include <math.h>
#include <time.h>

namespace comp {

// The eight readings a subdial can take. Order matches COMPLICATION_SOURCES so a theme
// written by a newer Studio against an older Orb lands on the right one by index as well
// as by name.
enum class Source : uint8_t { Date = 0, Day, Month, Hour24, Moon, Temp, Humidity, Wind };

// Units a reading can be SHOWN in. Studio writes a string into clock_style.json; the
// parser turns it into one of these once, so the per-frame path compares an enum rather
// than strcmp-ing "km/h" six times a second.
enum class Unit : uint8_t { None = 0, C, F, Pct, Kmh, Mph, Ms, Kn };

// What a source can read AT ALL, and whether its needle laps or stops. Not what any one
// dial is set to: that is the designer's min/max, which arrives per complication.
struct Spec { float min; float max; bool wrap; };

inline Spec spec_of(Source s) {
    switch (s) {
        case Source::Date:     return { 1.f,   32.f,     true  };
        case Source::Day:      return { 1.f,   8.f,      true  };
        case Source::Month:    return { 1.f,   13.f,     true  };
        case Source::Hour24:   return { 0.f,   24.f,     true  };
        case Source::Moon:     return { 0.f,   29.53f,   true  };
        case Source::Temp:     return { -20.f, 50.f,     false };
        case Source::Humidity: return { 0.f,   100.f,    false };
        case Source::Wind:     return { 0.f,   60.f,     false };
    }
    return { 0.f, 1.f, false };
}

// The Orb reports in one unit; the dial shows whichever the designer picked. Only
// temperature and wind have anywhere to go, which is why the other six fall straight
// through rather than carrying a conversion table that would only ever be the identity.
inline float convert(Source s, Unit u, float v) {
    if (s == Source::Temp) return u == Unit::F ? v * 9.f / 5.f + 32.f : v;
    if (s == Source::Wind) {
        if (u == Unit::Mph) return v / 1.609344f;
        if (u == Unit::Ms)  return v / 3.6f;
        if (u == Unit::Kn)  return v / 1.852f;
    }
    return v;
}

// THE MOON, FROM ARITHMETIC ALONE. Days since a known new moon, wrapped by the synodic
// month. No network, no API and no stored table, which is why the moon costs the Orb
// nothing despite sounding like the most exotic reading on the list.
//
// Takes UTC epoch seconds, NOT the local tm the calendar readings use. Studio computes
// this from Date.getTime(), which is absolute, so feeding it local time here would walk
// the phase by the timezone offset and put a Phoenix Orb seven hours off a London one.
constexpr double kNewMoonEpoch = 947182440.0;   // 2000-01-06 18:14:00 UTC
constexpr double kSynodicDays  = 29.530588853;

inline double moon_age_days(time_t utc) {
    const double days = ((double)utc - kNewMoonEpoch) / 86400.0;
    return fmod(fmod(days, kSynodicDays) + kSynodicDays, kSynodicDays);
}

// What the live weather is worth, for the three readings the clock cannot work out on its
// own. Studio previews these against a sample so a designer is never styling a hand
// pinned to its stop; the device has the real thing, and `has` says whether it does yet.
struct Reading {
    bool  has   = false;
    float temp  = 21.f;    // Celsius, as the Orb's own weather arrives
    float humidity = 55.f; // percent
    float wind  = 12.f;    // km/h
};

// WHAT THE DIAL READS, in the unit it is set to show. Split from the turn for the same
// reason Studio splits it: the line printed under a subdial and the angle of its hand have
// to be one number read two ways, or the dial contradicts its own caption.
inline float value(Source s, Unit u, const struct tm& local, time_t utc, const Reading& r) {
    float v = 0.f;
    switch (s) {
        case Source::Date:     v = (float)local.tm_mday; break;
        case Source::Day:      v = (float)local.tm_wday + 1.f; break;
        case Source::Month:    v = (float)local.tm_mon + 1.f; break;
        case Source::Hour24:   v = (float)local.tm_hour + (float)local.tm_min / 60.f; break;
        case Source::Moon:     v = (float)moon_age_days(utc); break;
        case Source::Temp:     v = r.temp; break;
        case Source::Humidity: v = r.humidity; break;
        case Source::Wind:     v = r.wind; break;
    }
    return convert(s, u, v);
}

// WHERE A GIVEN READING PUTS THE HAND, as a fraction of this dial's own arc. lo and hi are
// the designer's, already in the shown unit. Pass NAN for either to fall back to what the
// source can read, converted, which is what Studio does when a scale was never set.
inline float turn_of(Source s, Unit u, float lo, float hi, float v) {
    const Spec sp = spec_of(s);
    if (!isfinite(lo)) lo = convert(s, u, sp.min);
    if (!isfinite(hi)) hi = convert(s, u, sp.max);
    const float span = hi - lo;
    if (span == 0.f) return 0.f;
    const float t = (v - lo) / span;
    // A wrapping scale carries on round; a measuring one stops at its end, because a
    // needle that laps its own stop reads as a lower number than it is.
    if (sp.wrap) return fmodf(fmodf(t, 1.f) + 1.f, 1.f);
    return t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
}

// A turn placed on THIS subdial's arc. endDeg below startDeg simply runs the hand the
// other way, which is a real gauge and not a mistake, so there is no ordering to enforce.
inline float deg(float startDeg, float endDeg, float turn) {
    return startDeg + turn * (endDeg - startDeg);
}

} // namespace comp
