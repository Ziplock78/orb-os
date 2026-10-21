#pragma once
#include <cmath>
#include <cstdint>

namespace road_tile_plan {
// Clip before caching so a way returned for two neighbouring queries is not
// painted twice in full. Coordinates and quarter-cell edges share the 180 scale.
template<class Output>
bool clip(const int16_t *p, unsigned n, int lat, int lon, Output &output, int span = 1) {
    int16_t run[512]; unsigned used = 0;
    auto flush = [&]() {
        const bool ok = used < 2 || output.line(run, used);
        used = 0; return ok;
    };
    const double south = lat * 45, west = lon * 45;
    const double width = span * 45;
    for (unsigned i = 1; i < n; ++i) {
        const double y = p[2*i-2], x = p[2*i-1];
        const double dy = p[2*i] - y, dx = p[2*i+1] - x;
        double lo = 0, hi = 1;
        auto edge = [&](double a, double b) {
            if (a == 0) return b >= 0;
            const double t = b / a;
            if (a < 0) { if (t > hi) return false; if (t > lo) lo = t; }
            else { if (t < lo) return false; if (t < hi) hi = t; }
            return true;
        };
        if (!edge(-dx, x-west) || !edge(dx, west+width-x) ||
            !edge(-dy, y-south) || !edge(dy, south+width-y)) {
            if (!flush()) return false;
            continue;
        }
        const int16_t ay = (int16_t)std::lround(y + lo*dy), ax = (int16_t)std::lround(x + lo*dx);
        const int16_t by = (int16_t)std::lround(y + hi*dy), bx = (int16_t)std::lround(x + hi*dx);
        if (ay == by && ax == bx) continue;
        if (used && (run[2*used-2] != ay || run[2*used-1] != ax || used == 256)) {
            if (!flush()) return false;
        }
        if (!used) { run[0] = ay; run[1] = ax; used = 1; }
        run[2*used] = by; run[2*used+1] = bx; ++used;
        if (hi < 1 && !flush()) return false;
    }
    return flush();
}
}
