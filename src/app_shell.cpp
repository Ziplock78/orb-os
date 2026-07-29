#include "app_shell.h"
#include <Arduino.h>
#include "config.h"     // SCREEN_W / SCREEN_H

namespace {
    struct App {
        lv_obj_t     *screen;
        const char   *name;
        app_action_t  onPress;
        app_turn_t    onTurn;
        bool          capture;
        app_action_t  onEnter;
    };

    constexpr int   MAX_APPS      = 8;
    constexpr uint32_t ANIM_MS    = 250;   // slide duration between apps

    App  s_apps[MAX_APPS];
    int  s_count    = 0;
    int  s_cur      = 0;
    bool s_captured = false;

    // app-switcher overlay (lives on the top layer, above whatever screen is loaded)
    bool      s_browsing     = false;
    lv_obj_t *s_overlay      = nullptr;
    lv_obj_t *s_overlayLabel = nullptr;
    uint32_t  s_browseTouch  = 0;          // millis() of the last browse interaction
    constexpr uint32_t BROWSE_SETTLE_MS = 3000;   // auto-enter the shown app after this idle

    void show_overlay(const char *name) {
        if (!s_overlay) return;
        lv_label_set_text(s_overlayLabel, name);
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        s_browseTouch = millis();          // any turn/open restarts the settle countdown
    }
    void hide_overlay() {
        if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        s_browsing = false;
    }

    void commit_current() {                // enter the app the overlay is showing
        hide_overlay();
        s_captured = s_apps[s_cur].capture;
        if (s_apps[s_cur].onEnter) s_apps[s_cur].onEnter();
    }

    // Runs on the LVGL thread; if the switcher has sat idle long enough, drop into the shown app.
    void browse_tick(lv_timer_t * /*t*/) {
        if (s_browsing && (millis() - s_browseTouch) >= BROWSE_SETTLE_MS) commit_current();
    }

    void load(int idx, bool animate, bool forward) {
        if (idx < 0 || idx >= s_count || !s_apps[idx].screen) return;
        s_cur = idx;
        s_captured = s_apps[idx].capture;   // menu apps grab the knob on entry
        if (s_apps[idx].screen != lv_scr_act()) {   // apps sharing a screen (radar/weather) skip the load
            if (animate) {
                lv_scr_load_anim_t a = forward ? LV_SCR_LOAD_ANIM_MOVE_LEFT
                                               : LV_SCR_LOAD_ANIM_MOVE_RIGHT;
                lv_scr_load_anim(s_apps[idx].screen, a, ANIM_MS, 0, false /*don't delete old*/);
            } else {
                lv_scr_load(s_apps[idx].screen);
            }
        }
        if (s_apps[idx].onEnter) s_apps[idx].onEnter();
        Serial.printf("[shell] app %d/%d: %s\n", s_cur + 1, s_count, s_apps[idx].name);
    }
}

void app_shell::add(lv_obj_t *screen, const char *name,
                    app_action_t onPress, app_turn_t onTurn, bool capture, app_action_t onEnter) {
    if (s_count < MAX_APPS && screen) {
        s_apps[s_count].screen  = screen;
        s_apps[s_count].name    = name;
        s_apps[s_count].onPress = onPress;
        s_apps[s_count].onTurn  = onTurn;
        s_apps[s_count].capture = capture;
        s_apps[s_count].onEnter = onEnter;
        s_count++;
    }
}

void app_shell::add_active(const char *name,
                           app_action_t onPress, app_turn_t onTurn, bool capture, app_action_t onEnter) {
    add(lv_scr_act(), name, onPress, onTurn, capture, onEnter);
}

void app_shell::pressCurrent() {
    if (s_count && s_apps[s_cur].onPress) s_apps[s_cur].onPress();
}

bool app_shell::captured() { return s_captured; }

void app_shell::setCaptured(bool on) { s_captured = on; }

void app_shell::turnCurrent(int delta) {
    if (s_count && s_apps[s_cur].onTurn) s_apps[s_cur].onTurn(delta);
}

void app_shell::begin() {
    // Build the app-switcher overlay on the top layer so it floats over every screen.
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, SCREEN_W, SCREEN_H);
    lv_obj_center(s_overlay);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);   // dim the previewed app underneath
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_overlayLabel = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(s_overlayLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_overlayLabel, &lv_font_montserrat_48, 0);
    lv_obj_align(s_overlayLabel, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *hint = lv_label_create(s_overlay);
    lv_label_set_text(hint, "push to open");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9AA0A6), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 40);

    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(browse_tick, 200, nullptr);   // watches for the 3s browse settle

    if (s_count) load(0, false, true);
}

// First turn opens the switcher on the current app; further turns cycle apps.
void app_shell::browseTurn(int delta) {
    if (!s_count) return;
    if (!s_browsing) {
        s_browsing = true;
        show_overlay(s_apps[s_cur].name);
        return;
    }
    const int idx = (delta > 0) ? (s_cur + 1) % s_count
                                : (s_cur - 1 + s_count) % s_count;
    load(idx, false, delta > 0);       // load the app underneath (no slide; overlay is fixed on top)
    show_overlay(s_apps[s_cur].name);
}

// Push commits the shown app (hides the overlay); if not browsing, it's an app action.
void app_shell::browsePress() {
    if (s_browsing) commit_current();       // push commits the shown app
    else            pressCurrent();         // otherwise it's the app's own action
}

bool app_shell::browsing() { return s_browsing; }

void app_shell::openSwitcher() {
    if (!s_count) return;
    s_browsing = true;
    show_overlay(s_apps[s_cur].name);
}

void app_shell::next() {
    if (s_count) load((s_cur + 1) % s_count, true, true);
}

void app_shell::prev() {
    if (s_count) load((s_cur - 1 + s_count) % s_count, true, false);
}

int         app_shell::count() { return s_count; }
int         app_shell::index() { return s_cur; }
const char *app_shell::name()  { return s_count ? s_apps[s_cur].name : ""; }
