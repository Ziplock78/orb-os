#pragma once
#include <lvgl.h>

// Text laid along an arc, with each glyph tilted tangent to it, blitted into an
// RGB565+alpha raster.
//
// This is the THIRD screen to want it and the first to not copy it. clock_view.cpp grew the
// original (draw_baked_arc_text/blit_glyph_rot), radar_view.cpp copied it wholesale for the
// selection banners — its own comment says so — and the Headlines screen asking for the
// same thing is what finally made the duplication worth paying off rather than repeating.
// LVGL has no curved-text primitive and no glyph rotation, so somebody has to do this by
// hand; it should be somebody once.
//
// The only thing that had to change to share it is that the destination is now a parameter
// rather than a file-scope canvas: both original copies read a static buffer and the screen
// constants directly, which is exactly what made them uncopyable without editing.
namespace curved_text {

// A raster to draw into: RGB565 + 8-bit alpha, 3 bytes per pixel, the same layout
// custom_sprite/office_sprite emit and lv_canvas wants for LV_IMG_CF_TRUE_COLOR_ALPHA.
struct Target {
    uint8_t *buf;
    int      w;
    int      h;
};

// Lay `str` along an arc of radius R about (cx, cy) in TARGET coordinates, centred on
// arcDeg — a clock angle, 0 = twelve o'clock. Text below the horizontal is automatically
// flipped so it reads the right way up rather than upside down along the bottom of a dial.
//
// glow > 0 draws the same glyphs first at a ring of offsets in glowCol at falling opacity,
// which is how these screens fake a canvas shadowBlur the firmware has no equivalent for.
void draw_arc(const Target &dst, const lv_font_t *font, const char *str,
              float cx, float cy, float R, float arcDeg,
              lv_color_t col, int glow, lv_color_t glowCol, lv_opa_t opa = 255);

// The plate behind a straight line of text: a rounded rectangle sized to the words, painted
// before them. THEME_CAPS 33.
//
// It lives here rather than at each call site because the caller does not know how wide the
// text is going to be. draw_straight already measures the run to place it, so this is the
// only place that can size a plate to fit without measuring the same string twice and
// getting a different answer the second time.
//
// There is no curved equivalent, and there will not be one: a rounded rectangle bent around
// a dial is not a rounded rectangle, and every honest version of it is a different shape
// from the one the design was drawn against.
struct Pill {
    lv_color_t col    = {};
    lv_opa_t   opa    = 0;   // 0 = draw nothing at all, which is the default everywhere
    int        radius = 0;   // corner rounding, px, clamped to half the short side
};

// Build one from any theme struct carrying bg/bgOpa/radius, which since THEME_CAPS 33 is
// every text struct there is. Templated rather than overloaded so this header stays free of
// theme_style.h, which includes it.
template <typename T>
inline Pill pill_of(const T &t) {
    return { lv_color_hex(t.bg), (lv_opa_t)t.bgOpa, t.radius };
}

// The same glyph machinery without the arc: one straight line, baseline vertically centred
// on `by`, laid out by each glyph's own advance width so a digit changing width pushes only
// the tail of the string and a live value never wobbles.
// align: 0 = bx is the start, 1 = bx is the middle, 2 = bx is the end.
// pill: optional plate behind the words, drawn first. Opa 0, the default, draws none.
void draw_straight(const Target &dst, const lv_font_t *font, const char *str,
                   float bx, float by, lv_color_t col, int glow, lv_color_t glowCol, int align,
                   lv_opa_t opa = 255, const Pill &pill = Pill());

}  // namespace curved_text
