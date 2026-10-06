#include "update_ui.h"
#include <string.h>   // strcmp, for the _installed sentinel below
#include "ui.h"   // ui_splash_status(): during boot the splash narrates, not an overlay
// main.cpp. True while any update surface is up: the screen goes to full brightness no
// matter how dim the owner keeps it or how long it has sat idle, and comes back to its
// normal level when the surface goes down. See ensure() / destroy().
extern void host_update_bright(bool on);
#include <lvgl.h>
#include <stdio.h>
#ifdef ARDUINO
#include <Arduino.h>
#else
static unsigned long millis() { return 0; }
#endif

namespace update_ui {

// Defined near the bottom; file_received() below reaches back to it when the _installed
// sentinel arrives. Declared out here rather than inside the anonymous namespace, where
// it would be a second, unrelated `installed` and make the call ambiguous.
void installed(int files);

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
// The install SUCCEEDED and said so. Without this the watchdog below cannot tell the end of
// a transfer from the death of one: files stop arriving either way. See installed().
bool      s_finished     = false;
bool      s_rebootPending = false;
// Waiting to be replaced by a USB flash. Changes what the watchdog below means: see
// firmware_incoming().
bool      s_firmwareWait  = false;
// A ready notice is up and waiting for a press. See update_ui.h.
bool      s_awaitAck      = false;
lv_timer_t *s_autoClear   = nullptr;

void ensure() {
    if (s_panel) return;
    // Before the first pixel. An Orb that has dimmed for the night, or that its owner keeps
    // at a low level, was showing its update notice at that same low level, and a dim
    // "do not unplug" is not much better than none. Zion: the moment software starts
    // loading, firmware or theme, the screen goes to its brightest.
    host_update_bright(true);
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_panel);
    // The theme-install states leave this title alone, so it names the theme install. Every
    // other state sets its own. It used to say just "Updating", which had Zion looking for
    // what exactly was being updated.
    lv_label_set_text(s_title, "Updating theme");
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
    s_finished = false;
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
        host_update_bright(false);   // back to the owner's own level, idle clock restarted
        if (lv_obj_t *scr = lv_scr_act()) lv_obj_invalidate(scr);
        lv_obj_invalidate(lv_layer_top());
        lv_refr_now(NULL);
        // ...and again, a few frames later.
        //
        // One pass is demonstrably not always enough: a thin bright arc has been turning up
        // along the top edge of the dial after an install for a while now, and it survives
        // until something else repaints that region -- going to the app menu and back clears
        // it, which is a full redraw by another name. The pass above happens while boot code
        // is still driving lv_refr_now() by hand rather than the timer cycle LVGL expects, so
        // a region can be left unclaimed exactly once and then never revisited.
        //
        // A second pass on a normal timer tick costs one repaint on a path that runs a
        // handful of times in a device's life. It is a belt to go with the braces, and it is
        // honest about being one: I have not found what leaves the strip, only that a real
        // full redraw removes it.
        lv_timer_t *again = lv_timer_create([](lv_timer_t *t) {
            if (lv_obj_t *scr = lv_scr_act()) lv_obj_invalidate(scr);
            lv_obj_invalidate(lv_layer_top());
            lv_timer_del(t);
        }, 150, nullptr);
        if (again) lv_timer_set_repeat_count(again, 1);
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
    // A finished install is not a stalled one. installed() has already put the outcome on
    // screen, so leave it there: clear it after a few seconds, or hold it until the knob is
    // turned when it asked to be acknowledged.
    if (s_finished) {
        if (!s_awaitAck && idle > 6000) destroy();
        return;
    }
    if (!s_interrupted && idle > 12000) {
        s_interrupted = true;
        lv_label_set_text(s_title, "Update interrupted");
        // NOT "nothing was changed", which was never true. Files are written as they
        // arrive, so a transfer that stops partway leaves exactly the files that made it —
        // which is why _installed is written last and why a folder without it is not a
        // theme. Claiming a rollback that does not happen sent people looking for a fault
        // in the wrong place.
        lv_label_set_text(s_sub, "The transfer stopped partway.\nThe theme is incomplete. Install it again.");
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
    // A FILE ARRIVING AFTER A FINISHED NOTICE MEANS MORE WAS COMING.
    //
    // _installed ends A THEME, not necessarily the session: Orb Studio can push several in
    // one send, each with its own sentinel. Greg saw the consequence on 2026-10-06 — the
    // overlay said "Theme installed" when the first one landed and then sat there while the
    // second was still arriving, so a send that was half done looked finished. So the
    // terminal state is given up the moment another file turns up, and the next sentinel
    // announces the next theme.
    if (s_finished) {
        s_finished = false;
        lv_label_set_text(s_title, "Updating theme");
    }
    // _installed ARRIVING IS THE COMPLETION SIGNAL, whatever sent it.
    //
    // Every install path writes it LAST and nothing else does: the browser page sorts it to
    // the end, theme_pull queues it last, and the cable does the same. That invariant is
    // already load-bearing — a folder without _installed is not a theme, which is what makes
    // a half-finished transfer invisible rather than broken — so it can be trusted here too.
    //
    // This is what lets the cable path be fixed without Orb Studio changing: the explicit
    // signals (POST /installed, ?orb put-done) are better because they carry a count and
    // cannot be confused by a one-file push, but this one needs nothing from the sender.
    if (name && !strcmp(name, "_installed")) { installed(count); return; }
    if (s_interrupted) {   // the send resumed after a stall: back to the normal state
        s_interrupted = false;
        lv_label_set_text(s_title, "Updating theme");
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
    if (s_finished) {               // ...and a new file means the send is not over either
        s_finished = false;
        lv_label_set_text(s_title, "Updating theme");
    }
    if (s_interrupted) {            // a big file mid-flight is not an interruption after all
        s_interrupted = false;
        lv_label_set_text(s_title, "Updating theme");
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
    // While the boot splash is up, the message goes on the splash rather than over it.
    // A black notice appearing over the title card, then the title card coming back,
    // then the clock, read as the boot restarting. One screen, one line of status.
    if (ui_splash_status(what ? what : "Starting up")) return;
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

// AN INSTALL FINISHED. Say so, and mean it.
//
// Nothing ever called ready() outside the simulator's screenshot path, so no install on any
// transport — cable, WiFi pull, or the browser install page — ever reached a terminal state.
// Every one of them simply went quiet at the end, and twelve seconds later the watchdog
// declared "Update interrupted ... Nothing was changed" over a perfectly good install.
// Reported on the browser path and then on the cable path too, which is what showed it was
// not a quirk of one transport but a state that had been written and never wired up.
//
// The watchdog cannot work this out for itself: a finished transfer and a dead one both
// look like files stopping. Only the thing doing the sending knows, so it has to say.
void installed(int files) {
    ensure();
    s_lastActivity = millis();
    s_interrupted  = false;
    s_finished     = true;
    s_awaitAck     = false;
    lv_label_set_text(s_title, "Theme installed");
    char b[96];
    if (files > 0) snprintf(b, sizeof(b), "%d file%s written.\nPick it on the Orb to wear it.",
                            files, files == 1 ? "" : "s");
    else           snprintf(b, sizeof(b), "Pick it on the Orb to wear it.");
    lv_label_set_text(s_sub, b);
    lv_label_set_text(s_hint, "");
    if (!s_timer) s_timer = lv_timer_create(watchdog_cb, 1000, nullptr);
#ifdef ARDUINO
    Serial.printf("[update_ui] install finished: %d file(s)\n", files);
#endif
}

void ready(bool needsAck) {
    ensure();
    s_lastActivity = millis();
    s_awaitAck = needsAck;
    s_finished = true;   // a ready notice is an outcome, not a stall
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

// Boot is finished and nothing else is coming. Takes the boot notice down, wherever it
// was drawn, and shows NOTHING in its place: the splash's own hold and fade follow, and
// the clock after that. This replaced ready(), which put a "Ready" card over a clock that
// was already showing, which is exactly the "it looked done and then it wasn't" moment.
void booted() {
    ui_splash_status("");
    destroy();
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
    s_finished      = false;
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
    // Always the panel, never a line on the splash. This used to check whether the boot
    // splash was up and narrate "Preparing theme, k of n" along its bottom edge instead,
    // and after a theme push that is exactly what the user saw: the new theme's title card,
    // apparently finished, with small text under it saying it was not. The splash now goes
    // up after the bake (main.cpp), so during the bake this plain panel is the only thing
    // on the glass, and the title card's first appearance means the install is done.
    ensure();
    s_lastActivity = millis();
    // "Installing update, Step 3 of 3" was the language of a Studio install, which really
    // does have three steps. This screen also runs when somebody picks a theme from the
    // knob, where there is no install, no computer involved and no three steps: the Orb is
    // reading its own SD card and writing its own flash. Saying "update" there sent someone
    // looking for what their computer was doing. This wording is true in both cases.
    lv_label_set_text(s_title, "Preparing theme");
    char b[96];
    snprintf(b, sizeof(b), "Rebuilding from the SD card\n0 of %d", totalAssets);
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
    char b[128];
    snprintf(b, sizeof(b), "Rebuilding from the SD card\n%d of %d  %.24s",
             done, totalAssets, assetName ? assetName : "");
    lv_label_set_text(s_sub, b);
    lv_refr_now(NULL);
}

void bake_done() {
    // main.cpp shows the splash BEFORE calling this, so the repaint destroy() forces finds
    // the title card already covering the screen and never shows the Flight Tracker
    // underneath for a frame. Safe to call when no bake ran: destroy() does nothing then.
    const bool had = s_panel != nullptr;
    destroy();
#ifdef ARDUINO
    if (had) Serial.println("[update_ui] install finished — update overlay down, boot continues");
#endif
}

} // namespace update_ui
