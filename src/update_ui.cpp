#include "update_ui.h"
#include <lvgl.h>
#include <stdio.h>
#ifdef ARDUINO
#include <Arduino.h>
#else
static unsigned long millis() { return 0; }
#endif

namespace update_ui {
namespace {

// One overlay, three moments: receiving files, restarting, baking. Deliberately plain
// LVGL objects with stock styling — this is a SYSTEM surface like the hold-to-reboot
// warning, not themeable content. An update screen that depended on the very theme being
// replaced would be drawing from the thing it is overwriting.
lv_obj_t *s_panel    = nullptr;
lv_obj_t *s_title    = nullptr;
lv_obj_t *s_sub      = nullptr;
lv_obj_t *s_hint     = nullptr;
lv_timer_t *s_timer  = nullptr;
uint32_t  s_lastActivity = 0;
bool      s_interrupted  = false;
bool      s_rebootPending = false;

void ensure() {
    if (s_panel) return;
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_panel);
    lv_label_set_text(s_title, "Updating");
    lv_obj_set_style_text_color(s_title, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_28, 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -40);

    s_sub = lv_label_create(s_panel);
    lv_label_set_text(s_sub, "");
    lv_obj_set_style_text_color(s_sub, lv_color_hex(0x9aa4b0), 0);
    lv_obj_set_style_text_font(s_sub, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_sub, LV_ALIGN_CENTER, 0, 6);

    s_hint = lv_label_create(s_panel);
    lv_label_set_text(s_hint, "Keep power connected. Do not unplug.");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x5a636e), 0);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 60);
}

void destroy() {
    if (s_timer) { lv_timer_del(s_timer); s_timer = nullptr; }
    if (s_panel) { lv_obj_del(s_panel); s_panel = nullptr; s_title = s_sub = s_hint = nullptr; }
    s_interrupted = false;
    s_rebootPending = false;
}

// Files stopped arriving and nothing rebooted us: the send died partway. Say so briefly,
// then get out of the way — the device underneath is still fully usable.
void watchdog_cb(lv_timer_t *) {
    if (!s_panel || s_rebootPending) return;
    const uint32_t idle = millis() - s_lastActivity;
    if (!s_interrupted && idle > 12000) {
        s_interrupted = true;
        lv_label_set_text(s_title, "Update interrupted");
        lv_label_set_text(s_sub, "The transfer stopped partway.\nNothing was changed. Install again from Orb Studio.");
#ifdef ARDUINO
        Serial.println("[update_ui] transfer went quiet for 12s — showing 'interrupted', will clear");
#endif
    } else if (s_interrupted && idle > 20000) {
        destroy();
#ifdef ARDUINO
        Serial.println("[update_ui] cleared the interrupted-update overlay");
#endif
    }
}

} // namespace

void file_received(const char *name, int count) {
    ensure();
    s_lastActivity = millis();
    if (s_interrupted) {   // the send resumed after a stall: back to the normal state
        s_interrupted = false;
        lv_label_set_text(s_title, "Updating");
    }
    char b[96];
    // Numbered, because the thing a person cannot tell from the desk is whether the device
    // is finished or merely between steps. Saying which step it is on says both.
    snprintf(b, sizeof(b), "Step 1 of 3 - receiving files (%d)\n%.40s", count, name ? name : "");
    lv_label_set_text(s_sub, b);
    if (!s_timer) s_timer = lv_timer_create(watchdog_cb, 1000, nullptr);
#ifdef ARDUINO
    if (count == 1) Serial.println("[update_ui] receiving files — update overlay up");
#endif
}

void rebooting() {
    // Only meaningful mid-update. A bare /reboot (a deploy script, a curl) on an idle
    // device should not flash an update screen for 400 ms on its way down.
    if (!s_panel) return;
    s_rebootPending = true;
    lv_label_set_text(s_title, "Restarting");
    lv_label_set_text(s_sub, "Step 2 of 3 - restarting.\nThe screen goes dark for a few seconds,\nthen it prepares the artwork. Not finished yet.");
#ifdef ARDUINO
    Serial.println("[update_ui] reboot incoming — told the user to expect the restart");
#endif
}

void bake_begin(int totalAssets) {
    ensure();
    s_lastActivity = millis();
    lv_label_set_text(s_title, "Installing update");
    char b[64];
    snprintf(b, sizeof(b), "Step 3 of 3 - preparing artwork\n0 of %d", totalAssets);
    lv_label_set_text(s_sub, b);
    // Blocking work follows (the bake), so paint now rather than waiting for a timer
    // tick that will not come.
    lv_refr_now(NULL);
#ifdef ARDUINO
    Serial.printf("[update_ui] bake starting: %d asset(s)\n", totalAssets);
#endif
}

void bake_progress(const char *assetName, int done, int totalAssets) {
    if (!s_panel) return;
    s_lastActivity = millis();
    char b[96];
    snprintf(b, sizeof(b), "Step 3 of 3 - preparing artwork\n%d of %d  %.32s", done, totalAssets, assetName ? assetName : "");
    lv_label_set_text(s_sub, b);
    lv_refr_now(NULL);
}

void bake_done() {
    destroy();
#ifdef ARDUINO
    Serial.println("[update_ui] install finished — update overlay down, boot continues");
#endif
}

} // namespace update_ui
