// The menu (app-switcher) overlay's current/previous/next banners — a dedicated
// transparent canvas (not LVGL labels), the same reason radar_view.cpp's
// selection banners use one: LVGL labels have no glow. See menu_text.cpp.
#pragma once
#include <lvgl.h>

namespace menu_text {

// Turn the designer's explicit '|' break markers into '\n' lines, writing into `out`.
// Returns the line count (1 when the name has no marker).
//
// Explicit rather than measured: auto-wrapping required Studio's canvas metrics, LVGL's
// font metrics and this renderer to agree on where a line ends, and they did not, so the
// same name broke differently in the preview and on the dial. `font`/`wrapWidth` are kept
// in the signature only so older callers still compile; both are ignored.
int wrap_text(const lv_font_t *font, const char *in, int wrapWidth, char *out, size_t cap);


// Remember where the canvas should live. Call once, after the overlay object exists.
// This no longer allocates: see acquire()/release() below.
void init(lv_obj_t *parent);

// Allocate the drawing canvas (~868 KB of PSRAM) just before the overlay is shown, and
// give it back the moment it is hidden. Held permanently it competed with Settings'
// identical buffer and with decoded theme art, and lost. Safe to call repeatedly.
void acquire();
void release();

// Redraw all three banners for the given (already-resolved) app names — prev/next
// may be nullptr if there's only one app. Substitutes {name} into each banner's
// CUSTOM_MENU_*_FMT. No-op if the canvas hasn't been created (stock builds).
void refresh(const char *prevName, const char *curName, const char *nextName);

// False when the canvas could not be allocated (PSRAM pressure). Callers must fall back
// to a plain label rather than showing an overlay with no text on it at all.
bool available();

} // namespace menu_text
