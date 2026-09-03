#include "knob_help.h"

#include <lvgl.h>
#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace {

lv_obj_t *s_panel = nullptr;

// Built once and kept, rather than made and destroyed each time. The panel is four labels
// and it is reached from an input handler; holding it costs a few hundred bytes and removes
// an allocation from the path where somebody is already waiting for the screen to answer.
void ensure() {
    if (s_panel) return;

    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Zion's words, 2026-09-03, after reading the first version on the glass. One hint,
    // one sentence, rather than a titled list of three gestures: the only one worth
    // teaching is the rock, and the other two were being explained to somebody who had
    // just demonstrated they could work a knob.
    //
    // "activate to the main menu" in his note is a dictation slip for "activate the main
    // menu"; the stray preposition is dropped and nothing else about the sentence is mine.
    lv_obj_t *label = lv_label_create(s_panel);
    lv_label_set_text(label, "Hint:");
    lv_obj_set_style_text_color(label, lv_color_hex(0x9aa4b0), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, -84);

    // Wrapped by LVGL at a width that clears the bezel. It comes out four lines at this
    // size, and 340 px is the widest that block can be and still sit inside a 466 px circle
    // with the curve biting at the ends of the top and bottom lines.
    lv_obj_t *body = lv_label_create(s_panel);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, 340);
    lv_label_set_text(body,
                      "To activate the main menu from any app, "
                      "\"rock\" the knob by quickly turning the "
                      "knob left and then right");
    lv_obj_set_style_text_color(body, lv_color_white(), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 8);

    // Kept, quietly. A screen that appeared uninvited must never be one you have to work
    // out how to close.
    //
    // It says BOTH inputs because both have always worked: input_router clears this on a
    // press or a turn either way, and swallows whichever one did it. Saying only "push"
    // described a narrower device than the one underneath, which is the same fault as the
    // Ready notice demanding a press and making a screen about the knob being yours again
    // the one screen where most of the knob did nothing.
    lv_obj_t *hint = lv_label_create(s_panel);
    lv_label_set_text(hint, "turn or push to carry on");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x5a636e), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 122);
}

}  // namespace

bool knob_help::showing() { return s_panel != nullptr; }

void knob_help::show() {
    if (s_panel) return;
    ensure();
    // Drawn now rather than on the next timer tick. This is the frame that answers a press,
    // and the complaint it exists to end is a device that does nothing when you touch it.
    lv_refr_now(NULL);
#ifdef ARDUINO
    Serial.println("[knob_help] a press had nowhere to go; showing what the knob does");
#endif
}

void knob_help::dismiss() {
    if (!s_panel) return;
    lv_obj_del(s_panel);
    s_panel = nullptr;
    // Repaint what was underneath, by hand, twice.
    //
    // Straight off update_ui::destroy(), which learned it the expensive way: a panel on
    // lv_layer_top() does not always leave the screen beneath it fully reclaimed when it is
    // deleted, and what is left is a strip of the overlay that survives until something else
    // happens to draw over that region. One invalidate now and one on a later tick costs two
    // repaints on a path that runs a handful of times in a device's life.
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
