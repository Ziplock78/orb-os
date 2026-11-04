// Run from the repository root (C++17 host compiler):
// g++ -std=c++17 -Isrc tests/complications_test.cpp -o /tmp/complications_test && /tmp/complications_test
//
// EVERY NUMBER BELOW CAME OUT OF ORB STUDIO, not out of a head. They were produced by
// calling complicationValue / complicationTurnOf / moonAgeDays in theme-forge.ts against
// the instant and the readings named here, and pasted in. That is the point of the file:
// the firmware and the editor have to agree on where a hand lands, and the only way to
// know they do is to make one side quote the other.
//
// If a case here starts failing after a change to theme-forge.ts, the firmware has NOT
// broken. The two halves have drifted, and the fix is to decide which answer is right and
// move both. Re-dump the table rather than nudging a constant until the test goes green.
#include <assert.h>
#include <stdio.h>
#include "complications.h"

using comp::Source;
using comp::Unit;

// 2026-11-03T15:30:00Z. Read as UTC on both sides, so the calendar readings do not depend
// on where the machine running the test happens to be.
static const time_t AT = 1793719800;
static const comp::Reading LIVE = { true, 23.5f, 41.f, 18.3f };

static int failures = 0;

static void near(const char* what, double got, double want, double tol) {
    const double d = got - want < 0 ? want - got : got - want;
    if (d > tol) {
        printf("  FAIL %-22s got %.9f  want %.9f  (off by %.3g)\n", what, got, want, d);
        ++failures;
    }
}

// One source at its default scale, the way Studio dumped it: min and max are the source's
// own range put through the same conversion as the reading, which is what makes a dial in
// Fahrenheit point at the same place as the one in Celsius beside it.
static void check(const char* what, Source s, Unit u, double wantValue, double wantTurn) {
    struct tm local;
    gmtime_r(&AT, &local);
    const comp::Spec sp = comp::spec_of(s);
    const float lo = comp::convert(s, u, sp.min);
    const float hi = comp::convert(s, u, sp.max);
    const float v  = comp::value(s, u, local, AT, LIVE);
    const float t  = comp::turn_of(s, u, lo, hi, v);
    near(what, v, wantValue, 1e-3);
    char tn[64];
    snprintf(tn, sizeof tn, "%s turn", what);
    near(tn, t, wantTurn, 1e-5);
}

int main() {
    printf("complications: mirroring Orb Studio\n");

    // The moon is checked on its own and in double, because it is the one reading the
    // device computes from a constant rather than reads off something, so a typo in the
    // epoch would not show up as anything except a phase quietly days out.
    near("moon age days", comp::moon_age_days(AT), 23.261200768, 1e-6);

    check("date",      Source::Date,     Unit::None, 3.0,       0.064516129);
    check("day",       Source::Day,      Unit::None, 3.0,       0.285714286);
    check("month",     Source::Month,    Unit::None, 11.0,      0.833333333);
    check("hour24",    Source::Hour24,   Unit::None, 15.5,      0.645833333);
    check("moon",      Source::Moon,     Unit::None, 23.261201, 0.787714215);
    check("temp C",    Source::Temp,     Unit::C,    23.5,      0.621428571);
    check("temp F",    Source::Temp,     Unit::F,    74.3,      0.621428571);
    check("humidity",  Source::Humidity, Unit::Pct,  41.0,      0.410000000);
    check("wind km/h", Source::Wind,     Unit::Kmh,  18.3,      0.305000000);
    check("wind mph",  Source::Wind,     Unit::Mph,  11.371093, 0.305000000);
    check("wind m/s",  Source::Wind,     Unit::Ms,   5.083333,  0.305000000);
    check("wind kn",   Source::Wind,     Unit::Kn,   9.881210,  0.305000000);

    // A MEASURING DIAL STOPS AT ITS END. A thermometer whose needle laps reads as a lower
    // temperature than it is, which is worse than pinning: the dial is confidently wrong.
    near("clamp high", comp::turn_of(Source::Temp, Unit::C, -20.f, 50.f,  80.f), 1.0, 1e-6);
    near("clamp low",  comp::turn_of(Source::Temp, Unit::C, -20.f, 50.f, -99.f), 0.0, 1e-6);

    // A WRAPPING DIAL CARRIES ON ROUND, in both directions. The negative case is the one
    // that catches a C programmer out, because fmodf keeps the sign and a single modulo
    // would put the hand off the back of the scale.
    near("wrap over",  comp::turn_of(Source::Date, Unit::None, 1.f, 32.f,  40.f), 0.258064516, 1e-6);
    near("wrap under", comp::turn_of(Source::Date, Unit::None, 1.f, 32.f,  -5.f), 0.806451613, 1e-6);

    // A dial given no span at all points at its start rather than dividing by zero.
    near("zero span",  comp::turn_of(Source::Temp, Unit::C, 10.f, 10.f, 10.f), 0.0, 1e-9);

    // The arc. endDeg below startDeg runs the hand the other way, which is a real gauge.
    near("deg forward", comp::deg(-135.f, 135.f, 0.5f),   0.0, 1e-4);
    near("deg reversed", comp::deg(135.f, -135.f, 0.25f), 67.5, 1e-4);
    near("deg full",    comp::deg(0.f, 360.f, 0.787714215), 283.577117, 1e-3);

    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("all checks passed\n");
    return 0;
}
