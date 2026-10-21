#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>

// One small home cell first, then larger background cells: avoid paying the
// public server's queue delay for dozens of tiny requests.
namespace road_tile_plan {
constexpr int SCALE = 4;
constexpr int MAX_CELLS = 10;
struct Cell { int lat, lon; double distance; int span = 1; };
inline int degree(int quarter) { return (int)std::floor(quarter / 4.0); }
inline void path(char *out, size_t size, int lat, int lon, unsigned roadClass, int span = 1) {
    if (span == 4) std::snprintf(out, size, "/roads/d%d_%d_%c.bin", degree(lat), degree(lon), roadClass ? 't' : 'm');
    else std::snprintf(out, size, "/roads/q%d_%d_%c.bin", lat, lon, roadClass ? 't' : 'm');
}
inline unsigned build(double lat, double lon, double rangeKm, Cell *out) {
    if (!std::isfinite(lat) || !std::isfinite(lon) || !std::isfinite(rangeKm) ||
        lat < -90 || lat >= 90 || lon < -180 || lon >= 180 || rangeKm <= 0) return 0;
    const int cy = (int)std::floor(lat), cx = (int)std::floor(lon);
    const double cosLat = std::max(0.15, std::cos(lat * 3.141592653589793 / 180.0));
    unsigned count = 1;
    out[0] = {(int)std::floor(lat * SCALE), (int)std::floor(lon * SCALE), 0, 1};
    for (int y = cy - 1; y <= cy + 1; ++y) {
        for (int x = cx - 1; x <= cx + 1; ++x) {
            if (y < -90 || y >= 90 || x < -180 || x >= 180) continue;
            const double south = y, west = x;
            const double dy = std::max({south - lat, 0.0, lat - south - 1}) * 111.0;
            const double dx = std::max({west - lon, 0.0, lon - west - 1}) * 111.0 * cosLat;
            if (dx * dx + dy * dy > rangeKm * rangeKm) continue;
            // Centre distance breaks ties for cells sharing a boundary with home.
            const double centerY = (south + 0.5 - lat) * 111.0;
            const double centerX = (west + 0.5 - lon) * 111.0 * cosLat;
            out[count++] = {y*SCALE, x*SCALE, centerX * centerX + centerY * centerY, 4};
        }
    }
    const int homeY = cy*SCALE, homeX = cx*SCALE;
    std::sort(out + 1, out + count, [=](const Cell &a, const Cell &b) {
        const bool ah = a.lat == homeY && a.lon == homeX, bh = b.lat == homeY && b.lon == homeX;
        return ah != bh ? ah : a.distance < b.distance;
    });
    return count;
}
}
