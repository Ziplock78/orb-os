// Settings' knob-driven wheel lists (main menu, Display, Sound, Location,
// Units, and the chime/theme pickers) all share one text renderer, so a
// Launch Kit push's font/size/glow applies identically everywhere, not just
// the top-level list. LVGL labels have no glow, so — same reason
// menu_text.cpp exists — this is a dedicated transparent canvas, not labels.
// See settings_text.cpp.
#pragma once
#include <lvgl.h>

namespace settings_text {

// Create the canvas as the topmost child of `parent` (s_screen) — sized to
// fill it, so it draws over whichever wheel page is currently visible. Call
// once, after every other child of s_screen exists.
void init(lv_obj_t *parent);

// Allocate the drawing canvas (~868 KB of PSRAM) on entering Settings, release it on
// leaving. Held permanently it competed with the menu's identical buffer and with
// decoded theme art. Safe to call repeatedly.
void acquire();
void release();

// Call once at the top of wheel_layout(), before drawing any items — clears
// last frame's text so a previous page's items don't linger under this one.
void begin_frame();

// Draw one item's text centered at (x,y) in screen coordinates. No-op if the
// canvas hasn't been created (stock builds keep using plain lv_label text).
// `glow` and `glowCol` are the caller's now rather than read from the theme in here: the
// selected row and the rest carry their own, and this renderer draws both. Passing them in
// also means the one place that knows which row is which is the one place that decides.
void draw_item(const char *str, float x, float y, lv_color_t color, lv_opa_t opa,
               int glow, lv_color_t glowCol);

// False when the canvas could not be allocated (PSRAM pressure). Callers must then keep
// the plain labels visible, or Settings becomes unreadable and unnavigable.
bool available();

} // namespace settings_text
