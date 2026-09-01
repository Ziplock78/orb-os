#include "curved_text.h"
#include "diag_log.h"   // the arc guard below says the fault out loud
#include <math.h>
#include <string.h>

// Lifted verbatim from radar_view.cpp's rtext_* family, which was itself lifted from
// clock_view.cpp. The arithmetic is unchanged on purpose: both callers are tuned against
// designs that already exist, and a "tidy-up" here would move type on somebody's dial.
namespace {

// A 4-bpp (16-level) glyph alpha bitmap, as lv_font_conv --bpp 4 --no-compress emits it and
// as the built-in fonts store it: continuous bitstream, MSB first, box_w px per row, no
// row padding.
inline float glyph_alpha4(const uint8_t *bmp, int bw, int x, int y) {
    const int bit = (y * bw + x) * 4;
    const uint8_t byte = bmp[bit >> 3];
    const uint8_t nib = (bit & 4) ? (byte & 0x0F) : (byte >> 4);
    return nib * 17.0f;
}

// Fill a rounded rectangle into the same RGB565+alpha raster the glyphs land in.
//
// Blends rather than overwrites, so a plate at half strength shows what is behind it, and it
// runs BEFORE the glyphs so the words are never dimmed by their own background. Corners are
// a plain circle test rather than anything anti-aliased: at the radii these plates use, 0 to
// 20 px, the stair-stepping is a pixel deep and invisible under a glyph, and the alternative
// is a second coverage pass on every pixel of a shape whose whole job is to be ignored.
void fill_round_rect(const curved_text::Target &dst, int x0, int y0, int x1, int y1,
                     int radius, lv_color_t col, lv_opa_t opa) {
    if (!dst.buf || opa == 0 || x1 <= x0 || y1 <= y0) return;
    const int w = x1 - x0, h = y1 - y0;
    int r = radius;
    const int rmax = (w < h ? w : h) / 2;
    if (r > rmax) r = rmax;
    if (r < 0) r = 0;
    const uint8_t lo = (uint8_t)(col.full & 0xFF), hi = (uint8_t)(col.full >> 8);
    const int cx0 = x0 + r, cx1 = x1 - 1 - r, cy0 = y0 + r, cy1 = y1 - 1 - r;
    const int rr = r * r;
    for (int y = y0; y < y1; ++y) {
        if (y < 0 || y >= dst.h) continue;
        for (int x = x0; x < x1; ++x) {
            if (x < 0 || x >= dst.w) continue;
            if (r > 0) {
                // Only the four corner boxes need testing; everything else is inside by
                // construction, which is what keeps this cheap on a 466 px line.
                const int qx = (x < cx0) ? cx0 - x : (x > cx1) ? x - cx1 : 0;
                const int qy = (y < cy0) ? cy0 - y : (y > cy1) ? y - cy1 : 0;
                if (qx && qy && qx * qx + qy * qy > rr) continue;
            }
            const int px = (y * dst.w + x) * 3;
            uint8_t *b = dst.buf;
            if (opa >= 255 || b[px + 2] == 0) {
                b[px] = lo; b[px + 1] = hi;
                if (b[px + 2] < opa) b[px + 2] = opa;
            } else {
                // Something is already here. Mix toward the plate by its own opacity and
                // keep the stronger alpha, which is what an overlap of two plates should do.
                const uint16_t have = (uint16_t)(b[px] | (b[px + 1] << 8));
                const int hr = (have >> 11) & 0x1F, hg = (have >> 5) & 0x3F, hb = have & 0x1F;
                const uint16_t want = col.full;
                const int wr = (want >> 11) & 0x1F, wg = (want >> 5) & 0x3F, wb = want & 0x1F;
                const int a = opa;
                const int nr = (wr * a + hr * (255 - a)) / 255;
                const int ng = (wg * a + hg * (255 - a)) / 255;
                const int nb = (wb * a + hb * (255 - a)) / 255;
                const uint16_t out = (uint16_t)((nr << 11) | (ng << 5) | nb);
                b[px] = (uint8_t)(out & 0xFF); b[px + 1] = (uint8_t)(out >> 8);
                if (b[px + 2] < opa) b[px + 2] = opa;
            }
        }
    }
}

// Rotate one glyph about its own centre and blend it in, box centred at (destCx, destCy).
//
// Composites by "higher opacity wins" per pixel rather than true alpha-over. That is cheap,
// and correct for the back-to-front order these callers use: glow rings go down first at
// low opacity and never dim the sharp pass that follows, and the full-opacity fill always
// dominates its own footprint.
void blit_glyph(const curved_text::Target &dst, const uint8_t *bmp, int bw, int bh,
                float destCx, float destCy, float angleDeg, lv_color_t col, lv_opa_t maxOpa) {
    if (!bmp || bw <= 0 || bh <= 0 || !dst.buf) return;
    uint8_t *buf = dst.buf;
    const float th = angleDeg * (float)M_PI / 180.0f, ct = cosf(th), st = sinf(th);
    const float pivotX = bw * 0.5f, pivotY = bh * 0.5f;
    const float reach = sqrtf(pivotX * pivotX + pivotY * pivotY) + 1.0f;
    const int x0 = (int)fmaxf(0.0f, destCx - reach), x1 = (int)fminf((float)dst.w - 1, destCx + reach);
    const int y0 = (int)fmaxf(0.0f, destCy - reach), y1 = (int)fminf((float)dst.h - 1, destCy + reach);
    for (int dy = y0; dy <= y1; ++dy) {
        const float oy = dy - destCy;
        for (int dx = x0; dx <= x1; ++dx) {
            const float ox = dx - destCx;
            const float sxf = ox * ct + oy * st + pivotX;
            const float syf = -ox * st + oy * ct + pivotY;
            const int ix = (int)floorf(sxf), iy = (int)floorf(syf);
            if (ix < -1 || iy < -1 || ix >= bw || iy >= bh) continue;
            const float fx = sxf - ix, fy = syf - iy;
            const float a00 = (ix >= 0 && iy >= 0 && ix < bw && iy < bh) ? glyph_alpha4(bmp, bw, ix, iy) : 0.0f;
            const float a10 = (ix + 1 >= 0 && iy >= 0 && ix + 1 < bw && iy < bh) ? glyph_alpha4(bmp, bw, ix + 1, iy) : 0.0f;
            const float a01 = (ix >= 0 && iy + 1 >= 0 && ix < bw && iy + 1 < bh) ? glyph_alpha4(bmp, bw, ix, iy + 1) : 0.0f;
            const float a11 = (ix + 1 >= 0 && iy + 1 >= 0 && ix + 1 < bw && iy + 1 < bh) ? glyph_alpha4(bmp, bw, ix + 1, iy + 1) : 0.0f;
            float a = a00 * (1 - fx) * (1 - fy) + a10 * fx * (1 - fy) + a01 * (1 - fx) * fy + a11 * fx * fy;
            a = a * (float)maxOpa / 255.0f;
            if (a < 8.0f) continue;
            const int px = (dy * dst.w + dx) * 3;
            if ((uint8_t)a <= buf[px + 2]) continue;
            buf[px]     = (uint8_t)(col.full & 0xFF);
            buf[px + 1] = (uint8_t)(col.full >> 8);
            buf[px + 2] = (uint8_t)a;
        }
    }
}

// The same glyph at a ring of offsets around where it already sits, at falling opacity.
void blit_glyph_glow(const curved_text::Target &dst, const uint8_t *bmp, int bw, int bh,
                     float destCx, float destCy, float angleDeg, lv_color_t glowCol, int glow) {
    if (glow <= 0) return;
    static const float dirs[8][2] = {
        {1,0},{-1,0},{0,1},{0,-1},{0.707f,0.707f},{-0.707f,0.707f},{0.707f,-0.707f},{-0.707f,-0.707f}
    };
    const int rings = 3;
    for (int ri = 1; ri <= rings; ++ri) {
        const int r = (int)lroundf((float)glow * ri / rings);
        if (r <= 0) continue;
        const lv_opa_t opa = (lv_opa_t)(90 / ri);
        for (int di = 0; di < 8; ++di)
            blit_glyph(dst, bmp, bw, bh, destCx + dirs[di][0] * r, destCy + dirs[di][1] * r,
                       angleDeg, glowCol, opa);
    }
}

}  // namespace

void curved_text::draw_arc(const Target &dst, const lv_font_t *font, const char *str,
                           float cx, float cy, float R, float arcDeg,
                           lv_color_t col, int glow, lv_color_t glowCol, lv_opa_t opa) {
    if (!dst.buf || !font || !str || !str[0] || R < 1.0f) return;
    // An arc has one baseline, so a paragraph cannot be laid along it the way draw_straight
    // lays one. Said out loud rather than silently welded into a run-on, because that exact
    // silence is what put "Configure attheorb.local192.168.1.42" on the About screen. Nothing
    // on a card can reach this today - no theme ships splash_style.json - so it is a tripwire
    // for whoever curves a multi-line slot first, not a layout decided here in advance.
    if (strchr(str, '\n'))
        diag::log("curved_text: draw_arc cannot lay a paragraph on one arc: \"%s\"", str);
    const int n = (int)strlen(str), cap = n < 80 ? n : 80;
    float w[80], total = 0.0f;
    for (int i = 0; i < cap; ++i) {
        char c[2] = { str[i], 0 };
        lv_point_t s;
        lv_txt_get_size(&s, c, font, 0, 0, LV_COORD_MAX, 0);
        w[i] = (float)s.x;
        total += s.x;
    }
    // Along the bottom of a dial the glyphs have to be flipped, or the words run upside
    // down and backwards. This is the test for "am I below the middle".
    const float norm = fmodf(fmodf(arcDeg, 360.0f) + 360.0f, 360.0f);
    const bool  bottom = (norm > 90.0f && norm < 270.0f);
    const float dir = bottom ? -1.0f : 1.0f;
    const float base = arcDeg * (float)M_PI / 180.0f;
    const float lineH = (float)lv_font_get_line_height(font), desc = (float)font->base_line;
    const float halfMid = (lineH - 2.0f * desc) * 0.5f;
    float cursor = -total / 2.0f;
    for (int i = 0; i < cap; ++i) {
        const float mid = cursor + w[i] / 2.0f, ang = base + dir * mid / R;
        const float ax = cx + sinf(ang) * R, ay = cy - cosf(ang) * R;
        const float rot = ang + (bottom ? (float)M_PI : 0.0f);
        cursor += w[i];
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)str[i], 0)) continue;
        const uint8_t *bmp = lv_font_get_glyph_bitmap(font, (uint32_t)(uint8_t)str[i]);
        if (!bmp || g.box_w == 0 || g.box_h == 0) continue;
        const float offY = halfMid - (float)g.ofs_y - (float)g.box_h * 0.5f;
        const float cr = cosf(rot), sr = sinf(rot);
        const float destCx = ax - offY * sr, destCy = ay + offY * cr;
        const float rotDeg = rot * 180.0f / (float)M_PI;
        blit_glyph_glow(dst, bmp, g.box_w, g.box_h, destCx, destCy, rotDeg, glowCol, glow);
        blit_glyph(dst, bmp, g.box_w, g.box_h, destCx, destCy, rotDeg, col, opa);
    }
}

// Defined at the bottom of this file: one line, no newlines, the original body.
static void straight_line_impl(const curved_text::Target &dst, const lv_font_t *font, const char *str,
                               float bx, float by, lv_color_t col, int glow, lv_color_t glowCol,
                               int align, lv_opa_t opa, const curved_text::Pill &pill);

void curved_text::draw_straight(const Target &dst, const lv_font_t *font, const char *str,
                                float bx, float by, lv_color_t col, int glow, lv_color_t glowCol,
                                int align, lv_opa_t opa, const Pill &pill, int lineGap) {
    if (!dst.buf || !font || !str || !str[0]) return;

    // One line is the overwhelmingly common case and takes the identical path it always did:
    // no copy, no arithmetic, nothing to move a design that already exists.
    int lines = 1;
    for (const char *p = str; *p; ++p) if (*p == '\n') ++lines;
    if (lines == 1) { straight_line_impl(dst, font, str, bx, by, col, glow, glowCol, align, opa, pill); return; }

    // Centre the BLOCK on `by`, which is what the LVGL label these lines replaced did, so
    // the config address lands on the anchor a design already positioned rather than hanging
    // off the bottom of it.
    const float step = (float)lv_font_get_line_height(font) + (float)lineGap;
    float y = by - (float)(lines - 1) * step * 0.5f;
    char line[80];
    for (const char *p = str; ; y += step) {
        const char *nl = strchr(p, '\n');
        const size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const size_t room = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, room);
        line[room] = '\0';
        straight_line_impl(dst, font, line, bx, y, col, glow, glowCol, align, opa, pill);
        if (!nl) break;
        p = nl + 1;
    }
}

// One line, already free of newlines: draw_straight's original body, arithmetic untouched.
static void straight_line_impl(const curved_text::Target &dst, const lv_font_t *font, const char *str,
                               float bx, float by, lv_color_t col, int glow, lv_color_t glowCol,
                               int align, lv_opa_t opa, const curved_text::Pill &pill) {
    if (!str[0]) return;
    const int n = (int)strlen(str), cap = n < 80 ? n : 80;
    float w[80], total = 0.0f;
    for (int i = 0; i < cap; ++i) {
        lv_font_glyph_dsc_t g;
        w[i] = lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)str[i], 0) ? (float)g.adv_w : 0.0f;
        total += w[i];
    }
    const float startX = (align == 1) ? (bx - total / 2.0f) : (align == 2) ? (bx - total) : bx;
    const float lineH = (float)lv_font_get_line_height(font), desc = (float)font->base_line;
    const float halfMid = (lineH - 2.0f * desc) * 0.5f;

    // The plate first, sized to the run this call is about to lay down. The padding matches
    // what the weather map's credit label has always used (8 across, 2 down), because that
    // one is an LVGL label with LVGL's own padding and the two have to look like the same
    // control when a design moves a line from one screen to another.
    if (pill.opa) {
        const float padX = 8.0f, padY = 2.0f;
        fill_round_rect(dst,
                        (int)lroundf(startX - padX), (int)lroundf(by - lineH / 2.0f - padY),
                        (int)lroundf(startX + total + padX), (int)lroundf(by + lineH / 2.0f + padY),
                        pill.radius, pill.col, pill.opa);
    }

    float x = startX;
    for (int i = 0; i < cap; ++i) {
        lv_font_glyph_dsc_t g;
        if (lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)str[i], 0)) {
            const uint8_t *bmp = lv_font_get_glyph_bitmap(font, (uint32_t)(uint8_t)str[i]);
            if (bmp && g.box_w && g.box_h) {
                const float destCx = x + (float)g.ofs_x + (float)g.box_w * 0.5f;
                const float destCy = by + halfMid - (float)g.ofs_y - (float)g.box_h * 0.5f;
                blit_glyph_glow(dst, bmp, g.box_w, g.box_h, destCx, destCy, 0.0f, glowCol, glow);
                blit_glyph(dst, bmp, g.box_w, g.box_h, destCx, destCy, 0.0f, col, opa);
            }
        }
        x += w[i];
    }
}
