#include "wind_notice.h"

#include "clock_wind.h"
#include "app_shell.h"
#ifdef ARDUINO
#include "audio.h"
#else
// The simulator builds no audio module and has no speaker to send a tick to. A desktop
// window cannot answer "does the ratchet sound right" anyway, so the calls compile out
// rather than being faked.
#define audio_play(x) ((void)0)
#define AUDIO_WIND    0
#define AUDIO_CHIME   0
#endif

#include <lvgl.h>
#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace {

lv_obj_t *s_panel = nullptr;
lv_obj_t *s_ring  = nullptr;

// Last detent that made a sound. A hundred detents at one cue each would queue behind a
// playback path that holds the amplifier up for about a tenth of a second per tick, so a
// brisk wind would still be clicking long after it finished. Every other detent is enough
// to read as a ratchet and stays ahead of the turning.
uint32_t s_lastClickMs = 0;
constexpr uint32_t CLICK_GAP_MS = 55;

uint32_t now_ms() { return lv_tick_get(); }

void ensure() {
    if (s_panel) return;

    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    // The wind gauge, right out at the bezel where a circular screen has room to spare and
    // where it reads as the rim of the mechanism rather than as a progress bar. Starts at
    // twelve and fills clockwise, which is the direction that winds it.
    s_ring = lv_arc_create(s_panel);
    // 424, not 440. The panel is square and the glass is a circle inscribed in it, so a
    // ring at radius 220 sits thirteen pixels from the edge of a 466 px dial and any bezel
    // overlap eats it. Pulled in to leave twenty.
    lv_obj_set_size(s_ring, 424, 424);
    lv_obj_center(s_ring);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, clock_wind::DETENTS_FOR_FULL_WIND);
    lv_arc_set_value(s_ring, 0);
    // Not a control. It reports the wind; the knob is what moves it, and a stray touch on
    // the glass must not be able to claim four turns nobody made.
    lv_obj_remove_style(s_ring, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_ring, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(0x22282f), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(0xd8b56a), LV_PART_INDICATOR);

    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "The clock has\nwound down");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -66);

    lv_obj_t *ask = lv_label_create(s_panel);
    lv_label_set_long_mode(ask, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ask, 300);
    lv_label_set_text(ask, "Please wind the clock using the knob");
    lv_obj_set_style_text_color(ask, lv_color_hex(0x9aa4b0), 0);
    lv_obj_set_style_text_font(ask, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(ask, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ask, LV_ALIGN_CENTER, 0, 14);

    lv_obj_t *how = lv_label_create(s_panel);
    lv_label_set_text(how, "five turns to the right");
    lv_obj_set_style_text_color(how, lv_color_hex(0x5a636e), 0);
    lv_obj_set_style_text_font(how, &lv_font_montserrat_16, 0);
    lv_obj_align(how, LV_ALIGN_CENTER, 0, 84);
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
        audio_play(AUDIO_CHIME);
        dismiss();
        return;
    }
    if (s_ring) lv_arc_set_value(s_ring, clock_wind::progress());
    if (clock_wind::soundOn()) {
        const uint32_t t = now_ms();
        if (t - s_lastClickMs >= CLICK_GAP_MS) { s_lastClickMs = t; audio_play(AUDIO_WIND); }
    }
}

void wind_notice::dismiss() {
    if (!s_panel) return;
    lv_obj_del(s_panel);
    s_panel = nullptr;
    s_ring  = nullptr;
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
