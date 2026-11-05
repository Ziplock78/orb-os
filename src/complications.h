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
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
// The one {token} parser on the device. A second copy here is how two screens quietly
// disagree about what an unclosed brace does, which text_tokens.h was written to stop.
#include "text_tokens.h"

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

// THE NAMES, because clock_style.json carries strings and not indices.
//
// A number would be smaller and would be the wrong choice. The sources are a list two
// repositories have to agree on, and the failure mode of an index is silent: insert one
// in the middle on the Studio side and every theme already on an SD card starts reading
// the wrong dial, with nothing anywhere saying so. A name that no longer exists falls
// through to `false` here and the subdial simply does not draw, which is a fault somebody
// can see. Spellings are exactly the keys of COMPLICATION_SOURCES and the strings in
// COMPLICATION_UNITS; they are the contract, so they are not tidied.
inline bool source_from_name(const char *n, Source &out) {
    if (!n) return false;
    if (!strcmp(n, "date"))     { out = Source::Date;     return true; }
    if (!strcmp(n, "day"))      { out = Source::Day;      return true; }
    if (!strcmp(n, "month"))    { out = Source::Month;    return true; }
    if (!strcmp(n, "hour24"))   { out = Source::Hour24;   return true; }
    if (!strcmp(n, "moon"))     { out = Source::Moon;     return true; }
    if (!strcmp(n, "temp"))     { out = Source::Temp;     return true; }
    if (!strcmp(n, "humidity")) { out = Source::Humidity; return true; }
    if (!strcmp(n, "wind"))     { out = Source::Wind;     return true; }
    return false;
}

// An unknown or absent unit is None, not a failure: every source except temperature and
// wind has no unit to name, and a dial whose unit went missing still points somewhere
// sensible because the base unit is what the Orb measures in anyway.
inline Unit unit_from_name(const char *n) {
    if (!n || !n[0])        return Unit::None;
    if (!strcmp(n, "C"))    return Unit::C;
    if (!strcmp(n, "F"))    return Unit::F;
    if (!strcmp(n, "%"))    return Unit::Pct;
    if (!strcmp(n, "km/h")) return Unit::Kmh;
    if (!strcmp(n, "mph"))  return Unit::Mph;
    if (!strcmp(n, "m/s"))  return Unit::Ms;
    if (!strcmp(n, "kn"))   return Unit::Kn;
    return Unit::None;
}

// ---- THE PRINTED LINE -------------------------------------------------------
// The other half of a subdial, and the half Studio draws in its preview while the device
// drew nothing until THEME_CAPS 59. Mirrored from theme-forge.ts: complicationReadingOf,
// complicationWord, moonPhaseWord, complicationText, COMPLICATION_SOURCES[].short and
// COMPLICATION_UNITS. Same contract as the arithmetic above: if the line under a dial
// disagrees with the preview a designer laid it out in, every theme is subtly wrong.

// JAVASCRIPT'S ROUNDING, NOT C'S, and the difference is not academic.
//
// Math.round breaks a tie toward +infinity; C's round() and lroundf() break it away from
// zero. They agree on every positive half and disagree on every negative one, so a
// thermometer reading exactly -3.5 C prints -3 in Studio's preview and would print -4 on
// the glass. January in anywhere cold reaches that number, and nothing on the screen would
// explain why the two differ.
inline long round_js(float v) { return (long)floor((double)v + 0.5); }

// The unit as Studio spells it, which is also what unit_from_name above reads back. One
// table turned round rather than a second list, so a unit cannot exist in one direction.
inline const char *unit_text(Unit u) {
    switch (u) {
        case Unit::C:   return "C";
        case Unit::F:   return "F";
        case Unit::Pct: return "%";
        case Unit::Kmh: return "km/h";
        case Unit::Mph: return "mph";
        case Unit::Ms:  return "m/s";
        case Unit::Kn:  return "kn";
        default:        return "";
    }
}

// COMPLICATION_SOURCES[].short: what {name} expands to. Short because it is printed on a
// dial a few dozen pixels across, not because it is an abbreviation of anything.
inline const char *source_short(Source s) {
    switch (s) {
        case Source::Date:     return "DATE";
        case Source::Day:      return "DAY";
        case Source::Month:    return "MONTH";
        case Source::Hour24:   return "24H";
        case Source::Moon:     return "MOON";
        case Source::Temp:     return "TEMP";
        case Source::Humidity: return "HUM";
        case Source::Wind:     return "WIND";
    }
    return "";
}

// The eight phases a person would name, from the moon's age in days. Boundaries at the
// eighths of a lunation with the four exact ones given a narrow window of their own: a
// full moon is a night, not a fortnight. Edges copied from MOON_WORDS in theme-forge.ts.
inline const char *moon_word(float ageDays) {
    const double t = fmod(fmod((double)ageDays / kSynodicDays, 1.0) + 1.0, 1.0);
    if (t < 0.02) return "New";
    if (t < 0.23) return "Waxing crescent";
    if (t < 0.27) return "First quarter";
    if (t < 0.48) return "Waxing gibbous";
    if (t < 0.52) return "Full";
    if (t < 0.73) return "Waning gibbous";
    if (t < 0.77) return "Last quarter";
    if (t < 0.98) return "Waning crescent";
    return "New";
}

// What this reading is called in words, where it has a name. Anything that is only ever a
// number comes back as that number, so {word} on a thermometer is the reading rather than
// a gap: a token that can only ever be empty is a legend promising what it cannot keep.
inline void word_of(Source s, float v, bool longForm, char *out, size_t outSz) {
    if (!out || outSz == 0) return;
    static const char *const DAY_SHORT[7]  = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const DAY_FULL[7]   = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                               "Thursday", "Friday", "Saturday" };
    static const char *const MON_SHORT[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const char *const MON_FULL[12]  = { "January", "February", "March", "April", "May",
                                               "June", "July", "August", "September",
                                               "October", "November", "December" };
    const long n = round_js(v);
    if (s == Source::Day) {
        const int i = (int)(((n - 1) % 7 + 7) % 7);
        snprintf(out, outSz, "%s", longForm ? DAY_FULL[i] : DAY_SHORT[i]);
        return;
    }
    if (s == Source::Month) {
        const int i = (int)(((n - 1) % 12 + 12) % 12);
        snprintf(out, outSz, "%s", longForm ? MON_FULL[i] : MON_SHORT[i]);
        return;
    }
    // The moon takes the UNROUNDED age: its phase is a position in a cycle, and rounding
    // to a whole day first would snap the four narrow windows onto day boundaries.
    if (s == Source::Moon) { snprintf(out, outSz, "%s", moon_word(v)); return; }
    snprintf(out, outSz, "%ld", n);
}

// THE PLAIN READING, which is what a dial prints under itself when the theme writes no
// format of its own. Whole numbers: this is a line a few pixels tall on a 466 px screen,
// and a decimal there is a smudge.
inline void reading_of(Unit u, bool withUnit, float v, char *out, size_t outSz) {
    if (!out || outSz == 0) return;
    const long n = round_js(v);
    const char *ut = unit_text(u);
    if (!withUnit || !ut[0]) { snprintf(out, outSz, "%ld", n); return; }
    // A percentage sits against its number; everything else takes a space.
    snprintf(out, outSz, (u == Unit::Pct) ? "%ld%s" : "%ld %s", n, ut);
}

// THE LINE A SUBDIAL SHOWS. An empty format falls back to the plain reading above, so
// switching a dial to a readout never hands anybody a blank.
//
// `label` is the word printed on the little dial. Studio bakes that word into the plate
// and does not need it at draw time, but {label} is a token it offers, so the device is
// sent it too: a token the editor expands and the glass cannot is the same lie as a unit
// that only exists in one direction.
inline void text_of(Source s, Unit u, bool withUnit, const char *label,
                    const char *fmt, float v, char *out, size_t outSz) {
    if (!out || outSz == 0) return;
    // Trimmed at both ends before the emptiness test, exactly as Studio's .trim() is: a
    // field holding one space is a field somebody cleared.
    char f[64];
    {
        const char *b = fmt ? fmt : "";
        while (*b == ' ' || *b == '\t') ++b;
        size_t len = strlen(b);
        while (len > 0 && (b[len - 1] == ' ' || b[len - 1] == '\t')) --len;
        if (len >= sizeof(f)) len = sizeof(f) - 1;
        memcpy(f, b, len);
        f[len] = 0;
    }
    if (!f[0]) { reading_of(u, withUnit, v, out, outSz); return; }

    char sv[24], se[24], sw[24], sf[24];
    snprintf(sv, sizeof(sv), "%ld", round_js(v));
    snprintf(se, sizeof(se), "%.1f", (double)v);
    word_of(s, v, false, sw, sizeof(sw));
    word_of(s, v, true,  sf, sizeof(sf));
    const text_tokens::Tok toks[] = {
        { "value", sv },
        { "exact", se },
        { "unit",  unit_text(u) },
        { "name",  source_short(s) },
        { "label", label ? label : "" },
        { "word",  sw },
        { "full",  sf },
    };
    text_tokens::expand(out, outSz, f, toks, sizeof(toks) / sizeof(toks[0]));
}

} // namespace comp
