#include "theme_audio.h"

#include "theme_sd.h"
#include "theme_select.h"
#include "theme_style.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

#include <stdio.h>

namespace {

uint8_t *s_wind  = nullptr; size_t s_windLen  = 0;
uint8_t *s_chime = nullptr; size_t s_chimeLen = 0;

// A tick is milliseconds. Anything approaching a second is not a tick, it is a sound effect
// that will still be playing when the next detent arrives, and a hundred of those overlapping
// is a mess rather than a ratchet. 64 KB is two seconds at this format, which is far more
// headroom than a click needs and still refuses a file somebody uploaded by mistake.
constexpr size_t WIND_MAX_BYTES  = 64 * 1024;
// The hour, so it can be a real phrase. 640 KB is about ten seconds.
constexpr size_t CHIME_MAX_BYTES = 640 * 1024;

uint8_t *load_one(const char *name, size_t maxBytes, size_t &outLen) {
    outLen = 0;
    const char *slug = theme_select::activeSlug();
    if (!slug || !slug[0]) return nullptr;
    // Only what the theme SAYS it ships. A push never deletes from the card, so a sound from
    // an older push of the same theme would otherwise keep playing after it was removed —
    // the same trap custom_sprite documents for artwork.
    if (!theme_style::hasAsset(name)) return nullptr;

    char path[64];
    snprintf(path, sizeof(path), "/themes/%s/%s", slug, name);
    size_t len = 0;
    uint8_t *buf = theme_sd::read_whole(path, len, maxBytes);
#ifdef ARDUINO
    if (buf) Serial.printf("[theme_audio] %s: %u bytes\n", path, (unsigned)len);
    else     Serial.printf("[theme_audio] %s: not loaded\n", path);
#endif
    // An odd length would put the stream half a sample out and every frame after it would be
    // noise, so the tail is dropped rather than trusted.
    outLen = len & ~(size_t)1;
    return buf;
}

}  // namespace

void theme_audio::load() {
    if (s_wind)  { theme_sd::free(s_wind);  s_wind  = nullptr; s_windLen  = 0; }
    if (s_chime) { theme_sd::free(s_chime); s_chime = nullptr; s_chimeLen = 0; }
    s_wind  = load_one("wind.pcm",  WIND_MAX_BYTES,  s_windLen);
    s_chime = load_one("chime.pcm", CHIME_MAX_BYTES, s_chimeLen);
}

const uint8_t *theme_audio::wind(size_t &bytes)  { bytes = s_windLen;  return s_wind; }
const uint8_t *theme_audio::chime(size_t &bytes) { bytes = s_chimeLen; return s_chime; }
