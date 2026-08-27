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
// Waiting to be replaced by a USB flash. Changes what the watchdog below means: see
// firmware_incoming().
bool      s_firmwareWait  = false;
// A ready notice is up and waiting for a press. See update_ui.h.
bool      s_awaitAck      = false;
lv_timer_t *s_autoClear   = nullptr;

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
    const bool had = s_panel != nullptr;
    if (s_panel) { lv_obj_del(s_panel); s_panel = nullptr; s_title = s_sub = s_hint = nullptr; }
    s_interrupted = false;
    s_rebootPending = false;
    s_firmwareWait = false;
    s_awaitAck = false;
    if (s_autoClear) { lv_timer_del(s_autoClear); s_autoClear = nullptr; }
    // Repaint everything underneath, by hand.
    //
    // Deleting an object normally invalidates the area it occupied and that is enough. This
    // one is not a normal case twice over: the panel sits on lv_layer_top() rather than on
    // the screen, and bake_done() calls this from boot code between explicit lv_refr_now()
    // calls rather than from the timer cycle LVGL expects to be running. Anything that
    // leaves a region unclaimed in that situation shows on this panel as a strip of whatever
    // the overlay last had there, which does not clear until something else happens to draw
    // over it.
    //
    // A whole-screen invalidate costs one repaint on a path that runs a handful of times in
    // a device's life, so there is no reason to be clever about which region it was.
    if (had) {
        if (lv_obj_t *scr = lv_scr_act()) lv_obj_invalidate(scr);
        lv_obj_invalidate(lv_layer_top());
        lv_refr_now(NULL);
    }
}

// Files stopped arriving and nothing rebooted us: the send died partway. Say so briefly,
// then get out of the way — the device underneath is still fully usable.
void watchdog_cb(lv_timer_t *) {
    if (!s_panel || s_rebootPending) return;
    const uint32_t idle = millis() - s_lastActivity;
    // Waiting on a firmware flash inverts what this timer means. A flash stops this code
    // dead, so the fact that this callback is running AT ALL proves the flash never began:
    // the browser could not take the port, the device chooser was dismissed, or the tab was
    // closed. Ninety seconds covers a slow start and still refuses to leave a healthy Orb
    // wearing an update screen forever, which is the same promise the branch below makes.
    if (s_firmwareWait) {
        if (idle > 90000) {
#ifdef ARDUINO
            Serial.println("[update_ui] no flash arrived in 90s - clearing the firmware overlay");
#endif
            destroy();
        }
        return;
    }
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

void file_progress(const char *name, int count, uint32_t bytes) {
    ensure();                       // first chunk of the first file also raises the overlay
    s_lastActivity = millis();      // the whole point: this is activity
    if (s_interrupted) {            // a big file mid-flight is not an interruption after all
        s_interrupted = false;
        lv_label_set_text(s_title, "Updating");
    }
    // Repainting per 400-byte chunk would spend more time in LVGL than on the transfer.
    static uint32_t s_painted = 0;
    if (millis() - s_painted < 500) return;
    s_painted = millis();
    char b[112];
    snprintf(b, sizeof(b), "Step 1 of 3 - receiving files (%d)\n%.28s  %lu KB",
             count + 1, name ? name : "", (unsigned long)(bytes / 1024));
    lv_label_set_text(s_sub, b);
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

void booting(const char *what) {
    ensure();
    s_lastActivity = millis();
    lv_label_set_text(s_title, "Starting up");
    lv_label_set_text(s_sub, what ? what : "");
    lv_label_set_text(s_hint, "The knob will not answer until this clears.");
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -60);
    lv_obj_align(s_sub,   LV_ALIGN_CENTER, 0,   0);
    lv_obj_align(s_hint,  LV_ALIGN_CENTER, 0,  60);
    // Paint NOW. The whole reason this exists is that the caller is about to block for
    // twenty seconds without servicing LVGL, so a queued repaint would never be drawn.
    lv_refr_now(NULL);
}

void auto_clear_cb(lv_timer_t *) { destroy(); }

void ready(bool needsAck) {
    ensure();
    s_lastActivity = millis();
    s_awaitAck = needsAck;
    lv_label_set_text(s_title, "Ready");
    lv_label_set_text(s_sub, needsAck
        ? "The update is finished.\nEverything is running."
        : "");
    // Names the turn rather than the press, because a turn is the smaller motion and both
    // work. Shorter than the string it replaced, so the two-line layout below is unchanged.
    lv_label_set_text(s_hint, needsAck ? "Turn the knob to begin." : "");
    // Checked with `program --readyshot`. One line at 398 px was most of the dial's width
    // and the bezel crowds it; two shorter lines sit comfortably inside the glass.
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, needsAck ? -62 : 0);
    lv_obj_align(s_sub,   LV_ALIGN_CENTER, 0,   6);
    lv_obj_align(s_hint,  LV_ALIGN_CENTER, 0,  78);
    if (s_autoClear) { lv_timer_del(s_autoClear); s_autoClear = nullptr; }
    if (!needsAck) {
        // Long enough to read, short enough that nobody waits on it. An ordinary power-on
        // should not need permission to become a clock.
        s_autoClear = lv_timer_create(auto_clear_cb, 1200, nullptr);
        lv_timer_set_repeat_count(s_autoClear, 1);
    }
    lv_refr_now(NULL);
#ifdef ARDUINO
    Serial.printf("[update_ui] ready (%s)\n", needsAck ? "waiting for the knob" : "clearing itself");
#endif
}

bool awaitingAck() { return s_awaitAck && s_panel; }

void ackReady() {
    if (!s_awaitAck) return;
    destroy();
#ifdef ARDUINO
    Serial.println("[update_ui] ready notice acknowledged - the knob is yours");
#endif
}

void firmware_incoming() {
    ensure();
    s_lastActivity  = millis();
    s_interrupted   = false;
    s_rebootPending = false;
    s_firmwareWait  = true;
    lv_label_set_text(s_title, "Updating firmware");
    // Says the quiet part out loud. The complaint this exists to answer is not "what is it
    // doing", it is "has it locked up", so the screen not changing is named as the expected
    // behaviour rather than left to be inferred from a motionless panel.
    lv_label_set_text(s_sub,
                      "This usually takes a minute or two.\n"
                      "The screen will not change while it works.\n"
                      "It restarts itself when it is finished.");
    lv_label_set_text(s_hint, "Keep it plugged in. Do not unplug.");
    // Re-space for three lines. The shared offsets (-40 / +6 / +60) are set for the two-line
    // subtitle the theme-install states use, and a third line grows the block from its centre
    // in both directions, closing the gap under the title to almost nothing. Checked with
    // `program --updateshot`, which exists precisely because three labels at fixed offsets is
    // the layout that silently collides the moment one of them gains a line.
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -78);
    lv_obj_align(s_sub,   LV_ALIGN_CENTER, 0,   2);
    lv_obj_align(s_hint,  LV_ALIGN_CENTER, 0,  78);
    if (!s_timer) s_timer = lv_timer_create(watchdog_cb, 1000, nullptr);
    // The entire point of this function, and the one line that cannot be dropped. Everything
    // above only queues a repaint; the chip is moments from being reset into its bootloader,
    // and a frame still sitting in LVGL's buffer when that happens is never drawn at all.
    // draw_block() pushes over QSPI and blocks, so when this returns the pixels are on glass.
    lv_refr_now(NULL);
#ifdef ARDUINO
    Serial.println("[update_ui] firmware flash incoming - painted the notice while we still can");
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
