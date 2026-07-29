#include "settings_view.h"
#include "app_shell.h"
#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include "config.h"     // SCREEN_W / SCREEN_H

// Shared with main.cpp.
extern int  host_get_brightness();
extern void host_set_brightness(int v, bool save);
extern void host_set_location(double lat, double lon);              // saves + reboots
extern void host_set_location_named(const char *name, double lat, double lon);  // + records in recents
extern void host_locate_current();                                 // IP-locate + set + reboot
extern int  host_geocode(const char *query, char names[][40], double *lats, double *lons, int maxN);
extern int  host_recents_get(char names[][40], double *lats, double *lons, int maxN);
extern void host_recents_add(const char *name, double lat, double lon);
extern int  host_get_volume();
extern void host_set_volume(int v, bool save);
extern bool host_sound_radar();
extern void host_sound_set_radar(bool on);
extern bool host_sound_chime();
extern void host_sound_set_chime(bool on);
extern void host_sound_preview_chime();
extern void host_sound_preview_beep();

namespace {
    // MODE_LOCATION is a 4-item menu (current / search / recent / back); MODE_RECENT is
    // the scrollable list of recent cities you reach from that menu.
    enum Mode { MODE_MENU, MODE_BRIGHT, MODE_LOCATION, MODE_RECENT, MODE_SEARCH, MODE_SOUND, MODE_VOLUME };

    // --- main settings menu ---
    enum { ITEM_BRIGHTNESS = 0, ITEM_LOCATION, ITEM_SOUND, ITEM_BACK, ITEM_COUNT };
    const char *ITEM_LABELS[ITEM_COUNT] = { "Brightness", "Location", "Sound", "Back" };

    // --- sound submenu ---
    enum { SND_RADAR = 0, SND_CHIME, SND_VOLUME, SND_BACK, SND_COUNT };

    constexpr int VOL_STEP = 10;

    // --- location submenu ---
    enum { LM_CURRENT = 0, LM_SEARCH, LM_RECENT, LM_BACK, LM_COUNT };
    const char *LM_LABELS[LM_COUNT] = { "Current location", "Search city", "Recent cities", "Back" };

    // Seed the recents list once (first boot) so "Recent cities" isn't empty.
    struct City { const char *name; double lat; double lon; };
    const City SEED_CITIES[] = {
        {"Phoenix, AZ",    33.4484, -112.0740},
        {"Ithaca, NY",     42.4406,  -76.4966},
        {"Pueblo, CO",     38.2544, -104.6091},
        {"Denver, CO",     39.7400, -104.9900},
        {"Dallas, TX",     32.7831,  -96.8067},
        {"Chicago, IL",    41.8500,  -87.6500},
        {"New York, NY",   40.7100,  -74.0100},
        {"Los Angeles",    34.0522, -118.2437},
    };
    const int SEED_COUNT = (int)(sizeof(SEED_CITIES) / sizeof(SEED_CITIES[0]));

    constexpr int RECENTS_MAX = 8;

    // Search keyboard: 26 letters + '<' (backspace) + '_' (space)
    const char KEYS[]   = "ABCDEFGHIJKLMNOPQRSTUVWXYZ<_";
    const int  N_KEYS   = 28;

    constexpr int BRI_MIN = 8, BRI_MAX = 255, BRI_STEP = 13;

    Mode s_mode  = MODE_MENU;
    int  s_sel   = 0;          // main-menu selection
    int  s_bri   = 200;
    int  s_lmSel = 0;          // location-menu selection
    int  s_sndSel = 0;         // sound-menu selection
    int  s_vol   = 60;         // volume working value

    // recent cities
    char   s_recNames[RECENTS_MAX][40];
    double s_recLat[RECENTS_MAX], s_recLon[RECENTS_MAX];
    int    s_recCount = 0;
    int    s_recSel   = 0;     // 0..s_recCount-1 = a city, s_recCount = Back

    // search state
    char   s_str[28]   = "";
    int    s_kbIdx     = 0;
    char   s_sugName[4][40];
    double s_sugLat[4], s_sugLon[4];
    int    s_sugCount  = 0;
    bool   s_pending   = false;
    int    s_countdown = 0;
    bool   s_searching = false;

    lv_obj_t *s_screen  = nullptr;
    lv_obj_t *s_menu    = nullptr;
    lv_obj_t *s_hl      = nullptr;
    lv_obj_t *s_items[ITEM_COUNT] = { nullptr };
    lv_obj_t *s_hint    = nullptr;
    lv_obj_t *s_bright  = nullptr;
    lv_obj_t *s_barFill = nullptr;
    lv_obj_t *s_pct     = nullptr;
    lv_obj_t *s_lmPage  = nullptr;   // location menu
    lv_obj_t *s_lmHl    = nullptr;
    lv_obj_t *s_lmItems[LM_COUNT] = { nullptr };
    lv_obj_t *s_recPage = nullptr;   // recent cities scroller
    lv_obj_t *s_recName = nullptr;
    lv_obj_t *s_recCoord= nullptr;
    lv_obj_t *s_srchPage= nullptr;
    lv_obj_t *s_srchText= nullptr;
    lv_obj_t *s_strip[7]= { nullptr };
    lv_obj_t *s_sug[4]  = { nullptr };
    lv_obj_t *s_sndPage = nullptr;   // sound menu
    lv_obj_t *s_sndHl   = nullptr;
    lv_obj_t *s_sndItems[SND_COUNT] = { nullptr };
    lv_obj_t *s_volPage = nullptr;   // volume adjuster
    lv_obj_t *s_volFill = nullptr;
    lv_obj_t *s_volPct  = nullptr;

    const lv_color_t C_WHITE = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);
    const lv_color_t C_GREY  = LV_COLOR_MAKE(0x6A, 0x70, 0x78);
    const lv_color_t C_DIM   = LV_COLOR_MAKE(0x9A, 0xA0, 0xA6);
    const lv_color_t C_ACCENT= LV_COLOR_MAKE(0x4F, 0xC3, 0xF7);

    void refresh_menu() {
        for (int i = 0; i < ITEM_COUNT; ++i)
            lv_obj_set_style_text_color(s_items[i], i == s_sel ? C_WHITE : C_GREY, 0);
        lv_obj_align(s_hl, LV_ALIGN_CENTER, 0, -72 + s_sel * 48);
    }

    void refresh_bright() {
        int pct = (int)lroundf((s_bri - BRI_MIN) * 100.0f / (BRI_MAX - BRI_MIN));
        lv_obj_set_width(s_barFill, (lv_coord_t)(4 + pct * (236 - 4) / 100));
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", pct);
        lv_label_set_text(s_pct, buf);
    }

    void refresh_sound() {
        char b[28];
        snprintf(b, sizeof(b), "Radar sounds   %s", host_sound_radar() ? "ON" : "OFF");
        lv_label_set_text(s_sndItems[SND_RADAR], b);
        snprintf(b, sizeof(b), "Clock chime   %s", host_sound_chime() ? "ON" : "OFF");
        lv_label_set_text(s_sndItems[SND_CHIME], b);
        snprintf(b, sizeof(b), "Volume   %d%%", host_get_volume());
        lv_label_set_text(s_sndItems[SND_VOLUME], b);
        lv_label_set_text(s_sndItems[SND_BACK], "Back");
        for (int i = 0; i < SND_COUNT; ++i)
            lv_obj_set_style_text_color(s_sndItems[i], i == s_sndSel ? C_WHITE : C_GREY, 0);
        lv_obj_align(s_sndHl, LV_ALIGN_CENTER, 0, -72 + s_sndSel * 48);
    }

    void refresh_vol() {
        lv_obj_set_width(s_volFill, (lv_coord_t)(4 + s_vol * (236 - 4) / 100));
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", s_vol);
        lv_label_set_text(s_volPct, buf);
    }

    void refresh_locmenu() {
        for (int i = 0; i < LM_COUNT; ++i)
            lv_obj_set_style_text_color(s_lmItems[i], i == s_lmSel ? C_WHITE : C_GREY, 0);
        lv_obj_align(s_lmHl, LV_ALIGN_CENTER, 0, -72 + s_lmSel * 48);
    }

    void refresh_recent() {
        if (s_recCount == 0) {
            lv_label_set_text(s_recName, "No recent cities");
            lv_label_set_text(s_recCoord, "search to add one");
        } else if (s_recSel < s_recCount) {
            lv_label_set_text(s_recName, s_recNames[s_recSel]);
            char b[32]; snprintf(b, sizeof(b), "%.2f, %.2f", s_recLat[s_recSel], s_recLon[s_recSel]);
            lv_label_set_text(s_recCoord, b);
        } else {
            lv_label_set_text(s_recName, "Back");
            lv_label_set_text(s_recCoord, "");
        }
    }

    void load_recents() {
        s_recCount = host_recents_get(s_recNames, s_recLat, s_recLon, RECENTS_MAX);
        s_recSel   = 0;
    }

    void refresh_search() {
        lv_label_set_text(s_srchText, s_str[0] ? s_str : "type a city name");
        const bool inKeys = (s_kbIdx < N_KEYS);
        const int  center = inKeys ? s_kbIdx : N_KEYS - 1;
        for (int k = 0; k < 7; ++k) {
            const int idx = center - 3 + k;
            char c[2] = { 0, 0 };
            if (idx >= 0 && idx < N_KEYS) c[0] = KEYS[idx];
            lv_label_set_text(s_strip[k], c);
            const bool hot = inKeys && (k == 3);
            lv_obj_set_style_text_color(s_strip[k], hot ? C_WHITE : C_GREY, 0);
            lv_obj_set_style_text_font(s_strip[k], hot ? &lv_font_montserrat_28 : &lv_font_montserrat_20, 0);
        }
        if (s_searching) {
            lv_label_set_text(s_sug[0], "searching...");
            lv_obj_set_style_text_color(s_sug[0], C_GREY, 0);
            for (int j = 1; j < 4; ++j) lv_label_set_text(s_sug[j], "");
        } else {
            for (int j = 0; j < 4; ++j) {
                if (j < s_sugCount) {
                    lv_label_set_text(s_sug[j], s_sugName[j]);
                    const bool hot = !inKeys && (s_kbIdx - N_KEYS == j);
                    lv_obj_set_style_text_color(s_sug[j], hot ? C_ACCENT : C_DIM, 0);
                } else lv_label_set_text(s_sug[j], "");
            }
        }
    }

    void show_page(Mode m) {
        s_mode = m;
        lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bright, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lmPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_recPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_srchPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sndPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_volPage, LV_OBJ_FLAG_HIDDEN);
        if (m == MODE_MENU)          { lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_HIDDEN);    refresh_menu(); }
        else if (m == MODE_BRIGHT)   { lv_obj_clear_flag(s_bright, LV_OBJ_FLAG_HIDDEN);  refresh_bright(); }
        else if (m == MODE_LOCATION) { lv_obj_clear_flag(s_lmPage, LV_OBJ_FLAG_HIDDEN);  refresh_locmenu(); }
        else if (m == MODE_RECENT)   { lv_obj_clear_flag(s_recPage, LV_OBJ_FLAG_HIDDEN); refresh_recent(); }
        else if (m == MODE_SOUND)    { lv_obj_clear_flag(s_sndPage, LV_OBJ_FLAG_HIDDEN); refresh_sound(); }
        else if (m == MODE_VOLUME)   { lv_obj_clear_flag(s_volPage, LV_OBJ_FLAG_HIDDEN); refresh_vol(); }
        else                         { lv_obj_clear_flag(s_srchPage, LV_OBJ_FLAG_HIDDEN); refresh_search(); }
    }

    void mark_dirty() { s_pending = true; s_countdown = 3; refresh_search(); }   // ~600ms debounce

    void search_tick(lv_timer_t * /*t*/) {
        if (s_mode != MODE_SEARCH) return;
        if (s_searching) {                        // "searching" is already painted; do the blocking fetch now
            s_searching = false;
            s_sugCount = host_geocode(s_str, s_sugName, s_sugLat, s_sugLon, 4);
            const int total = N_KEYS + s_sugCount;
            if (s_kbIdx >= total) s_kbIdx = total - 1;
            refresh_search();
            return;
        }
        if (!s_pending) return;
        if (--s_countdown > 0) return;
        s_pending = false;
        if (strlen(s_str) < 2) { s_sugCount = 0; refresh_search(); return; }
        s_searching = true; refresh_search();     // paints "searching"; fetch fires next tick
    }
}

void settingsview::onTurn(int delta) {
    const int step = (delta > 0) ? 1 : -1;
    if (s_mode == MODE_MENU) {
        s_sel = (s_sel + step < 0) ? 0 : (s_sel + step >= ITEM_COUNT ? ITEM_COUNT - 1 : s_sel + step);
        refresh_menu();
    } else if (s_mode == MODE_BRIGHT) {
        s_bri += delta * BRI_STEP;
        if (s_bri < BRI_MIN) s_bri = BRI_MIN;
        if (s_bri > BRI_MAX) s_bri = BRI_MAX;
        host_set_brightness(s_bri, false);
        refresh_bright();
    } else if (s_mode == MODE_LOCATION) {
        s_lmSel += step;
        if (s_lmSel < 0) s_lmSel = 0;
        if (s_lmSel >= LM_COUNT) s_lmSel = LM_COUNT - 1;
        refresh_locmenu();
    } else if (s_mode == MODE_RECENT) {
        s_recSel += step;
        if (s_recSel < 0) s_recSel = 0;
        if (s_recSel > s_recCount) s_recSel = s_recCount;   // last stop is Back
        refresh_recent();
    } else if (s_mode == MODE_SOUND) {
        s_sndSel += step;
        if (s_sndSel < 0) s_sndSel = 0;
        if (s_sndSel >= SND_COUNT) s_sndSel = SND_COUNT - 1;
        refresh_sound();
    } else if (s_mode == MODE_VOLUME) {
        s_vol += delta * VOL_STEP;
        if (s_vol < 0) s_vol = 0;
        if (s_vol > 100) s_vol = 100;
        host_set_volume(s_vol, false);      // live preview level
        refresh_vol();
    } else {  // MODE_SEARCH
        const int total = N_KEYS + s_sugCount;
        s_kbIdx += step;
        if (s_kbIdx < 0) s_kbIdx = 0;
        if (s_kbIdx >= total) s_kbIdx = total - 1;
        refresh_search();
    }
}

// Called by the app shell when Settings becomes the active app (fresh entry from
// the switcher). Reset to the top menu; the shell has already captured the knob.
void settingsview::onEnter() {
    s_sel = 0;
    show_page(MODE_MENU);
    if (s_hint) lv_label_set_text(s_hint, "push to select");
}

void settingsview::onPress() {
    if (s_mode == MODE_MENU) {
        if (s_sel == ITEM_BRIGHTNESS) { s_bri = host_get_brightness(); show_page(MODE_BRIGHT); }
        else if (s_sel == ITEM_LOCATION) { s_lmSel = 0; show_page(MODE_LOCATION); }
        else if (s_sel == ITEM_SOUND) { s_sndSel = 0; show_page(MODE_SOUND); }
        else {                                          // Back -> return to the app switcher
            app_shell::setCaptured(false);
            app_shell::openSwitcher();
        }
    } else if (s_mode == MODE_BRIGHT) {
        host_set_brightness(s_bri, true);
        show_page(MODE_MENU);
    } else if (s_mode == MODE_SOUND) {
        if (s_sndSel == SND_RADAR) {
            const bool on = !host_sound_radar();
            host_sound_set_radar(on);
            refresh_sound();
            if (on) host_sound_preview_beep();
        } else if (s_sndSel == SND_CHIME) {
            const bool on = !host_sound_chime();
            host_sound_set_chime(on);
            refresh_sound();
            if (on) host_sound_preview_chime();
        } else if (s_sndSel == SND_VOLUME) {
            s_vol = host_get_volume();
            show_page(MODE_VOLUME);
        } else {                                        // Back -> main settings menu
            s_sel = ITEM_SOUND;
            show_page(MODE_MENU);
        }
    } else if (s_mode == MODE_VOLUME) {
        host_set_volume(s_vol, true);
        host_sound_preview_beep();                      // hear the new level
        show_page(MODE_SOUND);
    } else if (s_mode == MODE_LOCATION) {
        if (s_lmSel == LM_CURRENT) {
            lv_label_set_text(s_lmItems[LM_CURRENT], "Locating...");
            lv_refr_now(NULL);
            host_locate_current();                      // reboots on success
            lv_label_set_text(s_lmItems[LM_CURRENT], LM_LABELS[LM_CURRENT]);  // came back = failed
        } else if (s_lmSel == LM_SEARCH) {
            s_str[0] = 0; s_kbIdx = 0; s_sugCount = 0; s_pending = false; s_searching = false;
            show_page(MODE_SEARCH);
        } else if (s_lmSel == LM_RECENT) {
            load_recents();
            show_page(MODE_RECENT);
        } else {                                        // Back -> main settings menu
            s_sel = ITEM_LOCATION;
            show_page(MODE_MENU);
        }
    } else if (s_mode == MODE_RECENT) {
        if (s_recCount > 0 && s_recSel < s_recCount)
            host_set_location_named(s_recNames[s_recSel], s_recLat[s_recSel], s_recLon[s_recSel]);
        else
            show_page(MODE_LOCATION);                   // Back (or empty list)
    } else {  // MODE_SEARCH
        const int L = (int)strlen(s_str);
        if (s_kbIdx < 26) {                                 // a letter
            if (L < (int)sizeof(s_str) - 1) { s_str[L] = KEYS[s_kbIdx]; s_str[L + 1] = 0; }
            mark_dirty();
        } else if (s_kbIdx == 26) {                         // backspace (empty -> exit search)
            if (L > 0) { s_str[L - 1] = 0; mark_dirty(); }
            else show_page(MODE_LOCATION);
        } else if (s_kbIdx == 27) {                         // space
            if (L > 0 && L < (int)sizeof(s_str) - 1) { s_str[L] = ' '; s_str[L + 1] = 0; }
            mark_dirty();
        } else {                                            // a suggestion
            const int j = s_kbIdx - N_KEYS;
            if (j >= 0 && j < s_sugCount) host_set_location_named(s_sugName[j], s_sugLat[j], s_sugLon[j]);
        }
    }
}

void settingsview::init() {
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_set_style_text_color(title, C_DIM, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -150);

    // --- main menu page ---
    s_menu = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_size(s_menu, SCREEN_W, SCREEN_H); lv_obj_center(s_menu);
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_SCROLLABLE);

    s_hl = lv_obj_create(s_menu);
    lv_obj_remove_style_all(s_hl);
    lv_obj_set_size(s_hl, 240, 44);
    lv_obj_set_style_radius(s_hl, 10, 0);
    lv_obj_set_style_bg_color(s_hl, lv_color_hex(0x232A36), 0);
    lv_obj_set_style_bg_opa(s_hl, LV_OPA_COVER, 0);

    for (int i = 0; i < ITEM_COUNT; ++i) {
        s_items[i] = lv_label_create(s_menu);
        lv_label_set_text(s_items[i], ITEM_LABELS[i]);
        lv_obj_set_style_text_font(s_items[i], &lv_font_montserrat_20, 0);
        lv_obj_align(s_items[i], LV_ALIGN_CENTER, 0, -72 + i * 48);
    }
    s_hint = lv_label_create(s_menu);
    lv_label_set_text(s_hint, "push to select");
    lv_obj_set_style_text_color(s_hint, C_GREY, 0);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 150);

    // --- brightness page ---
    s_bright = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_bright);
    lv_obj_set_size(s_bright, SCREEN_W, SCREEN_H); lv_obj_center(s_bright);
    lv_obj_clear_flag(s_bright, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *blabel = lv_label_create(s_bright);
    lv_label_set_text(blabel, "Brightness");
    lv_obj_set_style_text_color(blabel, C_WHITE, 0);
    lv_obj_set_style_text_font(blabel, &lv_font_montserrat_20, 0);
    lv_obj_align(blabel, LV_ALIGN_CENTER, 0, -70);
    lv_obj_t *track = lv_obj_create(s_bright);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, 240, 18);
    lv_obj_set_style_radius(track, 9, 0);
    lv_obj_set_style_bg_color(track, lv_color_hex(0x2A2E33), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(track, LV_ALIGN_CENTER, 0, 0);
    s_barFill = lv_obj_create(track);
    lv_obj_remove_style_all(s_barFill);
    lv_obj_set_size(s_barFill, 120, 14);
    lv_obj_set_style_radius(s_barFill, 7, 0);
    lv_obj_set_style_bg_color(s_barFill, C_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_barFill, LV_OPA_COVER, 0);
    lv_obj_align(s_barFill, LV_ALIGN_LEFT_MID, 2, 0);
    s_pct = lv_label_create(s_bright);
    lv_label_set_text(s_pct, "--%");
    lv_obj_set_style_text_color(s_pct, C_WHITE, 0);
    lv_obj_set_style_text_font(s_pct, &lv_font_montserrat_20, 0);
    lv_obj_align(s_pct, LV_ALIGN_CENTER, 0, 50);
    lv_obj_t *bhint = lv_label_create(s_bright);
    lv_label_set_text(bhint, "turn to adjust, push to save");
    lv_obj_set_style_text_color(bhint, C_GREY, 0);
    lv_obj_set_style_text_font(bhint, &lv_font_montserrat_14, 0);
    lv_obj_align(bhint, LV_ALIGN_CENTER, 0, 110);

    // --- location menu page (Current / Search / Recent / Back) ---
    s_lmPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_lmPage);
    lv_obj_set_size(s_lmPage, SCREEN_W, SCREEN_H); lv_obj_center(s_lmPage);
    lv_obj_clear_flag(s_lmPage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lmtitle = lv_label_create(s_lmPage);
    lv_label_set_text(lmtitle, "Location");
    lv_obj_set_style_text_color(lmtitle, C_DIM, 0);
    lv_obj_set_style_text_font(lmtitle, &lv_font_montserrat_16, 0);
    lv_obj_align(lmtitle, LV_ALIGN_CENTER, 0, -122);
    s_lmHl = lv_obj_create(s_lmPage);
    lv_obj_remove_style_all(s_lmHl);
    lv_obj_set_size(s_lmHl, 300, 44);
    lv_obj_set_style_radius(s_lmHl, 10, 0);
    lv_obj_set_style_bg_color(s_lmHl, lv_color_hex(0x232A36), 0);
    lv_obj_set_style_bg_opa(s_lmHl, LV_OPA_COVER, 0);
    for (int i = 0; i < LM_COUNT; ++i) {
        s_lmItems[i] = lv_label_create(s_lmPage);
        lv_label_set_text(s_lmItems[i], LM_LABELS[i]);
        lv_obj_set_style_text_font(s_lmItems[i], &lv_font_montserrat_20, 0);
        lv_obj_align(s_lmItems[i], LV_ALIGN_CENTER, 0, -72 + i * 48);
    }
    lv_obj_t *lmhint = lv_label_create(s_lmPage);
    lv_label_set_text(lmhint, "turn to choose, push to select");
    lv_obj_set_style_text_color(lmhint, C_GREY, 0);
    lv_obj_set_style_text_font(lmhint, &lv_font_montserrat_14, 0);
    lv_obj_align(lmhint, LV_ALIGN_CENTER, 0, 150);

    // --- recent cities page (single-item scroller) ---
    s_recPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_recPage);
    lv_obj_set_size(s_recPage, SCREEN_W, SCREEN_H); lv_obj_center(s_recPage);
    lv_obj_clear_flag(s_recPage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *rtitle = lv_label_create(s_recPage);
    lv_label_set_text(rtitle, "Recent cities");
    lv_obj_set_style_text_color(rtitle, C_DIM, 0);
    lv_obj_set_style_text_font(rtitle, &lv_font_montserrat_16, 0);
    lv_obj_align(rtitle, LV_ALIGN_CENTER, 0, -70);
    s_recName = lv_label_create(s_recPage);
    lv_label_set_text(s_recName, "");
    lv_obj_set_style_text_color(s_recName, C_WHITE, 0);
    lv_obj_set_style_text_font(s_recName, &lv_font_montserrat_20, 0);
    lv_obj_align(s_recName, LV_ALIGN_CENTER, 0, -14);
    s_recCoord = lv_label_create(s_recPage);
    lv_label_set_text(s_recCoord, "");
    lv_obj_set_style_text_color(s_recCoord, C_GREY, 0);
    lv_obj_set_style_text_font(s_recCoord, &lv_font_montserrat_14, 0);
    lv_obj_align(s_recCoord, LV_ALIGN_CENTER, 0, 20);
    lv_obj_t *rhint = lv_label_create(s_recPage);
    lv_label_set_text(rhint, "turn to choose, push to set");
    lv_obj_set_style_text_color(rhint, C_GREY, 0);
    lv_obj_set_style_text_font(rhint, &lv_font_montserrat_14, 0);
    lv_obj_align(rhint, LV_ALIGN_CENTER, 0, 110);

    // --- search page ---
    s_srchPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_srchPage);
    lv_obj_set_size(s_srchPage, SCREEN_W, SCREEN_H); lv_obj_center(s_srchPage);
    lv_obj_clear_flag(s_srchPage, LV_OBJ_FLAG_SCROLLABLE);
    s_srchText = lv_label_create(s_srchPage);
    lv_label_set_text(s_srchText, "type a city name");
    lv_obj_set_style_text_color(s_srchText, C_WHITE, 0);
    lv_obj_set_style_text_font(s_srchText, &lv_font_montserrat_20, 0);
    lv_obj_align(s_srchText, LV_ALIGN_CENTER, 0, -135);
    for (int k = 0; k < 7; ++k) {
        s_strip[k] = lv_label_create(s_srchPage);
        lv_label_set_text(s_strip[k], "");
        lv_obj_set_style_text_color(s_strip[k], C_GREY, 0);
        lv_obj_set_style_text_font(s_strip[k], &lv_font_montserrat_20, 0);
        lv_obj_align(s_strip[k], LV_ALIGN_CENTER, (k - 3) * 48, -75);
    }
    for (int j = 0; j < 4; ++j) {
        s_sug[j] = lv_label_create(s_srchPage);
        lv_label_set_text(s_sug[j], "");
        lv_obj_set_style_text_color(s_sug[j], C_DIM, 0);
        lv_obj_set_style_text_font(s_sug[j], &lv_font_montserrat_16, 0);
        lv_obj_align(s_sug[j], LV_ALIGN_CENTER, 0, -5 + j * 34);
    }
    lv_obj_t *shint = lv_label_create(s_srchPage);
    lv_label_set_text(shint, "turn to letters then cities");
    lv_obj_set_style_text_color(shint, C_GREY, 0);
    lv_obj_set_style_text_font(shint, &lv_font_montserrat_14, 0);
    lv_obj_align(shint, LV_ALIGN_CENTER, 0, 150);

    // --- sound menu page (Radar / Chime / Volume / Back) ---
    s_sndPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_sndPage);
    lv_obj_set_size(s_sndPage, SCREEN_W, SCREEN_H); lv_obj_center(s_sndPage);
    lv_obj_clear_flag(s_sndPage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sndtitle = lv_label_create(s_sndPage);
    lv_label_set_text(sndtitle, "Sound");
    lv_obj_set_style_text_color(sndtitle, C_DIM, 0);
    lv_obj_set_style_text_font(sndtitle, &lv_font_montserrat_16, 0);
    lv_obj_align(sndtitle, LV_ALIGN_CENTER, 0, -122);
    s_sndHl = lv_obj_create(s_sndPage);
    lv_obj_remove_style_all(s_sndHl);
    lv_obj_set_size(s_sndHl, 300, 44);
    lv_obj_set_style_radius(s_sndHl, 10, 0);
    lv_obj_set_style_bg_color(s_sndHl, lv_color_hex(0x232A36), 0);
    lv_obj_set_style_bg_opa(s_sndHl, LV_OPA_COVER, 0);
    for (int i = 0; i < SND_COUNT; ++i) {
        s_sndItems[i] = lv_label_create(s_sndPage);
        lv_label_set_text(s_sndItems[i], "");
        lv_obj_set_style_text_font(s_sndItems[i], &lv_font_montserrat_20, 0);
        lv_obj_align(s_sndItems[i], LV_ALIGN_CENTER, 0, -72 + i * 48);
    }
    lv_obj_t *sndhint = lv_label_create(s_sndPage);
    lv_label_set_text(sndhint, "turn to choose, push to toggle");
    lv_obj_set_style_text_color(sndhint, C_GREY, 0);
    lv_obj_set_style_text_font(sndhint, &lv_font_montserrat_14, 0);
    lv_obj_align(sndhint, LV_ALIGN_CENTER, 0, 150);

    // --- volume page ---
    s_volPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_volPage);
    lv_obj_set_size(s_volPage, SCREEN_W, SCREEN_H); lv_obj_center(s_volPage);
    lv_obj_clear_flag(s_volPage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *vlabel = lv_label_create(s_volPage);
    lv_label_set_text(vlabel, "Volume");
    lv_obj_set_style_text_color(vlabel, C_WHITE, 0);
    lv_obj_set_style_text_font(vlabel, &lv_font_montserrat_20, 0);
    lv_obj_align(vlabel, LV_ALIGN_CENTER, 0, -70);
    lv_obj_t *vtrack = lv_obj_create(s_volPage);
    lv_obj_remove_style_all(vtrack);
    lv_obj_set_size(vtrack, 240, 18);
    lv_obj_set_style_radius(vtrack, 9, 0);
    lv_obj_set_style_bg_color(vtrack, lv_color_hex(0x2A2E33), 0);
    lv_obj_set_style_bg_opa(vtrack, LV_OPA_COVER, 0);
    lv_obj_clear_flag(vtrack, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(vtrack, LV_ALIGN_CENTER, 0, 0);
    s_volFill = lv_obj_create(vtrack);
    lv_obj_remove_style_all(s_volFill);
    lv_obj_set_size(s_volFill, 120, 14);
    lv_obj_set_style_radius(s_volFill, 7, 0);
    lv_obj_set_style_bg_color(s_volFill, C_ACCENT, 0);
    lv_obj_set_style_bg_opa(s_volFill, LV_OPA_COVER, 0);
    lv_obj_align(s_volFill, LV_ALIGN_LEFT_MID, 2, 0);
    s_volPct = lv_label_create(s_volPage);
    lv_label_set_text(s_volPct, "--%");
    lv_obj_set_style_text_color(s_volPct, C_WHITE, 0);
    lv_obj_set_style_text_font(s_volPct, &lv_font_montserrat_20, 0);
    lv_obj_align(s_volPct, LV_ALIGN_CENTER, 0, 50);
    lv_obj_t *vhint = lv_label_create(s_volPage);
    lv_label_set_text(vhint, "turn to adjust, push to test");
    lv_obj_set_style_text_color(vhint, C_GREY, 0);
    lv_obj_set_style_text_font(vhint, &lv_font_montserrat_14, 0);
    lv_obj_align(vhint, LV_ALIGN_CENTER, 0, 110);

    // First boot: seed the recents list so "Recent cities" starts populated.
    {
        char   tn[RECENTS_MAX][40];
        double tla[RECENTS_MAX], tlo[RECENTS_MAX];
        if (host_recents_get(tn, tla, tlo, RECENTS_MAX) == 0)
            for (int i = SEED_COUNT - 1; i >= 0; --i)      // reversed so SEED_CITIES[0] lands on top
                host_recents_add(SEED_CITIES[i].name, SEED_CITIES[i].lat, SEED_CITIES[i].lon);
    }

    s_bri = host_get_brightness();
    show_page(MODE_MENU);
    lv_timer_create(search_tick, 200, nullptr);
}

lv_obj_t *settingsview::screen() { return s_screen; }
