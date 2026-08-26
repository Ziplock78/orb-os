#include "app_shell.h"
#include "display.h"   // markInput — click-to-pixels timing
#ifdef ARDUINO
#include <Arduino.h>
#else
#include <cstdio>
#include <chrono>
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } } Serial;
static uint32_t millis() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
#endif
#include "config.h"     // SCREEN_W / SCREEN_H
#include "diag_log.h"
#include "app_theme.h"
#include "theme_style.h"
#include "theme_font.h"   // per-theme fonts, with the compiled font as fallback     // menu colour/position for the no-canvas fallback
#include "custom_menu.h"     // CUSTOM_HAS_MENU — a Launch Kit push replaces this overlay's look
#include "menu_sprite.h"     // menu_custom_plate()/menu_custom_overlay() — the editor's baked background / CRT+glass
#include "menu_text.h"       // menu_text::refresh() — the editor's current/prev/next banners

namespace {
    struct App {
        lv_obj_t     *screen;
        const char   *name;
        app_action_t  onPress;
        app_turn_t    onTurn;
        bool          capture;
        app_action_t  onEnter;
        app_action_t  onExit;
        bool          hidden;    // registered but skipped when cycling the menu
    };

    constexpr int   MAX_APPS      = 8;
    constexpr uint32_t ANIM_MS    = 250;   // slide duration between apps

    App  s_apps[MAX_APPS];
    int  s_count    = 0;
    int  s_cur      = 0;
    bool s_captured = false;

    // app-switcher overlay (lives on the top layer, above whatever screen is loaded)
    bool      s_browsing      = false;
    // Which app the switcher is POINTING AT, as distinct from the one actually loaded.
    // These used to be the same: every detent called load(), which fires the outgoing
    // app's onExit and the incoming app's onEnter, so scrubbing the menu meant freeing
    // one app's decoded artwork and decoding the next straight off the SD card, per
    // step. That is a 350 KB PNG expanded into a 636 KB buffer for a screen you are
    // scrolling past. Nothing loads now until you commit.
    int       s_browseIdx     = 0;
    lv_obj_t *s_overlay       = nullptr;
    lv_obj_t *s_overlayLabel  = nullptr;   // stock fallback (no custom menu design): plain centered name
    lv_obj_t *s_overlayHint   = nullptr;   // "push to open" — stock only, a custom design speaks for itself
    lv_obj_t *s_overlayPlate  = nullptr;   // a custom menu's baked background image, if any
    lv_obj_t *s_overlayGlass  = nullptr;   // a custom menu's baked CRT+glass, if any
    uint32_t  s_browseTouch   = 0;         // millis() of the last browse interaction
    // How many detents move the menu on by one app.
    //
    // One. Two was tried on 2026-08-26 and rejected on the hardware within minutes: it
    // reads as the menu being reluctant rather than as it being careful, which is a worse
    // feeling than the occasional accidental step it was meant to prevent.
    //
    // Worth keeping the number rather than deleting it, because the request that produced
    // it was real (this knob is light, one detent is easy to produce by accident) and the
    // answer might be a heavier detent in hardware rather than a filter in software. It is
    // also no longer load-bearing: with the accumulator below, one-per-item finally means
    // one, where before this it meant "one, and some of your turns are discarded".
    constexpr int BROWSE_DETENTS_PER_ITEM = 1;

    // Detents seen but not yet spent. This is the whole fix for "I have to click two or three
    // times before it switches".
    //
    // takeDelta() returns NET movement since the last poll, so a quick turn arrives as delta=3
    // rather than as three separate calls. The old code read only the SIGN of that and moved
    // exactly one app, silently discarding the other two. Turning slowly gave every detent its
    // own poll and felt perfect; turning at any speed, or turning while a frame was slow, threw
    // input away. Same shape as the Rock bug in knob.cpp: the magnitude was there and the
    // consumer looked only at the direction.
    //
    // Accumulating instead means no detent is ever lost. It also makes the two-per-item rule
    // above free, rather than a second place where input gets dropped on purpose.
    static int s_browseAccum = 0;
    constexpr uint32_t BROWSE_SETTLE_MS = 2000;   // auto-enter the shown app after this idle

    int next_visible(int from, int dir);   // forward decl — defined below, needed by show_overlay above it
    void load(int idx, bool animate, bool forward);   // same, needed by commit_current()

    // The menu's background art is 466x466 and costs ~636 KB of PSRAM decoded, and the
    // glass layer another ~636 KB. Both used to be decoded once at boot and held for the
    // life of the device, for a screen that is visible for a couple of seconds at a time.
    // That is what left Flight Tracker unable to allocate its own static overlays
    // ("[radar_sprite] static1: buffer alloc failed"). Same rule as the text canvas now:
    // build on show, tear down on hide.
    // Free PSRAM, for the log lines below. The exact budget on this board has been
    // guesswork all evening; print it at each allocation so it stops being guesswork.
    static unsigned psram_free_kb() {
#ifdef ESP_PLATFORM
        return (unsigned)(ESP.getFreePsram() / 1024);
#else
        return 0;
#endif
    }

    void overlay_art_acquire() {
        Serial.printf("[menu] art acquire: %u KB PSRAM free before\n", psram_free_kb());
        if (!s_overlayPlate) {
            if (const lv_img_dsc_t *plate = menu_custom_plate()) {
                s_overlayPlate = lv_img_create(s_overlay);
                lv_img_set_src(s_overlayPlate, plate);
                lv_obj_center(s_overlayPlate);
                lv_obj_move_background(s_overlayPlate);   // behind the text canvas
            }
        }
        if (!s_overlayGlass) {
            if (const lv_img_dsc_t *ov = menu_custom_overlay()) {
                s_overlayGlass = lv_img_create(s_overlay);
                lv_img_set_src(s_overlayGlass, ov);
                lv_obj_center(s_overlayGlass);
            }
        }
        if (s_overlayGlass) lv_obj_move_foreground(s_overlayGlass);   // CRT/glass on top
        Serial.printf("[menu] art acquire: %u KB PSRAM free after (plate=%d glass=%d)\n",
                      psram_free_kb(), s_overlayPlate ? 1 : 0, s_overlayGlass ? 1 : 0);
    }

    void overlay_art_release() {
        if (s_overlayPlate) { lv_obj_del(s_overlayPlate); s_overlayPlate = nullptr; }
        if (s_overlayGlass) { lv_obj_del(s_overlayGlass); s_overlayGlass = nullptr; }
        menu_sprite_release();   // give the decoded PSRAM back, not just the LVGL objects
    }

    void show_overlay(const char *name) {
        if (!s_overlay) return;
#if CUSTOM_HAS_MENU
        // Order matters, and getting it wrong showed up as plain white menu text: the
        // background art wants ~1.3 MB (plate + glass) and the text canvas ~868 KB, and
        // they do not both fit. Text first, because a menu you cannot read is useless
        // while a menu without a backdrop is merely plain.
        menu_text::acquire();   // ~868 KB PSRAM, held only while the overlay is up
        overlay_art_acquire();
        // A custom design draws current/prev/next itself (menu_text canvas); the stock
        // label stays hidden while that canvas exists. If it could not be allocated,
        // fall through to the plain label: an overlay with no text on it is worse than
        // an unstyled one, because there is then no way to see which app you are on.
        if (menu_text::available()) {
            const char *prevName = s_count ? s_apps[next_visible(s_browseIdx, -1)].name : "";
            const char *nextName = s_count ? s_apps[next_visible(s_browseIdx, +1)].name : "";
            menu_text::refresh(prevName, name, nextName);
            if (s_overlayLabel) lv_obj_add_flag(s_overlayLabel, LV_OBJ_FLAG_HIDDEN);
        } else if (s_overlayLabel) {
            // No canvas, which for a custom design normally means "this theme uses no
            // glow" (menu_text::acquire skips the 651 KB buffer then). The canvas was
            // never only about glow though: it also carried the theme's font, colour and
            // position, so falling back to a stock white Montserrat label threw the whole
            // menu design away. Dress the plain label in the theme's own values instead —
            // the same fix settings_view's wheel_layout already carries for its own
            // no-canvas branch.
            const theme_style::MenuText &mc = theme_style::menu().current;
            lv_obj_set_style_text_color(s_overlayLabel, lv_color_hex(mc.color), 0);
            lv_obj_set_style_text_font(s_overlayLabel, theme_font::menu_current(), 0);
            lv_obj_set_style_text_align(s_overlayLabel, LV_TEXT_ALIGN_CENTER, 0);
            // Wrapping belongs here too, not only on the glow canvas: menu_text's
            // draw_wrapped never ran for a no-glow theme, so "Wrap at" appeared to do
            // nothing on exactly the themes that take this path.
            //
            // Deliberately NOT LVGL's own LV_LABEL_LONG_WRAP. LVGL breaks a word that is
            // wider than the label mid-word, and with a 71 px font under a narrow wrap
            // width that turns "Flight" into "Fli/ght". menu_text::wrap_text applies the
            // same never-split-a-word rule the canvas renderer and Studio's preview use,
            // so all three agree.
            char wrapped[160];
            menu_text::wrap_text(theme_font::menu_current(), name, mc.wrapWidth, wrapped, sizeof(wrapped));
            lv_label_set_text(s_overlayLabel, wrapped);
            lv_obj_set_width(s_overlayLabel, SCREEN_W);
            lv_obj_set_style_text_line_space(s_overlayLabel, mc.lineGap, 0);
            // Centre the whole label BOX on the design's point. The box grows with the
            // number of lines, so one-line and two-line names share an optical centre
            // without computing any line maths here — and unlike lv_obj_set_pos, an
            // offset from LV_ALIGN_CENTER is what this label's alignment actually means.
            lv_obj_align(s_overlayLabel, LV_ALIGN_CENTER, mc.x - SCREEN_W / 2, mc.y - SCREEN_H / 2);
            lv_obj_clear_flag(s_overlayLabel, LV_OBJ_FLAG_HIDDEN);
        }
#else
        lv_label_set_text(s_overlayLabel, name);
#endif
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        s_browseTouch = millis();          // any turn/open restarts the settle countdown
    }
    void hide_overlay() {
        if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
#if CUSTOM_HAS_MENU
        menu_text::release();     // give the canvas back the moment it is off screen
        overlay_art_release();    // and the background/glass art with it
#endif
        s_browsing = false;
    }

    void commit_current() {                // enter the app the overlay is showing
        hide_overlay();
        // Half a turn left over from browsing must not be waiting to move the menu the next
        // time it opens.
        s_browseAccum = 0;
        // The one place the switcher actually loads an app. load() handles the outgoing
        // onExit, the screen swap, capture state, and the incoming onEnter, so exactly
        // one app's artwork is decoded per selection rather than one per detent.
        load(s_browseIdx, false, true);
        diag::log("enter %s", s_apps[s_cur].name);
    }

    // Runs on the LVGL thread; if the switcher has sat idle long enough, drop into the shown app.
    void browse_tick(lv_timer_t * /*t*/) {
        if (s_browsing && (millis() - s_browseTouch) >= BROWSE_SETTLE_MS) commit_current();
    }

    // Next non-hidden app from `from` stepping by `dir` (+1/-1), wrapping around.
    // Falls back to `from` if somehow everything is hidden, so cycling never hangs.
    int next_visible(int from, int dir) {
        for (int step = 1; step <= s_count; ++step) {
            const int idx = ((from + dir * step) % s_count + s_count) % s_count;
            if (!s_apps[idx].hidden) return idx;
        }
        return from;
    }

    void load(int idx, bool animate, bool forward) {
        if (idx < 0 || idx >= s_count || !s_apps[idx].screen) return;
        // Tell the outgoing app it's leaving before we swap, so it can free whatever it
        // decoded. Both this and onEnter now fire ONLY on a real app change (boot, a
        // committed switcher selection, or next()/prev()), never per switcher detent.
        if (idx != s_cur && s_count && s_apps[s_cur].onExit) s_apps[s_cur].onExit();
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
        diag::log("app %s", s_apps[idx].name);
    }
}

void app_shell::add(lv_obj_t *screen, const char *name,
                    app_action_t onPress, app_turn_t onTurn, bool capture, app_action_t onEnter, app_action_t onExit, bool hidden) {
    if (s_count < MAX_APPS && screen) {
        s_apps[s_count].screen  = screen;
        s_apps[s_count].name    = name;
        s_apps[s_count].onPress = onPress;
        s_apps[s_count].onTurn  = onTurn;
        s_apps[s_count].capture = capture;
        s_apps[s_count].onEnter = onEnter;
        s_apps[s_count].onExit  = onExit;
        s_apps[s_count].hidden  = hidden;
        s_count++;
    }
}

void app_shell::add_active(const char *name,
                           app_action_t onPress, app_turn_t onTurn, bool capture, app_action_t onEnter, app_action_t onExit, bool hidden) {
    add(lv_scr_act(), name, onPress, onTurn, capture, onEnter, onExit, hidden);
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
    const AppPalette &pal = app_theme::palette();
    lv_obj_set_style_bg_color(s_overlay, pal.bg, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);   // solid fallback; a custom plate (below) paints over it
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // A Launch Kit menu push's baked background, if any — sits under everything
    // else. No plate (stock, or a design with a solid-color background) just
    // leaves the overlay's own bg_color above showing through.
    // Menu art is NOT decoded here any more: see overlay_art_acquire(). Decoding it at
    // boot meant holding ~1.3 MB of PSRAM permanently for a transient overlay.

    s_overlayLabel = lv_label_create(s_overlay);
    lv_obj_set_style_text_color(s_overlayLabel, pal.ink, 0);
    lv_obj_set_style_text_font(s_overlayLabel, &lv_font_montserrat_48, 0);
    lv_obj_align(s_overlayLabel, LV_ALIGN_CENTER, 0, -12);

    s_overlayHint = lv_label_create(s_overlay);
    lv_label_set_text(s_overlayHint, "push to open");
    lv_obj_set_style_text_color(s_overlayHint, pal.dim, 0);
    lv_obj_set_style_text_font(s_overlayHint, &lv_font_montserrat_16, 0);
    lv_obj_align(s_overlayHint, LV_ALIGN_CENTER, 0, 40);

#if CUSTOM_HAS_MENU
    // A custom design replaces the plain name+hint with its own current/prev/next
    // banners (menu_text, real glow) and speaks for itself — hide the stock label
    // and hint for the whole session rather than toggling them per-push.
    lv_obj_add_flag(s_overlayLabel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_overlayHint, LV_OBJ_FLAG_HIDDEN);
    menu_text::init(s_overlay);
#endif

    // CRT + glass on top of everything, same layer order as the clock/radar/splash
    // compositors (background -> content -> overlay).
    // Glass likewise built on show, not at boot (see overlay_art_acquire()).

    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(browse_tick, 200, nullptr);   // watches for the 3s browse settle

    if (s_count) load(0, false, true);
}

// First turn opens the switcher on the current app; further turns cycle apps.
void app_shell::browseTurn(int delta) {
    if (!s_count) return;
    if (!s_browsing) {
        s_browsing = true;
        s_browseIdx = s_cur;
        // NOT evicting the app underneath any more. That was a workaround for the menu
        // needing ~1.7 MB against ~592 KB free, and it made every menu open-and-close pay
        // two full screen decodes (~370 ms each) — the "background disappears and takes a
        // moment to come back" behaviour. The memory came from somewhere better instead:
        // three of the four overlays were 100% transparent and are no longer shipped at
        // all, freeing 636 KB per screen. The app keeps its artwork while you browse.
        s_browseAccum = 0;   // the turn that opened it does not also move it
        show_overlay(s_apps[s_browseIdx].name);
        return;
    }
    // Move the cursor only. No load(), so no onExit/onEnter, so no SD read and no PNG
    // decode: a detent is just a text redraw on the overlay.
    //
    // That claim was measured rather than assumed, after the menu was reported as sluggish:
    // "[shell] detent -> Intel took 1ms". So the redraw was never the problem. What made it
    // feel slow was this function discarding detents, which the accumulator now fixes.
    s_browseAccum += delta;
    int moved = 0;
    while (s_browseAccum >= BROWSE_DETENTS_PER_ITEM) {
        s_browseAccum -= BROWSE_DETENTS_PER_ITEM;
        s_browseIdx = next_visible(s_browseIdx, +1);   // skip hidden apps
        ++moved;
    }
    while (s_browseAccum <= -BROWSE_DETENTS_PER_ITEM) {
        s_browseAccum += BROWSE_DETENTS_PER_ITEM;
        s_browseIdx = next_visible(s_browseIdx, -1);
        ++moved;
    }
    if (!moved) return;    // a half turn: kept, not thrown away, and spent on the next one
    show_overlay(s_apps[s_browseIdx].name);
}

// Push commits the shown app (hides the overlay); if not browsing, it's an app action.
void app_shell::browsePress() {
    if (s_browsing) commit_current();       // push commits the shown app
    else            pressCurrent();         // otherwise it's the app's own action
}

bool app_shell::browsing() { return s_browsing; }

void app_shell::openSwitcher() {
    s_browseIdx = s_cur;
    if (!s_count) return;
    s_browsing = true;
    // A rock is a left detent and a right one. Those must not be left in the accumulator to
    // nudge the menu the moment it opens.
    s_browseAccum = 0;
    show_overlay(s_apps[s_cur].name);
}

void app_shell::next() {
    if (s_count) load(next_visible(s_cur, +1), true, true);
}

void app_shell::prev() {
    if (s_count) load(next_visible(s_cur, -1), true, false);
}

void app_shell::selectApp(int idx) {
    if (idx >= 0 && idx < s_count) load(idx, false, true);
}

int         app_shell::count() { return s_count; }
int         app_shell::index() { return s_cur; }
const char *app_shell::name()  { return s_count ? s_apps[s_cur].name : ""; }
const char *app_shell::nameAt(int idx)  { return (idx >= 0 && idx < s_count) ? s_apps[idx].name : ""; }
bool        app_shell::hiddenAt(int idx){ return (idx >= 0 && idx < s_count) ? s_apps[idx].hidden : true; }
