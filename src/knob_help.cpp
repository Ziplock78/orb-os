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

    // "Three moves" rather than "the knob": naming the count up front is what turns a wall
    // of instructions into a list somebody expects to end. The Orb page says the same three
    // in the same order, so a person who read it meets the words twice rather than two
    // descriptions of the same object.
    lv_obj_t *title = lv_label_create(s_panel);
    lv_label_set_text(title, "One knob,\nthree moves");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -128);

    // The rock is the line this screen exists for, so it is the one in white with the other
    // two dimmed around it. Turning and pushing are here for context, not because anybody
    // needed telling.
    lv_obj_t *turn = lv_label_create(s_panel);
    lv_label_set_text(turn, "Turn to move");
    lv_obj_set_style_text_color(turn, lv_color_hex(0x9aa4b0), 0);
    lv_obj_set_style_text_font(turn, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(turn, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(turn, LV_ALIGN_CENTER, 0, -46);

    // The same words the Orb page uses. If one of these two ever changes, the other is
    // wrong, and a person meeting both is being told the device works two ways.
    lv_obj_t *rock = lv_label_create(s_panel);
    lv_label_set_text(rock, "Rock it back and forth\nfor the menu");
    lv_obj_set_style_text_color(rock, lv_color_white(), 0);
    lv_obj_set_style_text_font(rock, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_align(rock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(rock, LV_ALIGN_CENTER, 0, 8);

    lv_obj_t *push = lv_label_create(s_panel);
    lv_label_set_text(push, "Push to choose");
    lv_obj_set_style_text_color(push, lv_color_hex(0x9aa4b0), 0);
    lv_obj_set_style_text_font(push, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(push, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(push, LV_ALIGN_CENTER, 0, 74);

    // Says how to leave, because a screen that appeared uninvited must never be one you have
    // to work out how to close. Any input clears it, so naming the nearest one is enough.
    lv_obj_t *hint = lv_label_create(s_panel);
    lv_label_set_text(hint, "push to carry on");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x5a636e), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 138);
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
