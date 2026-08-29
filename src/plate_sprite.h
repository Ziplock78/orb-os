#pragma once
#include <lvgl.h>

// A background plate for a screen that has no compiled-in fallback.
//
// intel_sprite.cpp says the decode half of these files is "the same four functions
// everywhere", and that three attempts to share them foundered on each screen having its
// own compiled-in fallback symbols, its own log tag and its own asset names. That is true
// of the clock and the menu, which carry CUSTOM_* PNGs baked into the firmware.
//
// It is NOT true of the screens that arrived after themes did. Intel, the weather map and
// the Stock Ticker have no compiled fallback at all: for them the whole of the difference
// is an asset name and a word in a log line, and both of those are parameters. So this is
// the shared version for that set, and it is shorter than one copy of what it replaces.
//
// Same three rungs as everywhere else: pre-baked flash first (free, no PSRAM), then the SD
// PNG (~430 ms and ~424 KB), then nothing at all, which is the flat background colour the
// screen had before it could carry a picture.
namespace plate_sprite {

// A handle per screen, so two screens can hold their own plates without knowing about each
// other. Declare one at file scope in the view that owns it.
struct Plate {
    const char   *asset;      // e.g. "weather_plate.png"
    const char   *tag;        // what its log lines say
    uint8_t      *buf = nullptr;
    lv_img_dsc_t  dsc {};
    bool          tried = false;
};

// Decoded on the first ask and remembered until release(). Returns nullptr when the theme
// ships no such file, which is not an error: it is a design that chose a colour.
const lv_img_dsc_t *get(Plate &p);

// Give the pixels back. Called on the way out of a screen, so one app's artwork is not held
// while another is on the dial.
void release(Plate &p);

}  // namespace plate_sprite
