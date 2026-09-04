#include "wind_notice.h"

#include "clock_wind.h"
#include "theme_style.h"
#include "theme_font.h"
#include "custom_sprite.h"
#include "config.h"
#include "theme_audio.h"
#include "app_shell.h"
#ifdef ARDUINO
#include "audio.h"
#else
// The simulator builds no audio module and has no speaker to send a tick to. A desktop
// window cannot answer "does the ratchet sound right" anyway, so the calls compile out
// rather than being faked.
#define audio_play(x) ((void)0)
#define audio_play_pcm(p, n) ((void)0)
#define AUDIO_WIND    0
#define AUDIO_CHIME   0
#endif

#include <stdio.h>
#include <lvgl.h>
#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace {

lv_obj_t *s_panel = nullptr;
lv_obj_t *s_ring  = nullptr;
lv_obj_t *s_crank = nullptr;

// Descriptors over the decoded sprites. custom_sprite hands back RGB565 plus alpha at three
// bytes a pixel, which is exactly LV_IMG_CF_TRUE_COLOR_ALPHA, so there is nothing to convert
// and LVGL can rotate the crank itself.
lv_img_dsc_t s_bgDsc = {};
lv_img_dsc_t s_crankDsc = {};

void describe(lv_img_dsc_t &d, const CustomSprite &sp) {
    d.header.always_zero = 0;
    d.header.w = sp.w;
    d.header.h = sp.h;
    d.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    d.data_size = (uint32_t)sp.w * sp.h * 3;
    d.data = sp.data;
}

// Last detent that made a sound. A hundred detents at one cue each would queue behind a
// playback path that holds the amplifier up for about a tenth of a second per tick, so a
// brisk wind would still be clicking long after it finished. Every other detent is enough
// to read as a ratchet and stays ahead of the turning.
// A SWEEP, not a notch.
//
// Nobody winds one click at a time. A hand turns four or five clicks in one motion, pauses,
// and goes again, and the sound of winding is the sound of that whole motion. Zion recorded
// exactly that: a few clicks of ratchet, one file. Firing it per detent stacked five copies
// of a five-click sound on top of each other, which is the digital mush he heard.
//
// So the first detent after a pause starts a sweep and plays. Every detent inside that sweep
// is silent, because the sound already running IS the sound of them. Deliberately not one to
// one with the knob, which is the thing he had to say twice.
// 150 ms, down from 250. Inside one sweep the detents arrive every 30 to 80 ms; between two
// sweeps the hand repositions, which is quick. 250 was slow enough to miss somebody rolling
// again promptly and read it as one long sweep.
constexpr uint32_t SWEEP_GAP_MS = 150;

uint32_t s_lastDetentMs = 0;
uint32_t s_lastRefrMs  = 0;

// Instrumentation for the winding path only, and only on the device. Cleared each time it
// reports.
uint32_t s_refrMs   = 0;
uint32_t s_logAt    = 0;
int      s_sinceLog = 0;

uint32_t now_ms() { return lv_tick_get(); }

// Tenths of a degree, which is what LVGL wants. One crank turn per knob turn: anything else
// is a gear ratio nobody asked for, and the point is that it moves the way your hand does.
int16_t crank_angle() {
    const int per = clock_wind::DETENTS_PER_TURN;
    if (per <= 0) return 0;
    const float turns = (float)clock_wind::progress() / (float)per;
    return (int16_t)((int)(turns * 3600.0f) % 3600);
}

// The compiled ladder, and nothing between its rungs. LVGL fonts are glyph bitmaps rather
// than outlines, so a size this binary was not built with cannot be drawn at any quality;
// asking for one and getting the nearest is how a theme silently redesigns itself. Same
// switch ticker_view.cpp uses, and unknown values land on a default rather than the closest.
const lv_font_t *font_for_px(int px) {
    switch (px) {
        case 14: return &lv_font_montserrat_14;
        case 16: return &lv_font_montserrat_16;
        case 18: return &lv_font_montserrat_18;
        case 20: return &lv_font_montserrat_20;
        case 22: return &lv_font_montserrat_22;
        case 26: return &lv_font_montserrat_26;
        case 28: return &lv_font_montserrat_28;
        default: return &lv_font_montserrat_20;
    }
}

// One line of the wind screen. ml/mr are the band it may use, so the words wrap inside it and
// an uneven pair shifts the block sideways rather than only narrowing it. slot picks the
// theme's face; where the theme shipped none, the compiled ladder stands in at the size the
// design asked for, which is what every Orb below THEME_CAPS 42 draws.
//
// LVGL breaks on \n by itself, so a carriage return somebody typed in Studio is a line break
// here with nothing to do about it. That is why the words are stored whole rather than split.
lv_obj_t *line(lv_obj_t *parent, const char *text, int px, uint32_t color, int y,
               int ml, int mr, int slot) {
    lv_obj_t *l = lv_label_create(parent);
    const int band = SCREEN_W - ml - mr;
    if (band > 20) {
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, band);
    }
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    const lv_font_t *f = theme_font::wind_has_font(slot)
        ? (slot == 0 ? theme_font::wind_title()
         : slot == 1 ? theme_font::wind_ask()
                     : theme_font::wind_turns())
        : font_for_px(px);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    // Centred on the BAND, not on the screen, so the margins mean what they say.
    lv_obj_align(l, LV_ALIGN_CENTER, (lv_coord_t)((ml - mr) / 2), (lv_coord_t)y);
    return l;
}

void ensure() {
    if (s_panel) return;
    const theme_style::Clock &c = theme_style::clock();

    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(c.windBg), 0);
    // The design's own opacity. Under full, the clock it is asking you to wind shows through,
    // which is the point: this is a scrim rather than a screen that replaces the dial.
    lv_obj_set_style_bg_opa(s_panel, (lv_opa_t)c.windBgOpa, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    // ZERO PADDING, and it is not cosmetic. lv_obj_create carries a default style with
    // padding, and a child placed by lv_obj_set_pos is positioned inside the content box, so
    // every absolute coordinate on this panel was silently offset by it. The labels and the
    // gauge are centred, which is symmetric and hid it; the crank is the only thing placed by
    // an exact point, so the crank was the only thing visibly in the wrong place. Zion: the
    // fulcrum is not quite in the centre on the Orb but is in the preview.
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    // The picture, over the colour and under everything else. A design can have a flat wash,
    // a photograph, or a photograph with a wash over it, which is why the colour did not move
    // out of the way when this arrived.
    if (c.windImageOn) {
        const CustomSprite bg = wind_background();
        if (bg.data) {
            describe(s_bgDsc, bg);
            lv_obj_t *img = lv_img_create(s_panel);
            lv_img_set_src(img, &s_bgDsc);
            lv_obj_center(img);
        }
    }

    // The wind gauge, out at the bezel where a circular screen has room to spare and where it
    // reads as the rim of the mechanism rather than as a progress bar. Starts at twelve and
    // fills clockwise, which is the direction that winds it.
    if (c.windRingShow) {
    s_ring = lv_arc_create(s_panel);
    const int d = c.windRingR * 2;
    lv_obj_set_size(s_ring, d, d);
    lv_obj_center(s_ring);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, clock_wind::detentsForFullWind());
    lv_arc_set_value(s_ring, clock_wind::progress());
    // Not a control. It reports the wind; the knob is what moves it, and a stray touch on the
    // glass must not be able to claim four turns nobody made.
    lv_obj_remove_style(s_ring, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, c.windRingWidth, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, c.windRingWidth, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(c.windRingTrack), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(c.windRingFill), LV_PART_INDICATOR);
    }

    // The crank, turned by the knob about its own pivot. Positioned by that PIVOT rather than
    // its middle, the same arithmetic every hand uses: the artwork is trimmed to its ink, so
    // the turning point is wherever the designer put it inside that crop and the sprite is
    // offset to bring it to the spot on the dial.
    if (c.windCrankOn) {
        const CustomSprite cr = wind_crank();
        if (cr.data) {
            describe(s_crankDsc, cr);
            s_crank = lv_img_create(s_panel);
            lv_img_set_src(s_crank, &s_crankDsc);
            // Rotation without antialiasing. LVGL resamples every pixel of a turned image,
            // and with a smooth filter that is the most expensive thing on this screen by a
            // long way: the crank redraws on every detent, over a full-screen background that
            // has to be recomposited under it. Zion had a visible lag between his hand and the
            // crank. Off, the edges are a shade harder and the turn keeps up.
            lv_img_set_antialias(s_crank, false);
            lv_img_set_pivot(s_crank, c.windCrankPX, c.windCrankPY);
            lv_obj_set_pos(s_crank, (lv_coord_t)(c.windCrankX - c.windCrankPX),
                                    (lv_coord_t)(c.windCrankY - c.windCrankPY));
            lv_img_set_angle(s_crank, crank_angle());
        }
    }

    if (c.windTitleShow) line(s_panel, c.windTitle, c.windTitleSize, c.windTitleCol, c.windTitleY, c.windTitleML, c.windTitleMR, 0);
    if (c.windAskShow)   line(s_panel, c.windAsk,   c.windAskSize,   c.windAskCol,   c.windAskY,   c.windAskML,   c.windAskMR,   1);

    if (c.windTurnsShow) {
        // Built rather than written, because the number is the theme's. Writing "five turns"
        // as a string would have it keep saying five while the knob wanted three.
        const int n = clock_wind::turnsForFullWind();
        static const char *WORDS[] = { "one", "two", "three", "four", "five", "six", "seven",
                                       "eight", "nine", "ten" };
        char buf[64];
        if (n >= 1 && n <= 10) snprintf(buf, sizeof(buf), "%s turn%s to the right", WORDS[n - 1], n == 1 ? "" : "s");
        else                   snprintf(buf, sizeof(buf), "%d turns to the right", n);
        line(s_panel, buf, c.windTurnsSize, c.windTurnsCol, c.windTurnsY, c.windTurnsML, c.windTurnsMR, 2);
    }
}

}  // namespace

bool wind_notice::showing() { return s_panel != nullptr; }

void wind_notice::tick() {
    // Only over the clock. The mainspring belongs to that screen, and covering the flight
    // tracker with a demand about a different app would be the device interrupting itself.
    const bool want = clock_wind::enabled()
                   && clock_wind::noticeOn()
                   && clock_wind::stopped()
                   && !app_shell::browsing()
                   && app_shell::index() == app_shell::APP_CLOCK;

    if (want && !s_panel) {
        ensure();
        lv_refr_now(NULL);
#ifdef ARDUINO
        Serial.println("[wind] wound down; asking for a wind");
#endif
        return;
    }
    if (!want && s_panel) dismiss();
}

void wind_notice::turn(int delta) {
    if (!s_panel) return;
    if (!clock_wind::turn(delta)) return;    // wrong way: a crown that slips

    if (clock_wind::justWound()) {
        // Straight back to the clock, running, at the true time. Nothing to set: the RTC
        // never stopped and the network still agrees with it.
        //
        // No sound here. There was a chime on completion and Zion cut it: the reward for
        // winding a watch is that it starts, not a noise congratulating you. The ticks while
        // you turn are the sound this gesture has.
        dismiss();
        return;
    }
    if (s_crank) lv_img_set_angle(s_crank, crank_angle());
    if (s_ring || s_crank) {
        if (s_ring) lv_arc_set_value(s_ring, clock_wind::progress());
        // Drawn HERE, not at the bottom of the loop.
        //
        // main.cpp handles the knob first and calls lv_timer_handler() last, on purpose, so
        // a detent is acted on by the very next render rather than the one after. That is
        // right for a press, which happens once. It is wrong for winding, which is the only
        // thing on this device that answers a CONTINUOUS turn: the gauge then moved a whole
        // loop period after the knob did, and Zion reported it as massive lag. The arc's own
        // invalidation is the changed sector only, so this is a small repaint, not a frame.
        // Redrawn here rather than at the bottom of the loop, but no more than every 40 ms.
        //
        // The immediate repaint is what made the gauge track the knob instead of trailing a
        // whole loop behind it. With a background picture and a rotating crank on the same
        // screen, one repaint costs real time, and forcing one per detent means a brisk wind
        // asks for more redrawing than the chip can do and every one of them lands late.
        // Twenty five a second is faster than the panel refreshes anyway.
        const uint32_t t0 = now_ms();
        if (t0 - s_lastRefrMs >= 40) {
            s_lastRefrMs = t0;
            lv_refr_now(NULL);
            s_refrMs += now_ms() - t0;
        }
    }
    {
        const uint32_t t = now_ms();
        const bool newSweep = (t - s_lastDetentMs) > SWEEP_GAP_MS;
        s_lastDetentMs = t;
        // The theme's own sound if it shipped one, otherwise the built-in tick. A Steam Punk
        // clock and an Aviator chronometer have no more business sounding alike than they do
        // sharing a typeface.
        // RETRIGGERS, rather than waiting for the clip to finish.
        //
        // The first version left a sound in flight alone, on the reasoning that restarting a
        // ratchet mid-phrase would sound doubled. That was written when playback could not be
        // stopped, so a restart really would have layered. It can be stopped now: every new
        // request abandons the one in flight and clears the buffer. Zion rolls two or three
        // more sweeps inside one clip and wants each one to sound, which is what a real
        // ratchet does. A sound that ignores you until it has finished its sentence is the
        // thing that felt wrong.
        if (clock_wind::soundOn() && newSweep) {
            size_t n = 0;
            if (const uint8_t *pcm = theme_audio::wind(n)) audio_play_pcm(pcm, n);
            else                                           audio_play(AUDIO_WIND);
        }
    }
#ifdef ARDUINO
    // Every twenty detents, so the log is a handful of lines per wind rather than a hundred.
    // Prints what it actually costs, because "it feels laggy" and "the redraw is slow" are
    // different claims and only one of them is fixable here.
    if (++s_sinceLog >= 20) {
        const uint32_t now = now_ms();
        Serial.printf("[wind] %d detents in %lu ms (%lu ms redrawing), at %d of %d\n",
                      s_sinceLog, (unsigned long)(now - s_logAt), (unsigned long)s_refrMs,
                      clock_wind::progress(), clock_wind::detentsForFullWind());
        s_sinceLog = 0; s_logAt = now; s_refrMs = 0;
    }
#endif
}

void wind_notice::dismiss() {
    if (!s_panel) return;
    lv_obj_del(s_panel);
    s_panel = nullptr;
    s_ring  = nullptr;
    s_crank = nullptr;
    // Repaint what was underneath, by hand, twice. Same lesson as update_ui::destroy() and
    // knob_help::dismiss(): a panel on lv_layer_top() does not always leave the screen
    // beneath it fully reclaimed, and what is left is a strip that survives until something
    // else happens to draw over it.
    if (lv_obj_t *scr = lv_scr_act()) lv_obj_invalidate(scr);
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(NULL);
    lv_timer_t *again = lv_timer_create([](lv_timer_t *t) {
        if (lv_obj_t *scr = lv_scr_act()) lv_obj_invalidate(scr);
        lv_obj_invalidate(lv_layer_top());
        lv_timer_del(t);
    }, 150, nullptr);
    if (again) lv_timer_set_repeat_count(again, 1);
}
