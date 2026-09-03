#pragma once

#include <stddef.h>
#include <stdint.h>

// Sounds a theme brings with it.
//
// Two of them, and they are not the same kind of thing. `wind.pcm` is a tick, fired once per
// detent while somebody is turning the knob to wind the clock, so it has to be short and it
// has to be cheap. `chime.pcm` is the hour, fired once every sixty minutes, so it can be a
// few seconds of something proper.
//
// WHY THE THEME AND NOT THE DEVICE. Zion's vintage radio is the whole argument: the detail
// people talked about was the AM static between stations, which was a SOUND, and it belonged
// to that object rather than to a settings menu. A Steam Punk clock and an Aviator chronometer
// should not click the same way any more than they should share a typeface.
//
// FORMAT. Raw PCM, 16 kHz, 16-bit signed, stereo interleaved, no container — exactly what
// audio.cpp already streams for the built-in chimes, so playback is the code that was already
// there. Studio does the decoding and resampling in the browser, where there is a real audio
// stack, rather than asking an ESP32 to parse an MP3.
//
// Both are OPTIONAL. Absent, the Orb uses its built-in tick and its built-in chime, which is
// what every theme written before this does.

namespace theme_audio {

// Load whatever the active theme ships, freeing whatever the last one did. Call when a theme
// becomes active, on the same path that applies the rest of its settings.
void load();

// Null when this theme ships no winding sound, which is the normal case.
//
// The chime is deliberately NOT here. It belongs to the device rather than to the worn theme
// (see chime_library) and it is streamed off the card as it rings rather than held, so a two
// minute chime costs this nothing.
const uint8_t *wind(size_t &bytes);

}  // namespace theme_audio
