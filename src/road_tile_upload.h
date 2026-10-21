#pragma once
#include <cstddef>
#include <cstdint>

// Browser uploads must contain the same bounded, clipped binary geometry as
// device-generated tiles. Portable so malformed uploads can be tested on a host.
namespace road_tile_upload {
constexpr size_t MAX_BYTES = 512 * 1024;
inline unsigned u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
template<class Input>
bool validate(Input &file, size_t size, int lat, int lon, int span) {
    if (size < 6 || size > MAX_BYTES) return false;
    uint8_t header[6];
    if (file.read(header, 6) != 6) return false;
    const unsigned polys = u16(header);
    const uint32_t points = u16(header+2) | ((uint32_t)u16(header+4) << 16);
    if (polys > 16000 || points < polys*2 || size != 6 + polys*2 + points*4ULL) return false;
    uint32_t sum = 0;
    for (unsigned i = 0; i < polys; ++i) {
        if (file.read(header, 2) != 2) return false;
        const unsigned count = u16(header);
        if (count < 2 || count > 256) return false;
        sum += count;
    }
    if (sum != points) return false;
    uint8_t buffer[512];
    for (uint32_t at = 0; at < points;) {
        const unsigned n = points-at > 128 ? 128 : points-at;
        if (file.read(buffer, n*4) != n*4) return false;
        for (unsigned i = 0; i < n; ++i) {
            const int y = (int16_t)u16(buffer+4*i), x = (int16_t)u16(buffer+4*i+2);
            if (y < lat*45 || y > (lat+span)*45 || x < lon*45 || x > (lon+span)*45) return false;
        }
        at += n;
    }
    return true;
}
}
