#include "settings_view.h"
#include "app_shell.h"
#include "app_theme.h"
#include "theme_select.h"   // which Launch Kit design (of however many are installed on the SD card) is active
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include "config.h"     // SCREEN_W / SCREEN_H
#include "splash_art.h" // splash_art_decode() — the boot splash, reused for the About page
#include "splash_lines.h" // the three standing lines and the glass over them
#include "diag_log.h"
#include "custom_settings.h"    // CUSTOM_HAS_SETTINGS (compile-time show/hide gate) + compiled fallback defaults — see theme_style.h
#include "settings_sprite.h"    // settings_custom_plate()/settings_custom_overlay() — the editor's baked background / CRT+glass
#include "settings_text.h"      // settings_text::draw_item() — the wheel's glow-capable text, shared by every list
#include "theme_style.h"
#include "theme_font.h"   // per-theme fonts, with the compiled font as fallback        // per-theme wheel geometry/colors/highlight/default-selection — the runtime half of custom_settings.h's macros

// Shared with main.cpp.
extern int  host_get_brightness();
extern void host_set_brightness(int v, bool save);
extern uint32_t host_get_idle_ms();
extern void     host_set_idle_ms(uint32_t ms);
extern void host_set_location(double lat, double lon);              // saves + reboots
extern void host_set_location_named(const char *name, double lat, double lon);  // + records in recents
extern bool host_locate_current();                                 // IP-locate + set + reboot; false = failed, didn't reboot
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
extern int  host_chime_count();
extern const char *host_chime_name(int idx);
extern int  host_chime_index();
extern void host_chime_set(int idx);
extern void host_chime_preview(int idx);
extern void host_wifi_scan_start();
extern int  host_wifi_scan_result(char names[][33], int8_t *rssi, bool *isOpen, int maxN);
extern void host_wifi_connect(const char *ssid, const char *pass);
extern int  host_wifi_connect_status();
extern void host_wifi_connected_reboot();
extern void host_factory_reset();          // wipes WiFi + all saved settings, reboots
extern bool host_wx_is_imperial();         // resolved (mode + Auto-detected location) weather units
extern int  host_wx_units_mode();          // 0=Auto 1=Metric 2=Imperial (raw saved mode)
extern void host_wx_units_set(int mode);
// Flight Tracker display range. Lives here because touch was removed: this used to be
// the on-screen zoom button in ui.cpp, which was the only way to change range and died
// with the touchscreen. host_set_range_km() persists to NVS and re-renders, exactly as
// that button's callback did.
extern float host_get_range_km();
extern void  host_set_range_km(float km);

namespace {
    // MODE_LOCATION is a 4-item menu (current / search / recent / back); MODE_RECENT is
    // the scrollable list of recent cities you reach from that menu.
    enum Mode { MODE_MENU, MODE_DISPLAY, MODE_BRIGHT, MODE_LOCATION, MODE_RECENT, MODE_SEARCH, MODE_SOUND, MODE_VOLUME, MODE_ABOUT,
                MODE_WIFI_LIST, MODE_WIFI_PASSWORD, MODE_WIFI_STATUS, MODE_RESET_CONFIRM, MODE_UNITS, MODE_CHIME_SELECT,
                MODE_THEME_SELECT, MODE_THEME_NOTICE, MODE_DESIGN_SELECT, MODE_DESIGN_NOTICE, MODE_RANGE,
                MODE_FIRSTBOOT, MODE_FIRSTBOOT_PHONE, MODE_NO_SDCARD };

    // --- main settings menu ---
    // ITEM_RANGE was added when touch (and with it the on-screen zoom button) was
    // removed. Inserting mid-list shifts the numeric meaning of a theme's saved
    // "default selection", so an older theme may now open Settings on a neighbouring
    // item. Cosmetic only, and DEFAULT_SEL below still clamps anything out of range.
    enum { ITEM_DISPLAY = 0, ITEM_LOCATION, ITEM_SOUND, ITEM_UNITS, ITEM_RANGE, ITEM_WIFI, ITEM_DESIGN, ITEM_ABOUT, ITEM_RESET, ITEM_BACK, ITEM_COUNT };
    const char *ITEM_LABELS[ITEM_COUNT] = { "Display", "Location", "Sound", "Units", "Range", "WiFi", "Theme", "About", "Reset", "Back" };
    // A Launch Kit push's "Default selection" (was editor-preview-only; now baked
    // in) — which item the main menu opens on, both at first boot and every time
    // the app switcher hands control back to Settings. Out-of-range (a stale
    // design from before ITEM_COUNT grew, say) falls back to item 0 rather than
    // reading garbage. Runtime (per active SD theme, see theme_style.h) rather than
    // constexpr — the same identifier, used the same way, just resolved at each
    // reference instead of baked in, since ArduinoJson can't produce a compile-time
    // constant.
    #define DEFAULT_SEL ((theme_style::settings().defaultSel >= 0 && theme_style::settings().defaultSel < ITEM_COUNT) ? theme_style::settings().defaultSel : 0)

    // --- units submenu (Weather app metric/imperial, Auto by default) ---
    enum { UNIT_MODE = 0, UNIT_BACK, UNIT_COUNT };

    // --- range submenu (Flight Tracker display range; steps come from config.h) ---
    enum { RNG_VALUE = 0, RNG_BACK, RNG_COUNT };
    const int RANGE_N = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));

    // --- display submenu (screen timeout + brightness) ---
    enum { DSP_SCREEN = 0, DSP_BRIGHT, DSP_THEME, DSP_BACK, DSP_COUNT };
    const uint32_t IDLE_MS[] = { 0, 28800000UL, 14400000UL, 7200000UL, 3600000UL, 1800000UL, 600000UL, 120000UL };
    const char *IDLE_LABELS[] = { "Always on", "8 hours", "4 hours", "2 hours", "1 hour", "30 min", "10 min", "2 min" };
    const int IDLE_N = (int)(sizeof(IDLE_MS) / sizeof(IDLE_MS[0]));

    constexpr int WIFI_MAX = 12;   // most-scanned networks shown, strongest signal wins on duplicates

    // Main-menu "wheel": the selected item always sits at dead center (the highlight
    // bar never moves — the items scroll past it instead), and neighbors are placed
    // by angle around a virtual cylinder rather than a flat linear list. sin() bunches
    // spacing up compressed toward the top/bottom the further an item is from the
    // selection, exactly like watching a real wheel turn edge-on, and doubles as the
    // fix for too many items crowding a round screen: distance-based fade + shrink
    // means only ~3 rows either side ever have any real presence, everything past that
    // is fully transparent, regardless of how many items ITEM_COUNT grows to.
    // A Launch Kit push overrides these five per the active SD theme (theme_style.h)
    // — same identifiers, resolved at each reference now instead of baked in as a
    // single shared compile-time constant (see DEFAULT_SEL above for why).
    #define WHEEL_R        (theme_style::settings().wheelR)        // virtual wheel radius, px
    #define WHEEL_RX       (theme_style::settings().wheelRx)       // horizontal recede at the far edge, px
    #define WHEEL_STEP_DEG (theme_style::settings().wheelStepDeg)  // angle per item away from the selection
    #define WHEEL_CY       (theme_style::settings().wheelCy)       // vertical offset of the stationary highlight/list center, px (0 = dead center)
    #define WHEEL_FADE     (theme_style::settings().wheelFade)     // opacity falloff steepness — see wheel_layout()'s opa line

    // --- sound submenu ---
    enum { SND_RADAR = 0, SND_CHIME, SND_CHIME_SEL, SND_VOLUME, SND_BACK, SND_COUNT };

    constexpr int VOL_STEP = 10;

    // --- chime picker (Sound > Chime sound) ---
    // Only "Westminster" exists today, but the list is sized for future named chimes
    // (see audio_chime_count() / chime_westminster.h) without any UI changes needed.
    constexpr int CHIME_UI_MAX = 8;

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
    int  s_sel   = DEFAULT_SEL; // main-menu selection
    int  s_bri   = 200;
    int  s_lmSel = 0;          // location-menu selection
    int  s_sndSel = 0;         // sound-menu selection
    int  s_chimeSel = 0;       // chime-picker selection (0..count-1 = a chime, count = Back)
    int  s_unitsSel = 0;       // units-menu selection
    int  s_rangeSel = 0;       // range-menu selection
    int  s_dspSel = 0;         // display-menu selection
    int  s_themeSel = 0;       // theme-picker selection (0..APP_THEME_COUNT-1 = a theme, APP_THEME_COUNT = Back)
    int  s_designSel = 0;      // design-picker selection (0..s_designCount-1 = a theme, s_designCount = Back)
    int  s_vol   = 60;         // volume working value

    // Installed Launch Kit themes (/themes/<slug>/ on the SD card), rescanned each
    // time the Design picker is entered — see refresh_designSelect().
    char s_designSlugs[theme_select::MAX_THEMES][theme_select::MAX_SLUG_LEN];
    int  s_designCount = 0;

    // recent cities
    char   s_recNames[RECENTS_MAX][40];
    double s_recLat[RECENTS_MAX], s_recLon[RECENTS_MAX];
    int    s_recCount = 0;
    int    s_recSel   = 0;     // 0..s_recCount-1 = a city, s_recCount = Back

    // WiFi setup (fully encoder-driven, styled like the rest of Settings)
    char    s_wifiNames[WIFI_MAX][33];
    int8_t  s_wifiRssi[WIFI_MAX];
    bool    s_wifiOpen[WIFI_MAX];
    int     s_wifiCount   = 0;
    int     s_wifiSel     = 0;         // list selection: 0..count-1 networks, count=Rescan, count+1=Back
    bool    s_wifiScanning = false;
    char    s_wifiSelSsid[33] = "";
    bool    s_wifiSelOpen     = false;
    bool    s_wifiConnecting  = false;

    // password entry (same character-strip picker as the city search)
    char    s_pass[65]  = "";
    int     s_wkbIdx    = 0;
    // printable password charset + two trailing virtual keys: DEL and OK(connect)
    const char WKEYS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !@#$%^&*()-_=+.,:;?/";
    const int  N_WKEYS = (int)(sizeof(WKEYS) - 1);
    const int  WK_DEL  = N_WKEYS;          // strip index for backspace
    const int  WK_OK   = N_WKEYS + 1;      // strip index for connect
    const int  WK_TOTAL = N_WKEYS + 2;

    constexpr int WIFI_VISIBLE = 5;        // rows shown at once in the scrolling network list
    constexpr int WIFI_ROW_DY  = 44;

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
    lv_obj_t *s_dspPage = nullptr;   // display menu (screen timeout + brightness)
    lv_obj_t *s_dspHl   = nullptr;
    lv_obj_t *s_dspItems[DSP_COUNT] = { nullptr };
    lv_obj_t *s_sndPage = nullptr;   // sound menu
    lv_obj_t *s_sndHl   = nullptr;
    lv_obj_t *s_sndItems[SND_COUNT] = { nullptr };
    lv_obj_t *s_unitsPage = nullptr;   // units menu
    lv_obj_t *s_unitsHl   = nullptr;
    lv_obj_t *s_unitsItems[UNIT_COUNT] = { nullptr };
    lv_obj_t *s_rangePage = nullptr;   // range menu (Flight Tracker display range)
    lv_obj_t *s_rangeHl   = nullptr;
    lv_obj_t *s_rangeItems[RNG_COUNT] = { nullptr };
    lv_obj_t *s_chimeSelPage = nullptr;   // chime picker (Sound > Chime sound)
    lv_obj_t *s_chimeSelHl   = nullptr;
    lv_obj_t *s_chimeSelItems[CHIME_UI_MAX + 1] = { nullptr };   // chimes + Back
    lv_obj_t *s_themeSelPage = nullptr;   // theme picker (Display > Theme)
    lv_obj_t *s_themeSelHl   = nullptr;
    lv_obj_t *s_themeSelItems[APP_THEME_COUNT + 1] = { nullptr };   // themes + Back
    lv_obj_t *s_themeNoticePage = nullptr;   // "restarting..." heads-up, shown right before the reboot
    lv_obj_t *s_designPage = nullptr;   // design picker (top-level Design item)
    lv_obj_t *s_designHl   = nullptr;
    lv_obj_t *s_designItems[theme_select::MAX_THEMES + 1] = { nullptr };   // installed themes + Back
    lv_obj_t *s_designNoticePage = nullptr;   // "restarting..." heads-up, shown right before the reboot
    lv_obj_t *s_volPage = nullptr;   // volume adjuster
    lv_obj_t *s_volFill = nullptr;
    lv_obj_t *s_volPct  = nullptr;
    lv_obj_t *s_plateImg = nullptr;    // themed background, built on enter / freed on exit
    lv_obj_t *s_ovImg    = nullptr;    // themed CRT+glass, same lifecycle
    lv_obj_t *s_aboutPage = nullptr;   // About: the boot splash, push to return
    // The version, address and credit lines used to be three labels built here at hardcoded
    // offsets. They are theme data now (splash_style.json) and are drawn by splash_lines,
    // which the boot splash also uses, so the two places that show this picture cannot
    // drift apart again.
    char      s_netInfo[112] = "";     // last line handed to setNetInfo(), replayed on page open
    lv_obj_t *s_aboutImg  = nullptr;   // decoded fresh each time (see refresh_about()) — cheap, avoids relying on splash_art's shared decode buffer staying valid
    // --- first-boot WiFi choice (UX-019 as amended 2026-08-30, UX-022) ---
    //
    // The one screen on the device that offers a choice the owner did not arrive wanting to
    // make. UX-005 forbids that everywhere else and the amendment carves out this screen
    // only: the first minute with a new object is the one moment where somebody who cannot
    // find their way has no fallback and no reason to persist. The on-device path is
    // pre-selected, so the default is still a single press.
    enum { FB_ONDEVICE = 0, FB_PHONE, FB_COUNT };
    const char *FB_LABELS[FB_COUNT] = { "Choose a network here", "Use my phone instead" };
    lv_obj_t *s_fbPage  = nullptr;
    lv_obj_t *s_fbHl    = nullptr;
    lv_obj_t *s_fbItems[FB_COUNT] = { nullptr, nullptr };
    int       s_fbSel   = FB_ONDEVICE;
    // Set when boot wanted the WiFi choice but the card notice had to come first.
    bool      s_pendingWifiSetup = false;
    lv_obj_t *s_fbPhonePage = nullptr;

    // --- no readable SD card (UX-024) ---
    //
    // Dismissible with a press, deliberately. UX-024 asks the Orb to SAY it needs a card,
    // in words rather than an error code; it does not ask it to refuse to run, and UX-041
    // says a screen the owner cannot turn away from is its own fault. The device does in
    // fact still work without a card — it falls back to the flash-baked artwork — so
    // trapping somebody on this notice would break a working Orb to report a degraded one.
    // Said once, unmissably, at the only moment it is actionable.
    lv_obj_t *s_noSdPage = nullptr;

    lv_obj_t *s_resetPage = nullptr;   // Reset: warning + confirm, push to wipe, turn to cancel

    // WiFi setup pages (encoder-driven, same look as the rest of Settings)
    lv_obj_t *s_wifiListPage  = nullptr;    // scrolling network list
    lv_obj_t *s_wifiHl        = nullptr;    // highlight bar
    lv_obj_t *s_wifiRows[WIFI_VISIBLE] = { nullptr };
    lv_obj_t *s_wifiListHint  = nullptr;
    bool      s_firstBootPrompt = false;    // set by openWifiSetupPrompt(); swaps the WiFi list's hint text
    lv_obj_t *s_wifiPassPage  = nullptr;    // password: text line + character strip
    lv_obj_t *s_wifiPassTitle = nullptr;
    lv_obj_t *s_passText      = nullptr;
    lv_obj_t *s_wkStrip[7]    = { nullptr };
    lv_obj_t *s_wifiPassHint  = nullptr;
    lv_obj_t *s_wifiStatusPage= nullptr;
    lv_obj_t *s_wifiStatusLbl = nullptr;
    lv_obj_t *s_wifiStatusHint= nullptr;

    // Runtime-set from app_theme::palette() at init() — Office swaps every one of these
    // (see settingsview::init()) the same way ui.cpp's UI_* variables do.
    lv_color_t C_WHITE = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF);   // primary text (selected row)
    lv_color_t C_GREY  = LV_COLOR_MAKE(0x6A, 0x70, 0x78);   // secondary text (unselected rows)
    lv_color_t C_DIM   = LV_COLOR_MAKE(0x9A, 0xA0, 0xA6);   // hints
    lv_color_t C_ACCENT= LV_COLOR_MAKE(0x4F, 0xC3, 0xF7);   // slider fill / links
    lv_color_t C_BG    = lv_color_black();                  // screen background
    lv_color_t C_HL    = lv_color_hex(0x232A36);            // selected-row pill fill
    lv_color_t C_TRACK = lv_color_hex(0x2A2E33);            // slider track

    // Every one of the 7 highlight pills (main menu + the 6 sub-lists below)
    // calls this right after lv_obj_create(), so a Launch Kit push's highlight
    // style is identical everywhere instead of only the top-level list —
    // matching wheel_layout()'s own already-shared color/shape.
    void style_highlight(lv_obj_t *hl) {
        lv_obj_remove_style_all(hl);
#if CUSTOM_HAS_SETTINGS
        const theme_style::Settings &ss = theme_style::settings();
        lv_obj_set_size(hl, ss.hlW, ss.hlH);
        lv_obj_set_style_radius(hl, ss.hlRadius, 0);
        lv_obj_set_style_bg_color(hl, lv_color_hex(ss.hlColor), 0);
        lv_obj_set_style_bg_opa(hl, ss.hlShow ? (lv_opa_t)ss.hlOpacity : LV_OPA_TRANSP, 0);
#else
        lv_obj_set_size(hl, 300, 44);
        lv_obj_set_style_radius(hl, 10, 0);
        lv_obj_set_style_bg_color(hl, C_HL, 0);
        lv_obj_set_style_bg_opa(hl, LV_OPA_COVER, 0);
#endif
    }

    // Shared by every fixed-item menu in Settings (main menu, Display, Sound, Location)
    // so they all roll the same way and one tuning change (WHEEL_R etc.) moves them all
    // together. The selected item sits at a fixed vertical spot (screen center plus
    // WHEEL_CY, 0 by default); hl (if given) sits fixed there too — nothing slides,
    // the wheel scrolls past a stationary highlight/gate.
    void wheel_layout(lv_obj_t **items, int count, int sel, lv_obj_t *hl) {
#if CUSTOM_HAS_SETTINGS
        // A Launch Kit push draws every item itself, on the shared glow-capable
        // canvas (settings_text.cpp) — LVGL labels have no glow, the same reason
        // the app-switcher menu overlay isn't plain labels either. One frame
        // per call (this fires once per knob turn, cheap): clear, then redraw
        // every item this list currently has. The label objects still exist and
        // still get positioned below (harmless, just invisible) so nothing else
        // that reads their geometry breaks.
        settings_text::begin_frame();
#endif
        for (int i = 0; i < count; ++i) {
            const int d = i - sel;
            const float angleDeg = fabsf((float)d) * WHEEL_STEP_DEG;
            // Past a quarter turn there is no more dial to put anything on.
            //
            // This angle used to be CLAMPED to 90 rather than cut off, which is what made a
            // long list pile up: at 22 degrees a step, everything five or more rows from the
            // selection got exactly 90, sin(90) is 1 for all of them, and they landed on the
            // same pixel at the top of the wheel drawn over one another. On the ten-row main
            // list, scrolling to the bottom stacked five labels in one place.
            //
            // They were supposed to be invisible by then, and on paper they are: cos(90) is
            // 0, so the falloff below is 0. In floating point cosf(M_PI/2) is about -4.4e-8,
            // a tiny NEGATIVE number, and powf(negative, fractional) is NaN. Any Fade that
            // is not a whole or half number makes that exponent fractional, and the NaN cast
            // to lv_opa_t is whatever the conversion happens to produce. So the row that
            // should have been invisible was drawn, at an opacity nobody chose.
            //
            // Cut off rather than clamped, and the falloff floored at 0 so no value of Fade
            // can produce a NaN.
            const bool offDial = angleDeg >= 90.0f;
            const float angleRad = fminf(angleDeg, 90.0f) * (float)M_PI / 180.0f;
            const float sy = WHEEL_CY + (d < 0 ? -1.0f : 1.0f) * WHEEL_R * sinf(angleRad);
            const float sx = WHEEL_RX * (1.0f - cosf(angleRad));
            lv_obj_align(items[i], LV_ALIGN_CENTER, (lv_coord_t)lroundf(sx), (lv_coord_t)lroundf(sy));

            // Continuous falloff off the same angle used for position — cos(angle)
            // raised to 2*WHEEL_FADE — rather than a fixed per-row table, so Fade
            // is a real dial (Launch Kit's Wheel shape > Fade) instead of 4 baked
            // numbers. Font stepping stays a fixed 3-step table: it's cosmetic,
            // stock-look-only (a Launch Kit push draws one uniform size — see
            // settings_text::draw_item below), and unrelated to the fade itself.
            const int ad = abs(d);
            const float fall = fmaxf(0.0f, cosf(angleRad));
            const lv_opa_t opa = offDial ? 0
                               : (lv_opa_t)lroundf(255.0f * powf(fall, 2.0f * WHEEL_FADE));
            const lv_font_t *font;
            if      (ad == 0) font = &lv_font_montserrat_20;
            else if (ad == 1) font = &lv_font_montserrat_16;
            else              font = &lv_font_montserrat_14;
#if CUSTOM_HAS_SETTINGS
          // The theme's own opacity for this row, MULTIPLIED into the wheel's distance fade
          // rather than replacing it. The fade is what makes the wheel read as a wheel; a
          // theme asking for faint text is asking for faint text at every position on it.
          const lv_opa_t rowOpa = (lv_opa_t)((int)opa *
              ((i == sel) ? theme_style::settings().selOpa : theme_style::settings().itemOpa) / 255);
          if (settings_text::available()) {
            lv_obj_set_style_text_opa(items[i], LV_OPA_TRANSP, 0);   // the native label draws nothing; the canvas draws the real glyphs below
            // Positioned above like every other row, so anything reading these objects'
            // geometry still finds them where it expects; simply not drawn.
            if (offDial) continue;
            const theme_style::Settings &sg = theme_style::settings();
            settings_text::draw_item(lv_label_get_text(items[i]), 233.0f + sx, 233.0f + sy,
                                     lv_color_hex(i == sel ? sg.selColor : sg.itemColor), rowOpa,
                                     i == sel ? sg.selGlow : sg.itemGlow,
                                     lv_color_hex(i == sel ? sg.selGlowColor : sg.itemGlowColor),
                                     i == sel ? theme_font::settings_sel() : theme_font::settings_item());
          } else {
            // No canvas (either it could not be allocated, or the theme asks for no glow
            // so we deliberately skipped it). Draw with plain labels, but still using the
            // THEME's font and colours: the canvas only ever added glow on top of those,
            // and falling back to the stock font stepping made a themed device suddenly
            // render Settings in the wrong size and weight.
            lv_obj_set_style_text_font(items[i],
                i == sel ? theme_font::settings_sel() : theme_font::settings_item(), 0);
            lv_obj_set_style_text_opa(items[i], rowOpa, 0);
            lv_obj_set_style_text_color(items[i],
                lv_color_hex(i == sel ? theme_style::settings().selColor
                                      : theme_style::settings().itemColor), 0);
            (void)font;   // stock 3-step sizing is not used when a theme is active
          }
#else
            lv_obj_set_style_text_font(items[i], font, 0);
            lv_obj_set_style_text_opa(items[i], opa, 0);
            lv_obj_set_style_text_color(items[i], i == sel ? C_WHITE : C_GREY, 0);
#endif
        }
        if (hl) lv_obj_align(hl, LV_ALIGN_CENTER, 0, (lv_coord_t)lroundf(WHEEL_CY));
    }

    void refresh_menu() { wheel_layout(s_items, ITEM_COUNT, s_sel, s_hl); }

    int idle_index() {   // which IDLE_MS entry the current timeout matches (default 1 hour)
        const uint32_t cur = host_get_idle_ms();
        for (int i = 0; i < IDLE_N; ++i) if (IDLE_MS[i] == cur) return i;
        return 4;   // 1 hour
    }

    void refresh_display() {
        char b[28];
        snprintf(b, sizeof(b), "Screen   %s", IDLE_LABELS[idle_index()]);
        lv_label_set_text(s_dspItems[DSP_SCREEN], b);
        lv_label_set_text(s_dspItems[DSP_BRIGHT], "Brightness");
        snprintf(b, sizeof(b), "Theme   %s", app_theme::name(app_theme::get()));
        lv_label_set_text(s_dspItems[DSP_THEME], b);
        lv_label_set_text(s_dspItems[DSP_BACK], "Back");
        wheel_layout(s_dspItems, DSP_COUNT, s_dspSel, s_dspHl);
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
        snprintf(b, sizeof(b), "Chime sound   %s", host_chime_name(host_chime_index()));
        lv_label_set_text(s_sndItems[SND_CHIME_SEL], b);
        snprintf(b, sizeof(b), "Volume   %d%%", host_get_volume());
        lv_label_set_text(s_sndItems[SND_VOLUME], b);
        lv_label_set_text(s_sndItems[SND_BACK], "Back");
        wheel_layout(s_sndItems, SND_COUNT, s_sndSel, s_sndHl);
    }

    // Chime picker: turning previews each chime live (host_chime_preview), pressing
    // confirms it (host_chime_set) and returns to Sound. Sized for CHIME_UI_MAX chimes
    // though only one ("Westminster") exists today.
    int chime_item_count() { return host_chime_count() + 1; }   // chimes + Back

    void refresh_chimeSelect() {
        const int n = host_chime_count();
        for (int i = 0; i < n && i < CHIME_UI_MAX; ++i)
            lv_label_set_text(s_chimeSelItems[i], host_chime_name(i));
        lv_label_set_text(s_chimeSelItems[n], "Back");
        wheel_layout(s_chimeSelItems, chime_item_count(), s_chimeSel, s_chimeSelHl);
    }

    // Theme picker: turning browses Default/Office, pressing shows the restart notice
    // and applies it (settingsview::onPress). Sized for APP_THEME_COUNT themes.
    void refresh_themeSelect() {
        for (int i = 0; i < APP_THEME_COUNT; ++i)
            lv_label_set_text(s_themeSelItems[i], app_theme::name(i));
        lv_label_set_text(s_themeSelItems[APP_THEME_COUNT], "Back");
        wheel_layout(s_themeSelItems, APP_THEME_COUNT + 1, s_themeSel, s_themeSelHl);
    }

    int design_item_count() { return s_designCount + 1; }   // installed themes + Back

    // Design picker (top-level "Design" item): turning browses whichever Launch Kit
    // themes are actually installed on the SD card, pressing shows the restart
    // notice and applies it (settingsview::onPress) — same shape as the Theme
    // (Default/Office) picker above, just backed by theme_select's dynamic slug
    // list instead of a fixed 2-entry enum. Rescans the card every time this page
    // is entered (see show_page's MODE_DESIGN_SELECT dispatch) rather than once at
    // boot, so a card swapped since boot (or a fresh export copied over) shows up
    // without a full reboot just to see it.
    void refresh_designSelect() {
        s_designCount = theme_select::listInstalled(s_designSlugs);
        if (s_designCount > theme_select::MAX_THEMES) s_designCount = theme_select::MAX_THEMES;
        // Show each theme's display name, never its folder slug. The theme shown as
        // "Modern" lives in a folder called `the-office` (its former name), and putting
        // the slug on screen made the two look like different themes.
        //
        // UNLESS two of them say the same thing, which happens more than it should: Studio
        // gives a new theme a new folder, so saving a design twice under one name leaves two
        // folders on the card, both calling themselves Aviator. Two identical rows and no
        // way to tell which is which is worse than showing a folder name, so a name that
        // collides earns its slug in brackets and a name that does not is left alone.
        for (int i = 0; i < s_designCount; ++i) {
            char label[32];
            theme_style::labelFor(s_designSlugs[i], label, sizeof(label));
            bool clash = false;
            for (int j = 0; j < s_designCount && !clash; ++j) {
                if (j == i) continue;
                char other[32];
                theme_style::labelFor(s_designSlugs[j], other, sizeof(other));
                clash = strcmp(label, other) == 0;
            }
            if (clash) {
                char shown[64];
                snprintf(shown, sizeof(shown), "%.20s (%.16s)", label, s_designSlugs[i]);
                lv_label_set_text(s_designItems[i], shown);
            } else {
                lv_label_set_text(s_designItems[i], label);
            }
        }
        lv_label_set_text(s_designItems[s_designCount], "Back");
        for (int i = s_designCount + 1; i < theme_select::MAX_THEMES + 1; ++i) lv_label_set_text(s_designItems[i], "");
        if (s_designSel > s_designCount) s_designSel = s_designCount;
        wheel_layout(s_designItems, design_item_count(), s_designSel, s_designHl);
    }

    void refresh_units() {
        const int mode = host_wx_units_mode();
        const char *resolved = host_wx_is_imperial() ? "F, mi" : "C, km";
        char b[36];
        if (mode == 0) snprintf(b, sizeof(b), "Units   Auto (%s)", resolved);
        else           snprintf(b, sizeof(b), "Units   %s", mode == 2 ? "Imperial (F, mi)" : "Metric (C, km)");
        lv_label_set_text(s_unitsItems[UNIT_MODE], b);
        lv_label_set_text(s_unitsItems[UNIT_BACK], "Back");
        wheel_layout(s_unitsItems, UNIT_COUNT, s_unitsSel, s_unitsHl);
    }

    void refresh_range() {
        char b[36];
        snprintf(b, sizeof(b), "Range   %.0f km", (double)host_get_range_km());
        lv_label_set_text(s_rangeItems[RNG_VALUE], b);
        lv_label_set_text(s_rangeItems[RNG_BACK], "Back");
        wheel_layout(s_rangeItems, RNG_COUNT, s_rangeSel, s_rangeHl);
    }

    void refresh_vol() {
        lv_obj_set_width(s_volFill, (lv_coord_t)(4 + s_vol * (236 - 4) / 100));
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", s_vol);
        lv_label_set_text(s_volPct, buf);
    }

    void refresh_locmenu() { wheel_layout(s_lmItems, LM_COUNT, s_lmSel, s_lmHl); }

    // Same wheel every other menu uses, deliberately. The mockup drew a ">" against the
    // chosen row; the wheel's own highlight pill already says that, in the grammar the rest
    // of the device has taught, and a second marker would be the only one of its kind here.
    void refresh_firstboot() { wheel_layout(s_fbItems, FB_COUNT, s_fbSel, s_fbHl); }

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

    void refresh_wifi_list();     // defined below (used by show_page)
    void refresh_wifi_pass();

    // Re-decodes on every entry rather than caching: splash_art_decode()'s target buffer
    // is shared with ui_splash_show(), so holding onto a stale lv_img_dsc_t across a boot
    // splash redecode would be wrong. Decoding is a one-time-per-visit PNG unpack, cheap.
    void refresh_about() {
        static lv_img_dsc_t aboutImg;
        if (splash_art_decode(app_theme::get() == APP_THEME_OFFICE, &aboutImg))
            lv_img_set_src(s_aboutImg, &aboutImg);
        // Built on entry rather than at init: the canvas is 651 KB of PSRAM and this page
        // is visited, not lived on. release() runs when the page closes.
        splash_lines::attach(s_aboutPage);
        splash_lines::setNetwork(s_netInfo);
    }

    void show_page(Mode m) {
        s_mode = m;
        // The wheel-list text canvas (settings_text) is the topmost child of
        // s_screen — drawn over whichever page is visible — but it's only ever
        // cleared inside wheel_layout(), called from each *list* page's own
        // refresh_*(). A non-list page (About, Reset confirm, WiFi status, the
        // theme/design notices) never calls that, so without this the canvas
        // just keeps showing whatever list was drawn last, bled on top of
        // whatever's underneath. Clearing unconditionally here, before the mode
        // switch below, means every page starts blank and a list page's own
        // refresh_*() (called a few lines down) redraws its own items right
        // back — cheap (one canvas clear) and safe even in stock builds, where
        // begin_frame() is a no-op with no canvas to clear.
        settings_text::begin_frame();
        lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bright, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lmPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_recPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_srchPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sndPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_chimeSelPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_unitsPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_rangePage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_volPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_aboutPage, LV_OBJ_FLAG_HIDDEN);
        // Hiding the About page does not free its canvas, and that canvas is 651 KB of
        // PSRAM. Every other page here is a handful of labels; this one is not, so it is
        // the one page that has to give its memory back when it leaves the screen.
        splash_lines::release();
        lv_obj_add_flag(s_resetPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_fbPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_fbPhonePage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_noSdPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifiListPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifiPassPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wifiStatusPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_dspPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_themeSelPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_themeNoticePage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_designPage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_designNoticePage, LV_OBJ_FLAG_HIDDEN);
        if (m == MODE_MENU)          { lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_HIDDEN);    refresh_menu(); }
        else if (m == MODE_DISPLAY)  { lv_obj_clear_flag(s_dspPage, LV_OBJ_FLAG_HIDDEN); refresh_display(); }
        else if (m == MODE_BRIGHT)   { lv_obj_clear_flag(s_bright, LV_OBJ_FLAG_HIDDEN);  refresh_bright(); }
        else if (m == MODE_LOCATION) { lv_obj_clear_flag(s_lmPage, LV_OBJ_FLAG_HIDDEN);  refresh_locmenu(); }
        else if (m == MODE_RECENT)   { lv_obj_clear_flag(s_recPage, LV_OBJ_FLAG_HIDDEN); refresh_recent(); }
        else if (m == MODE_SOUND)    { lv_obj_clear_flag(s_sndPage, LV_OBJ_FLAG_HIDDEN); refresh_sound(); }
        else if (m == MODE_CHIME_SELECT) { lv_obj_clear_flag(s_chimeSelPage, LV_OBJ_FLAG_HIDDEN); refresh_chimeSelect(); }
        else if (m == MODE_UNITS)    { lv_obj_clear_flag(s_unitsPage, LV_OBJ_FLAG_HIDDEN); refresh_units(); }
        else if (m == MODE_RANGE)    { lv_obj_clear_flag(s_rangePage, LV_OBJ_FLAG_HIDDEN); refresh_range(); }
        else if (m == MODE_VOLUME)   { lv_obj_clear_flag(s_volPage, LV_OBJ_FLAG_HIDDEN); refresh_vol(); }
        else if (m == MODE_ABOUT)    { lv_obj_clear_flag(s_aboutPage, LV_OBJ_FLAG_HIDDEN); refresh_about(); }
        else if (m == MODE_RESET_CONFIRM) { lv_obj_clear_flag(s_resetPage, LV_OBJ_FLAG_HIDDEN); }
        else if (m == MODE_FIRSTBOOT)       { lv_obj_clear_flag(s_fbPage, LV_OBJ_FLAG_HIDDEN); refresh_firstboot(); }
        else if (m == MODE_FIRSTBOOT_PHONE) { lv_obj_clear_flag(s_fbPhonePage, LV_OBJ_FLAG_HIDDEN); }
        else if (m == MODE_NO_SDCARD)       { lv_obj_clear_flag(s_noSdPage, LV_OBJ_FLAG_HIDDEN); }
        else if (m == MODE_WIFI_LIST)     { lv_obj_clear_flag(s_wifiListPage, LV_OBJ_FLAG_HIDDEN); refresh_wifi_list(); }
        else if (m == MODE_WIFI_PASSWORD) { lv_obj_clear_flag(s_wifiPassPage, LV_OBJ_FLAG_HIDDEN); refresh_wifi_pass(); }
        else if (m == MODE_WIFI_STATUS)   { lv_obj_clear_flag(s_wifiStatusPage, LV_OBJ_FLAG_HIDDEN); }
        else if (m == MODE_THEME_SELECT)  { lv_obj_clear_flag(s_themeSelPage, LV_OBJ_FLAG_HIDDEN); refresh_themeSelect(); }
        else if (m == MODE_THEME_NOTICE)  { lv_obj_clear_flag(s_themeNoticePage, LV_OBJ_FLAG_HIDDEN); }
        else if (m == MODE_DESIGN_SELECT) { lv_obj_clear_flag(s_designPage, LV_OBJ_FLAG_HIDDEN); refresh_designSelect(); }
        else if (m == MODE_DESIGN_NOTICE) { lv_obj_clear_flag(s_designNoticePage, LV_OBJ_FLAG_HIDDEN); }
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

    // ---- WiFi setup (encoder-driven: scrolling list -> character-strip password) ----

    int wifi_item_count() { return s_wifiCount + 2; }   // networks + "Rescan" + "Back"

    void wifi_item_name(int idx, char *out, size_t n) {
        if (idx < s_wifiCount)       snprintf(out, n, "%s", s_wifiNames[idx]);
        else if (idx == s_wifiCount) snprintf(out, n, "Rescan");
        else                         snprintf(out, n, "Back");
    }

    void refresh_wifi_list() {
        if (s_wifiScanning) {
            lv_obj_add_flag(s_wifiHl, LV_OBJ_FLAG_HIDDEN);
            for (int r = 0; r < WIFI_VISIBLE; ++r) lv_label_set_text(s_wifiRows[r], "");
            lv_label_set_text(s_wifiRows[WIFI_VISIBLE / 2], "Scanning...");
            lv_obj_set_style_text_color(s_wifiRows[WIFI_VISIBLE / 2], C_DIM, 0);
            lv_label_set_text(s_wifiListHint, "");
            return;
        }
        const int total = wifi_item_count();
        int top = s_wifiSel - WIFI_VISIBLE / 2;
        if (top > total - WIFI_VISIBLE) top = total - WIFI_VISIBLE;
        if (top < 0) top = 0;
        for (int r = 0; r < WIFI_VISIBLE; ++r) {
            const int idx = top + r;
            if (idx < total) {
                char nm[40];
                wifi_item_name(idx, nm, sizeof(nm));
                lv_label_set_text(s_wifiRows[r], nm);
                lv_obj_set_style_text_color(s_wifiRows[r], idx == s_wifiSel ? C_WHITE : C_GREY, 0);
            } else {
                lv_label_set_text(s_wifiRows[r], "");
            }
        }
        const int hlRow = s_wifiSel - top;
        lv_obj_clear_flag(s_wifiHl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_wifiHl, LV_ALIGN_CENTER, 0, -(WIFI_VISIBLE - 1) * WIFI_ROW_DY / 2 + hlRow * WIFI_ROW_DY);
        lv_label_set_text(s_wifiListHint, s_firstBootPrompt ? "Start by connecting to your local Wi-Fi"
                                                             : "turn to choose, push to select");
    }

    void refresh_wifi_pass() {
        lv_label_set_text(s_passText, s_pass[0] ? s_pass : "(enter password)");
        for (int k = 0; k < 7; ++k) {
            const int idx = s_wkbIdx - 3 + k;
            const bool hot = (k == 3);
            char c[8] = { 0 };
            if (idx < 0 || idx >= WK_TOTAL) c[0] = 0;
            else if (idx == WK_DEL) snprintf(c, sizeof(c), "DEL");
            else if (idx == WK_OK)  snprintf(c, sizeof(c), "OK");
            else if (WKEYS[idx] == ' ') snprintf(c, sizeof(c), "SP");   // show space as "SP"
            else { c[0] = WKEYS[idx]; c[1] = 0; }
            lv_label_set_text(s_wkStrip[k], c);
            lv_obj_set_style_text_color(s_wkStrip[k], hot ? C_WHITE : C_GREY, 0);
            lv_obj_set_style_text_font(s_wkStrip[k], hot ? &lv_font_montserrat_28 : &lv_font_montserrat_18, 0);
        }
    }

    void start_wifi_scan() {
        s_wifiScanning = true;
        s_wifiCount = 0;
        s_wifiSel = 0;
        diag::log("wifi: scan start");
        host_wifi_scan_start();
        refresh_wifi_list();
    }

    void wifi_begin_connect(const char *pass) {
        s_wifiConnecting = true;
        // s_firstBootPrompt stays set through to the connect result — wifi_tick() uses it
        // to decide whether to auto-locate on success (see there).
        lv_label_set_text_fmt(s_wifiStatusLbl, "Connecting to\n%s...", s_wifiSelSsid);
        lv_label_set_text(s_wifiStatusHint, "");
        host_wifi_connect(s_wifiSelSsid, pass);
        show_page(MODE_WIFI_STATUS);
    }

    // Selecting a network from the list: connect straight away if open, else ask for a password.
    void wifi_pick_network(int idx) {
        snprintf(s_wifiSelSsid, sizeof(s_wifiSelSsid), "%s", s_wifiNames[idx]);
        s_wifiSelOpen = s_wifiOpen[idx];
        if (s_wifiSelOpen) {
            wifi_begin_connect("");
        } else {
            s_pass[0] = 0; s_wkbIdx = 0;
            lv_label_set_text_fmt(s_wifiPassTitle, "Password: %s", s_wifiSelSsid);
            refresh_wifi_pass();
            show_page(MODE_WIFI_PASSWORD);
        }
    }

    void wifi_tick(lv_timer_t * /*t*/) {
        if (s_mode == MODE_WIFI_LIST && s_wifiScanning) {
            const int n = host_wifi_scan_result(s_wifiNames, s_wifiRssi, s_wifiOpen, WIFI_MAX);
            if (n == -1) return;                        // scan still running
            s_wifiScanning = false;
            s_wifiCount = (n < 0) ? 0 : n;
            s_wifiSel = 0;
            diag::log("wifi: scan done, %d networks", s_wifiCount);
            refresh_wifi_list();
        } else if (s_mode == MODE_WIFI_STATUS && s_wifiConnecting) {
            const int st = host_wifi_connect_status();
            if (st == 0) return;                        // still connecting
            s_wifiConnecting = false;
            if (st == 1) {
                lv_label_set_text(s_wifiStatusLbl, "Connected!");
                if (s_firstBootPrompt) {
                    // First-time setup (fresh out of the box, or right after a Reset):
                    // auto-detect location from the IP instead of making them go set it
                    // manually. host_locate_current() reboots on success and never
                    // returns; it only returns (false) if the lookup itself failed, in
                    // which case fall back to a plain reboot so this doesn't just hang.
                    lv_label_set_text(s_wifiStatusHint, "finding your location...");
                    lv_refr_now(NULL);
                    if (!host_locate_current()) {
                        lv_label_set_text(s_wifiStatusHint, "restarting...");
                        host_wifi_connected_reboot();
                    }
                } else {
                    lv_label_set_text(s_wifiStatusHint, "restarting...");
                    host_wifi_connected_reboot();
                }
            } else {
                lv_label_set_text_fmt(s_wifiStatusLbl, "Couldn't connect to\n%s.\nCheck the password.", s_wifiSelSsid);
                lv_label_set_text(s_wifiStatusHint, "push to go back");
            }
        }
    }
}

void settingsview::onTurn(int delta) {
    const int step = (delta > 0) ? 1 : -1;
    if (s_mode == MODE_MENU) {
        s_sel = (s_sel + step < 0) ? 0 : (s_sel + step >= ITEM_COUNT ? ITEM_COUNT - 1 : s_sel + step);
        refresh_menu();
    } else if (s_mode == MODE_DISPLAY) {
        s_dspSel += step;
        if (s_dspSel < 0) s_dspSel = 0;
        if (s_dspSel >= DSP_COUNT) s_dspSel = DSP_COUNT - 1;
        refresh_display();
    } else if (s_mode == MODE_BRIGHT) {
        s_bri += delta * BRI_STEP;
        if (s_bri < BRI_MIN) s_bri = BRI_MIN;
        if (s_bri > BRI_MAX) s_bri = BRI_MAX;
        host_set_brightness(s_bri, false);
        refresh_bright();
    } else if (s_mode == MODE_FIRSTBOOT) {
        s_fbSel += step;
        if (s_fbSel < 0) s_fbSel = 0;
        if (s_fbSel >= FB_COUNT) s_fbSel = FB_COUNT - 1;
        refresh_firstboot();
    } else if (s_mode == MODE_FIRSTBOOT_PHONE) {
        // Nothing to move between. The only action is Back, and it is on the press.
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
    } else if (s_mode == MODE_CHIME_SELECT) {
        const int total = chime_item_count();
        s_chimeSel += step;
        if (s_chimeSel < 0) s_chimeSel = 0;
        if (s_chimeSel >= total) s_chimeSel = total - 1;
        refresh_chimeSelect();
        if (s_chimeSel < host_chime_count()) host_chime_preview(s_chimeSel);   // hear it as you browse
    } else if (s_mode == MODE_UNITS) {
        s_unitsSel += step;
        if (s_unitsSel < 0) s_unitsSel = 0;
        if (s_unitsSel >= UNIT_COUNT) s_unitsSel = UNIT_COUNT - 1;
        refresh_units();
    } else if (s_mode == MODE_RANGE) {
        s_rangeSel += step;
        if (s_rangeSel < 0) s_rangeSel = 0;
        if (s_rangeSel >= RNG_COUNT) s_rangeSel = RNG_COUNT - 1;
        refresh_range();
    } else if (s_mode == MODE_VOLUME) {
        s_vol += delta * VOL_STEP;
        if (s_vol < 0) s_vol = 0;
        if (s_vol > 100) s_vol = 100;
        host_set_volume(s_vol, false);      // live preview level
        refresh_vol();
    } else if (s_mode == MODE_WIFI_LIST) {
        const int total = wifi_item_count();
        s_wifiSel += step;
        if (s_wifiSel < 0) s_wifiSel = 0;
        if (s_wifiSel >= total) s_wifiSel = total - 1;
        refresh_wifi_list();
    } else if (s_mode == MODE_WIFI_PASSWORD) {
        s_wkbIdx += step;
        if (s_wkbIdx < 0) s_wkbIdx = 0;
        if (s_wkbIdx >= WK_TOTAL) s_wkbIdx = WK_TOTAL - 1;
        refresh_wifi_pass();
    } else if (s_mode == MODE_THEME_SELECT) {
        s_themeSel += step;
        if (s_themeSel < 0) s_themeSel = 0;
        if (s_themeSel >= APP_THEME_COUNT + 1) s_themeSel = APP_THEME_COUNT;
        refresh_themeSelect();
    } else if (s_mode == MODE_DESIGN_SELECT) {
        const int total = design_item_count();
        s_designSel += step;
        if (s_designSel < 0) s_designSel = 0;
        if (s_designSel >= total) s_designSel = total - 1;
        wheel_layout(s_designItems, total, s_designSel, s_designHl);   // rescanning the card every turn would be wasteful — just re-layout
    } else if (s_mode == MODE_ABOUT) {
        // Turning backs out to the list, the same way the reset confirmation does.
        //
        // This page used to ignore the knob entirely, and a page that ignores the knob is
        // the one place on this device where the only control does nothing. Pressing left
        // to the app switcher, which is a fine way out of Settings but the wrong way out of
        // ONE PAGE of it: you came here from the list and the list is where back means.
        s_sel = ITEM_ABOUT;      // land on the row you left from, not at the top
        show_page(MODE_MENU);
    } else if (s_mode == MODE_WIFI_STATUS || s_mode == MODE_THEME_NOTICE || s_mode == MODE_DESIGN_NOTICE) {
        // static pages — turning does nothing here
    } else if (s_mode == MODE_RESET_CONFIRM) {
        s_sel = ITEM_RESET;      // turning either way backs out — this page is confirm/cancel only
        show_page(MODE_MENU);
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
namespace {
    void settings_art_acquire() {
        if (!s_plateImg) {
            if (const lv_img_dsc_t *plate = settings_custom_plate()) {
                s_plateImg = lv_img_create(s_screen);
                lv_img_set_src(s_plateImg, plate);
                lv_obj_center(s_plateImg);
                lv_obj_move_background(s_plateImg);   // behind every page
            }
        }
        if (!s_ovImg) {
            if (const lv_img_dsc_t *ov = settings_custom_overlay()) {
                s_ovImg = lv_img_create(s_screen);
                lv_img_set_src(s_ovImg, ov);
                lv_obj_center(s_ovImg);
            }
        }
        if (s_ovImg) lv_obj_move_foreground(s_ovImg);   // CRT/glass over everything
    }

    void settings_art_release() {
        if (s_plateImg) { lv_obj_del(s_plateImg); s_plateImg = nullptr; }
        if (s_ovImg)    { lv_obj_del(s_ovImg);    s_ovImg    = nullptr; }
        settings_sprite_release();   // hand the decoded PSRAM back too
    }
}

// Called by app_shell when the shell switches away from Settings. Gives back the text
// canvas and the background art so they are not held while another app needs the PSRAM.
void settingsview::onExit() {
    settings_text::release();
    settings_art_release();
}

void settingsview::onEnter() {
    settings_art_acquire();
    settings_text::acquire();
    s_sel = DEFAULT_SEL;
    show_page(MODE_MENU);
}

void settingsview::onPress() {
    if (s_mode == MODE_FIRSTBOOT) {
        if (s_fbSel == FB_ONDEVICE) {
            // Straight into the scan/pick/type flow that has existed in this file all
            // along and was unreachable at boot for six weeks behind the CUT-03 enum bug.
            s_firstBootPrompt = true;
            start_wifi_scan();
            show_page(MODE_WIFI_LIST);
        } else {
            show_page(MODE_FIRSTBOOT_PHONE);
        }
        return;
    }
    if (s_mode == MODE_NO_SDCARD) {
        // Said once. Where it goes next is whatever the boot would have shown anyway: the
        // WiFi choice if there is also no network, otherwise out to the app switcher.
        if (s_pendingWifiSetup) { s_pendingWifiSetup = false; s_fbSel = FB_ONDEVICE; show_page(MODE_FIRSTBOOT); }
        else { app_shell::setCaptured(false); app_shell::openSwitcher(); }
        return;
    }
    if (s_mode == MODE_FIRSTBOOT_PHONE) {
        s_fbSel = FB_ONDEVICE;      // Back lands on the on-device row, as agreed
        show_page(MODE_FIRSTBOOT);
        return;
    }
    if (s_mode == MODE_MENU) {
        if (s_sel == ITEM_DISPLAY) { s_dspSel = 0; show_page(MODE_DISPLAY); }
        else if (s_sel == ITEM_LOCATION) { s_lmSel = 0; show_page(MODE_LOCATION); }
        else if (s_sel == ITEM_SOUND) { s_sndSel = 0; show_page(MODE_SOUND); }
        else if (s_sel == ITEM_UNITS) { s_unitsSel = 0; show_page(MODE_UNITS); }
        else if (s_sel == ITEM_RANGE) { s_rangeSel = 0; show_page(MODE_RANGE); }
        else if (s_sel == ITEM_WIFI) { diag::log("wifi: enter (open list)"); start_wifi_scan(); show_page(MODE_WIFI_LIST); }
        else if (s_sel == ITEM_DESIGN) { s_designSel = 0; show_page(MODE_DESIGN_SELECT); }
        else if (s_sel == ITEM_ABOUT) { show_page(MODE_ABOUT); }
        else if (s_sel == ITEM_RESET) { show_page(MODE_RESET_CONFIRM); }
        else {                                          // Back -> return to the app switcher
            app_shell::setCaptured(false);
            app_shell::openSwitcher();
        }
    } else if (s_mode == MODE_DISPLAY) {
        if (s_dspSel == DSP_SCREEN) {                   // cycle the screen-dim timeout
            host_set_idle_ms(IDLE_MS[(idle_index() + 1) % IDLE_N]);
            refresh_display();
        } else if (s_dspSel == DSP_BRIGHT) {
            s_bri = host_get_brightness();
            show_page(MODE_BRIGHT);
        } else if (s_dspSel == DSP_THEME) {
            s_themeSel = app_theme::get();
            show_page(MODE_THEME_SELECT);
        } else {                                        // Back -> exit Settings to the app switcher
            app_shell::setCaptured(false);
            app_shell::openSwitcher();
        }
    } else if (s_mode == MODE_BRIGHT) {
        host_set_brightness(s_bri, true);
        app_shell::setCaptured(false);      // back always exits to the switcher, not one level up
        app_shell::openSwitcher();
    } else if (s_mode == MODE_ABOUT) {
        app_shell::setCaptured(false);      // push anywhere on this page exits to the switcher
        app_shell::openSwitcher();
    } else if (s_mode == MODE_RESET_CONFIRM) {
        host_factory_reset();          // wipes + shows its own countdown + reboots; doesn't return
    } else if (s_mode == MODE_WIFI_LIST) {
        if (s_wifiScanning) { /* wait for scan */ }
        else if (s_wifiSel < s_wifiCount) wifi_pick_network(s_wifiSel);
        else if (s_wifiSel == s_wifiCount) start_wifi_scan();          // Rescan
        else { app_shell::setCaptured(false); app_shell::openSwitcher(); }   // Back
    } else if (s_mode == MODE_WIFI_PASSWORD) {
        if (s_wkbIdx < N_WKEYS) {                                       // add a character
            const int L = (int)strlen(s_pass);
            if (L < (int)sizeof(s_pass) - 1) { s_pass[L] = WKEYS[s_wkbIdx]; s_pass[L + 1] = 0; }
            refresh_wifi_pass();
        } else if (s_wkbIdx == WK_DEL) {                               // backspace
            const int L = (int)strlen(s_pass);
            if (L > 0) s_pass[L - 1] = 0;
            refresh_wifi_pass();
        } else {                                                       // OK -> connect
            wifi_begin_connect(s_pass);
        }
    } else if (s_mode == MODE_WIFI_STATUS) {
        if (!s_wifiConnecting) show_page(MODE_WIFI_LIST);   // ignore while actively connecting
    } else if (s_mode == MODE_UNITS) {
        if (s_unitsSel == UNIT_MODE) {
            host_wx_units_set((host_wx_units_mode() + 1) % 3);   // Auto -> Metric -> Imperial -> Auto
            refresh_units();
        } else {                                        // Back -> exit Settings to the app switcher
            app_shell::setCaptured(false);
            app_shell::openSwitcher();
        }
    } else if (s_mode == MODE_RANGE) {
        if (s_rangeSel == RNG_VALUE) {
            // Cycle to the next step up, wrapping at the top. Find where we are by
            // nearest match rather than storing an index, so a range restored from NVS
            // (or set by a Launch Kit push) that is not exactly on a step still lands
            // somewhere sensible instead of jumping to 10 km.
            const float cur = host_get_range_km();
            int best = 0; float bd = 1e9f;
            for (int i = 0; i < RANGE_N; ++i) {
                float d = cur - RANGE_STEPS_KM[i];
                if (d < 0) d = -d;
                if (d < bd) { bd = d; best = i; }
            }
            host_set_range_km(RANGE_STEPS_KM[(best + 1) % RANGE_N]);
            refresh_range();
        } else {                                        // Back -> exit Settings to the app switcher
            app_shell::setCaptured(false);
            app_shell::openSwitcher();
        }
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
        } else if (s_sndSel == SND_CHIME_SEL) {
            s_chimeSel = host_chime_index();
            show_page(MODE_CHIME_SELECT);
            host_chime_preview(s_chimeSel);             // preview the current pick on entry
        } else if (s_sndSel == SND_VOLUME) {
            s_vol = host_get_volume();
            show_page(MODE_VOLUME);
        } else {                                        // Back -> exit Settings to the app switcher
            app_shell::setCaptured(false);
            app_shell::openSwitcher();
        }
    } else if (s_mode == MODE_CHIME_SELECT) {
        if (s_chimeSel < host_chime_count()) host_chime_set(s_chimeSel);   // Back leaves it unchanged
        app_shell::setCaptured(false);      // back always exits to the switcher, not one level up
        app_shell::openSwitcher();
    } else if (s_mode == MODE_THEME_SELECT) {
        if (s_themeSel < APP_THEME_COUNT && s_themeSel != app_theme::get()) {
            show_page(MODE_THEME_NOTICE);
            lv_refr_now(NULL);           // force the notice onto the panel before the blocking reboot below
            app_theme::set(s_themeSel);  // reboots on device (never returns there); applies live + returns on the sim
        }
        app_shell::setCaptured(false);   // back always exits to the switcher, not one level up
        app_shell::openSwitcher();
    } else if (s_mode == MODE_THEME_NOTICE) {
        // transitional page — the device reboots before this could ever fire; only
        // reachable at all on the sim, and only if something presses during that instant
    } else if (s_mode == MODE_DESIGN_SELECT) {
        if (s_designSel < s_designCount && strcmp(s_designSlugs[s_designSel], theme_select::activeSlug()) != 0) {
            show_page(MODE_DESIGN_NOTICE);
            lv_refr_now(NULL);                              // force the notice onto the panel before the blocking reboot below
            theme_select::set(s_designSlugs[s_designSel]);  // reboots on device (never returns there); re-execs on the sim
        }
        app_shell::setCaptured(false);   // back always exits to the switcher, not one level up
        app_shell::openSwitcher();
    } else if (s_mode == MODE_DESIGN_NOTICE) {
        // transitional page — the device reboots before this could ever fire; only
        // reachable at all on the sim, and only if something presses during that instant
    } else if (s_mode == MODE_VOLUME) {
        host_set_volume(s_vol, true);
        host_sound_preview_beep();                      // hear the new level
        app_shell::setCaptured(false);      // back always exits to the switcher, not one level up
        app_shell::openSwitcher();
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
        } else {                                        // Back -> exit Settings to the app switcher
            app_shell::setCaptured(false);
            app_shell::openSwitcher();
        }
    } else if (s_mode == MODE_RECENT) {
        if (s_recCount > 0 && s_recSel < s_recCount)
            host_set_location_named(s_recNames[s_recSel], s_recLat[s_recSel], s_recLon[s_recSel]);
        else {                                              // Back (or empty list) -> app switcher
            app_shell::setCaptured(false);
            app_shell::openSwitcher();
        }
    } else {  // MODE_SEARCH
        const int L = (int)strlen(s_str);
        if (s_kbIdx < 26) {                                 // a letter
            if (L < (int)sizeof(s_str) - 1) { s_str[L] = KEYS[s_kbIdx]; s_str[L + 1] = 0; }
            mark_dirty();
        } else if (s_kbIdx == 26) {                         // backspace (empty -> exit search)
            if (L > 0) { s_str[L - 1] = 0; mark_dirty(); }
            else { app_shell::setCaptured(false); app_shell::openSwitcher(); }   // exit to switcher
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
    if (app_theme::get() == APP_THEME_OFFICE) {
        const AppPalette &p = app_theme::palette();
        C_WHITE = p.ink; C_GREY = p.soft; C_DIM = p.dim; C_ACCENT = p.accent;
        C_BG = p.bg; C_HL = p.highlight; C_TRACK = p.hairline;
    }

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, C_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    // A Launch Kit push's baked background sits right on top of the plain
    // bg_color fill above. Every sub-page below (brightness, WiFi, Display,
    // etc.) is its own transparent container (lv_obj_remove_style_all, no
    // bg_opa of its own) stacked on s_screen, so this one background shows
    // through consistently across all of Settings, not just the main wheel
    // menu the design was authored against.
    // Background art is NOT decoded here any more: see settings_art_acquire(). At 466x466
    // it costs ~636 KB of PSRAM, and holding it from boot (plus the same again for the
    // glass layer) starved screens that were actually on display.

    // --- main menu page ---
    s_menu = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_menu);
    lv_obj_set_size(s_menu, SCREEN_W, SCREEN_H); lv_obj_center(s_menu);
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_SCROLLABLE);

    s_hl = lv_obj_create(s_menu);
    style_highlight(s_hl);

    for (int i = 0; i < ITEM_COUNT; ++i) {
        s_items[i] = lv_label_create(s_menu);
        lv_label_set_text(s_items[i], ITEM_LABELS[i]);
        // Font, opacity, and position are all set dynamically in refresh_menu() —
        // they depend on distance from the current selection (the wheel effect).
    }
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
    lv_obj_set_style_bg_color(track, C_TRACK, 0);
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
    style_highlight(s_lmHl);
    for (int i = 0; i < LM_COUNT; ++i) {
        s_lmItems[i] = lv_label_create(s_lmPage);
        lv_label_set_text(s_lmItems[i], LM_LABELS[i]);
        // Font, opacity, position: wheel_layout(), called from refresh_locmenu().
    }
    lv_obj_t *lmhint = lv_label_create(s_lmPage);
    lv_label_set_text(lmhint, "turn to choose, push to select");
    lv_obj_set_style_text_color(lmhint, C_GREY, 0);
    lv_obj_set_style_text_font(lmhint, &lv_font_montserrat_14, 0);
    lv_obj_align(lmhint, LV_ALIGN_CENTER, 0, 150);

    // --- first boot: which way do you want to give it WiFi? ---
    //
    // Five lines in a 466 px circle. At 90 px above and below centre the chord is still
    // 430 px wide and the inscribed area is about 330 px tall, so this sits inside the
    // budget at readable sizes with room left: nothing here is shrunk to fit.
    s_fbPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_fbPage);
    lv_obj_set_size(s_fbPage, SCREEN_W, SCREEN_H); lv_obj_center(s_fbPage);
    // Opaque black, not a transparent overlay: this is the first thing a stranger sees and
    // it must not have the clock it cannot trust showing through from behind.
    lv_obj_set_style_bg_color(s_fbPage, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_fbPage, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_fbPage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *fbTitle = lv_label_create(s_fbPage);
    lv_label_set_text(fbTitle, "The Orb needs WiFi");
    lv_obj_set_style_text_color(fbTitle, lv_color_white(), 0);
    lv_obj_set_style_text_font(fbTitle, &lv_font_montserrat_20, 0);
    lv_obj_align(fbTitle, LV_ALIGN_CENTER, 0, -122);
    // Load-bearing, not decoration. The S3 has no 5 GHz radio, so a 5 GHz-only network
    // never appears in the scan at all — and before this line the screen offered no reason
    // why, leaving a stranger looking at a list with their own network missing from it.
    lv_obj_t *fbBand = lv_label_create(s_fbPage);
    lv_label_set_text(fbBand, "2.4 GHz only");
    lv_obj_set_style_text_color(fbBand, C_DIM, 0);
    lv_obj_set_style_text_font(fbBand, &lv_font_montserrat_16, 0);
    lv_obj_align(fbBand, LV_ALIGN_CENTER, 0, -92);
    s_fbHl = lv_obj_create(s_fbPage);
    style_highlight(s_fbHl);
    for (int i = 0; i < FB_COUNT; ++i) {
        s_fbItems[i] = lv_label_create(s_fbPage);
        lv_label_set_text(s_fbItems[i], FB_LABELS[i]);
        // Font, opacity, position: wheel_layout(), called from refresh_firstboot().
    }
    // A sixth line, and the one addition to the agreed copy. This is the only screen whose
    // audience has never touched the knob before, so the one place the grammar cannot be
    // assumed. Same wording and position the location menu already uses.
    lv_obj_t *fbHint = lv_label_create(s_fbPage);
    lv_label_set_text(fbHint, "turn to choose, push to select");
    lv_obj_set_style_text_color(fbHint, C_GREY, 0);
    lv_obj_set_style_text_font(fbHint, &lv_font_montserrat_14, 0);
    lv_obj_align(fbHint, LV_ALIGN_CENTER, 0, 150);

    // --- first boot: the phone path ---
    //
    // Instruction only. The access point is already running by the time this screen can be
    // reached: autoConnect() raises "The Orb Setup" before main.cpp selects Settings, so
    // there is nothing to start here and nothing to tear down on the way back.
    s_fbPhonePage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_fbPhonePage);
    lv_obj_set_size(s_fbPhonePage, SCREEN_W, SCREEN_H); lv_obj_center(s_fbPhonePage);
    lv_obj_set_style_bg_color(s_fbPhonePage, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_fbPhonePage, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_fbPhonePage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *fpLead = lv_label_create(s_fbPhonePage);
    lv_label_set_text(fpLead, "On your phone, join");
    lv_obj_set_style_text_color(fpLead, C_DIM, 0);
    lv_obj_set_style_text_font(fpLead, &lv_font_montserrat_16, 0);
    lv_obj_align(fpLead, LV_ALIGN_CENTER, 0, -78);
    // The network name is the one thing on this screen a person has to copy correctly, so
    // it is the one thing set larger than everything around it.
    lv_obj_t *fpSsid = lv_label_create(s_fbPhonePage);
    lv_label_set_text(fpSsid, "The Orb Setup");
    lv_obj_set_style_text_color(fpSsid, lv_color_white(), 0);
    lv_obj_set_style_text_font(fpSsid, &lv_font_montserrat_28, 0);
    lv_obj_align(fpSsid, LV_ALIGN_CENTER, 0, -36);
    lv_obj_t *fpL1 = lv_label_create(s_fbPhonePage);
    lv_label_set_text(fpL1, "A page opens by itself.");
    lv_obj_set_style_text_color(fpL1, lv_color_white(), 0);
    lv_obj_set_style_text_font(fpL1, &lv_font_montserrat_16, 0);
    lv_obj_align(fpL1, LV_ALIGN_CENTER, 0, 24);
    lv_obj_t *fpL2 = lv_label_create(s_fbPhonePage);
    lv_label_set_text(fpL2, "Pick your network there.");
    lv_obj_set_style_text_color(fpL2, lv_color_white(), 0);
    lv_obj_set_style_text_font(fpL2, &lv_font_montserrat_16, 0);
    lv_obj_align(fpL2, LV_ALIGN_CENTER, 0, 52);
    lv_obj_t *fpBack = lv_label_create(s_fbPhonePage);
    lv_label_set_text(fpBack, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_color(fpBack, C_GREY, 0);
    lv_obj_set_style_text_font(fpBack, &lv_font_montserrat_20, 0);
    lv_obj_align(fpBack, LV_ALIGN_CENTER, 0, 120);

    // --- no readable SD card ---
    s_noSdPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_noSdPage);
    lv_obj_set_size(s_noSdPage, SCREEN_W, SCREEN_H); lv_obj_center(s_noSdPage);
    lv_obj_set_style_bg_color(s_noSdPage, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_noSdPage, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_noSdPage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sdTitle = lv_label_create(s_noSdPage);
    // The same voice as "The Orb needs WiFi", on purpose: these are the two things it can
    // be missing, and a person who meets both should not have to learn two tones.
    lv_label_set_text(sdTitle, "The Orb needs\nan SD card");
    lv_obj_set_style_text_color(sdTitle, lv_color_white(), 0);
    lv_obj_set_style_text_font(sdTitle, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(sdTitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sdTitle, LV_ALIGN_CENTER, 0, -60);
    // Why, in one line. UX-006: nothing on screen is unexplained. Without this the notice
    // is a demand with no reason attached, which is the shape of an error code in words.
    lv_obj_t *sdWhy = lv_label_create(s_noSdPage);
    lv_label_set_text(sdWhy, "Designs live on the card.");
    lv_obj_set_style_text_color(sdWhy, C_DIM, 0);
    lv_obj_set_style_text_font(sdWhy, &lv_font_montserrat_16, 0);
    lv_obj_align(sdWhy, LV_ALIGN_CENTER, 0, 16);
    lv_obj_t *sdHow = lv_label_create(s_noSdPage);
    lv_label_set_text(sdHow, "Insert one and restart.");
    lv_obj_set_style_text_color(sdHow, lv_color_white(), 0);
    lv_obj_set_style_text_font(sdHow, &lv_font_montserrat_16, 0);
    lv_obj_align(sdHow, LV_ALIGN_CENTER, 0, 46);
    lv_obj_t *sdHint = lv_label_create(s_noSdPage);
    lv_label_set_text(sdHint, "push to carry on without one");
    lv_obj_set_style_text_color(sdHint, C_GREY, 0);
    lv_obj_set_style_text_font(sdHint, &lv_font_montserrat_14, 0);
    lv_obj_align(sdHint, LV_ALIGN_CENTER, 0, 130);

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

    // --- display menu page (Screen timeout / Brightness / Back) ---
    s_dspPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_dspPage);
    lv_obj_set_size(s_dspPage, SCREEN_W, SCREEN_H); lv_obj_center(s_dspPage);
    lv_obj_clear_flag(s_dspPage, LV_OBJ_FLAG_SCROLLABLE);
    {
        lv_obj_t *dtitle = lv_label_create(s_dspPage);
        lv_label_set_text(dtitle, "Display");
        lv_obj_set_style_text_color(dtitle, C_DIM, 0);
        lv_obj_set_style_text_font(dtitle, &lv_font_montserrat_16, 0);
        lv_obj_align(dtitle, LV_ALIGN_CENTER, 0, -110);
        s_dspHl = lv_obj_create(s_dspPage);
        style_highlight(s_dspHl);
        for (int i = 0; i < DSP_COUNT; ++i) {
            s_dspItems[i] = lv_label_create(s_dspPage);
            lv_label_set_text(s_dspItems[i], "");
            // Font, opacity, position: wheel_layout(), called from refresh_display().
        }
        lv_obj_t *dhint = lv_label_create(s_dspPage);
        lv_label_set_text(dhint, "turn to choose, push to select");
        lv_obj_set_style_text_color(dhint, C_GREY, 0);
        lv_obj_set_style_text_font(dhint, &lv_font_montserrat_14, 0);
        lv_obj_align(dhint, LV_ALIGN_CENTER, 0, 150);
    }

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
    style_highlight(s_sndHl);
    for (int i = 0; i < SND_COUNT; ++i) {
        s_sndItems[i] = lv_label_create(s_sndPage);
        lv_label_set_text(s_sndItems[i], "");
        // Font, opacity, position: wheel_layout(), called from refresh_sound().
    }
    lv_obj_t *sndhint = lv_label_create(s_sndPage);
    lv_label_set_text(sndhint, "turn to choose, push to toggle");
    lv_obj_set_style_text_color(sndhint, C_GREY, 0);
    lv_obj_set_style_text_font(sndhint, &lv_font_montserrat_14, 0);
    lv_obj_align(sndhint, LV_ALIGN_CENTER, 0, 150);

    // --- chime picker page (Sound > Chime sound) ---
    s_chimeSelPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_chimeSelPage);
    lv_obj_set_size(s_chimeSelPage, SCREEN_W, SCREEN_H); lv_obj_center(s_chimeSelPage);
    lv_obj_clear_flag(s_chimeSelPage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *chimetitle = lv_label_create(s_chimeSelPage);
    lv_label_set_text(chimetitle, "Chime sound");
    lv_obj_set_style_text_color(chimetitle, C_DIM, 0);
    lv_obj_set_style_text_font(chimetitle, &lv_font_montserrat_16, 0);
    lv_obj_align(chimetitle, LV_ALIGN_CENTER, 0, -122);
    s_chimeSelHl = lv_obj_create(s_chimeSelPage);
    style_highlight(s_chimeSelHl);
    for (int i = 0; i < CHIME_UI_MAX + 1; ++i) {
        s_chimeSelItems[i] = lv_label_create(s_chimeSelPage);
        lv_label_set_text(s_chimeSelItems[i], "");
        // Font, opacity, position: wheel_layout(), called from refresh_chimeSelect().
    }
    lv_obj_t *chimehint = lv_label_create(s_chimeSelPage);
    lv_label_set_text(chimehint, "turn to preview, push to select");
    lv_obj_set_style_text_color(chimehint, C_GREY, 0);
    lv_obj_set_style_text_font(chimehint, &lv_font_montserrat_14, 0);
    lv_obj_align(chimehint, LV_ALIGN_CENTER, 0, 150);

    // --- theme picker page (Display > Theme) ---
    s_themeSelPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_themeSelPage);
    lv_obj_set_size(s_themeSelPage, SCREEN_W, SCREEN_H); lv_obj_center(s_themeSelPage);
    lv_obj_clear_flag(s_themeSelPage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *themetitle = lv_label_create(s_themeSelPage);
    lv_label_set_text(themetitle, "Theme");
    lv_obj_set_style_text_color(themetitle, C_DIM, 0);
    lv_obj_set_style_text_font(themetitle, &lv_font_montserrat_16, 0);
    lv_obj_align(themetitle, LV_ALIGN_CENTER, 0, -122);
    s_themeSelHl = lv_obj_create(s_themeSelPage);
    style_highlight(s_themeSelHl);
    for (int i = 0; i < APP_THEME_COUNT + 1; ++i) {
        s_themeSelItems[i] = lv_label_create(s_themeSelPage);
        lv_label_set_text(s_themeSelItems[i], "");
        // Font, opacity, position: wheel_layout(), called from refresh_themeSelect().
    }
    lv_obj_t *themehint = lv_label_create(s_themeSelPage);
    lv_label_set_text(themehint, "turn to browse, push to select");
    lv_obj_set_style_text_color(themehint, C_GREY, 0);
    lv_obj_set_style_text_font(themehint, &lv_font_montserrat_14, 0);
    lv_obj_align(themehint, LV_ALIGN_CENTER, 0, 150);

    // --- theme restart notice (Display > Theme > pick one) ---
    // Reuses the picker's own background/ink so it reads as one continuous flow (pick ->
    // notice -> reboot) instead of a jarring color flash right before the screen blanks.
    s_themeNoticePage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_themeNoticePage);
    lv_obj_set_size(s_themeNoticePage, SCREEN_W, SCREEN_H); lv_obj_center(s_themeNoticePage);
    lv_obj_set_style_bg_color(s_themeNoticePage, C_BG, 0);
    lv_obj_set_style_bg_opa(s_themeNoticePage, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_themeNoticePage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *noticeMsg = lv_label_create(s_themeNoticePage);
    lv_label_set_text(noticeMsg, "The Orb will now\nrestart under the\nnew theme.");
    lv_obj_set_style_text_align(noticeMsg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(noticeMsg, C_WHITE, 0);
    lv_obj_set_style_text_font(noticeMsg, &lv_font_montserrat_20, 0);
    lv_obj_center(noticeMsg);

    // --- design picker page (top-level Design item) — same shape as the theme
    // picker above, sized for theme_select::MAX_THEMES installed slugs + Back
    // instead of a fixed APP_THEME_COUNT. Labels are set in refresh_designSelect().
    s_designPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_designPage);
    lv_obj_set_size(s_designPage, SCREEN_W, SCREEN_H); lv_obj_center(s_designPage);
    lv_obj_clear_flag(s_designPage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *designtitle = lv_label_create(s_designPage);
    lv_label_set_text(designtitle, "Theme");
    lv_obj_set_style_text_color(designtitle, C_DIM, 0);
    lv_obj_set_style_text_font(designtitle, &lv_font_montserrat_16, 0);
    lv_obj_align(designtitle, LV_ALIGN_CENTER, 0, -122);
    s_designHl = lv_obj_create(s_designPage);
    style_highlight(s_designHl);
    for (int i = 0; i < theme_select::MAX_THEMES + 1; ++i) {
        s_designItems[i] = lv_label_create(s_designPage);
        lv_label_set_text(s_designItems[i], "");
        // Font, opacity, position: wheel_layout(), called from refresh_designSelect().
    }
    lv_obj_t *designhint = lv_label_create(s_designPage);
    lv_label_set_text(designhint, "turn to browse, push to select");
    lv_obj_set_style_text_color(designhint, C_GREY, 0);
    lv_obj_set_style_text_font(designhint, &lv_font_montserrat_14, 0);
    lv_obj_align(designhint, LV_ALIGN_CENTER, 0, 150);

    // --- design restart notice (Design > pick one) ---
    // Reuses the picker's own background/ink so it reads as one continuous flow (pick ->
    // notice -> reboot) instead of a jarring color flash right before the screen blanks.
    s_designNoticePage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_designNoticePage);
    lv_obj_set_size(s_designNoticePage, SCREEN_W, SCREEN_H); lv_obj_center(s_designNoticePage);
    lv_obj_set_style_bg_color(s_designNoticePage, C_BG, 0);
    lv_obj_set_style_bg_opa(s_designNoticePage, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_designNoticePage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *designNoticeMsg = lv_label_create(s_designNoticePage);
    lv_label_set_text(designNoticeMsg, "The Orb will now\nrestart under the\nnew design.");
    lv_obj_set_style_text_align(designNoticeMsg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(designNoticeMsg, C_WHITE, 0);
    lv_obj_set_style_text_font(designNoticeMsg, &lv_font_montserrat_20, 0);
    lv_obj_center(designNoticeMsg);

    // --- range menu page (Flight Tracker display range cycle / Back) ---
    s_rangePage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_rangePage);
    lv_obj_set_size(s_rangePage, SCREEN_W, SCREEN_H); lv_obj_center(s_rangePage);
    lv_obj_clear_flag(s_rangePage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *rangetitle = lv_label_create(s_rangePage);
    lv_label_set_text(rangetitle, "Range");
    lv_obj_set_style_text_color(rangetitle, C_DIM, 0);
    lv_obj_set_style_text_font(rangetitle, &lv_font_montserrat_16, 0);
    lv_obj_align(rangetitle, LV_ALIGN_CENTER, 0, -122);
    s_rangeHl = lv_obj_create(s_rangePage);
    style_highlight(s_rangeHl);
    for (int i = 0; i < RNG_COUNT; ++i) {
        s_rangeItems[i] = lv_label_create(s_rangePage);
        lv_label_set_text(s_rangeItems[i], "");
        // Font, opacity, position: wheel_layout(), called from refresh_range().
    }
    lv_obj_t *rangehint = lv_label_create(s_rangePage);
    lv_label_set_text(rangehint, "push to cycle how far the scope sees");
    lv_obj_set_style_text_color(rangehint, C_GREY, 0);
    lv_obj_set_style_text_font(rangehint, &lv_font_montserrat_14, 0);
    lv_obj_align(rangehint, LV_ALIGN_CENTER, 0, 122);

    // --- units menu page (Auto/Metric/Imperial cycle / Back) ---
    s_unitsPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_unitsPage);
    lv_obj_set_size(s_unitsPage, SCREEN_W, SCREEN_H); lv_obj_center(s_unitsPage);
    lv_obj_clear_flag(s_unitsPage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *unitstitle = lv_label_create(s_unitsPage);
    lv_label_set_text(unitstitle, "Units");
    lv_obj_set_style_text_color(unitstitle, C_DIM, 0);
    lv_obj_set_style_text_font(unitstitle, &lv_font_montserrat_16, 0);
    lv_obj_align(unitstitle, LV_ALIGN_CENTER, 0, -122);
    s_unitsHl = lv_obj_create(s_unitsPage);
    style_highlight(s_unitsHl);
    for (int i = 0; i < UNIT_COUNT; ++i) {
        s_unitsItems[i] = lv_label_create(s_unitsPage);
        lv_label_set_text(s_unitsItems[i], "");
        // Font, opacity, position: wheel_layout(), called from refresh_units().
    }
    lv_obj_t *unitshint = lv_label_create(s_unitsPage);
    lv_label_set_text(unitshint, "push to cycle Auto / Metric / Imperial");
    lv_obj_set_style_text_color(unitshint, C_GREY, 0);
    lv_obj_set_style_text_font(unitshint, &lv_font_montserrat_14, 0);
    lv_obj_align(unitshint, LV_ALIGN_CENTER, 0, 150);

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
    lv_obj_set_style_bg_color(vtrack, C_TRACK, 0);
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

    // --- About page: the boot splash image, push anywhere to return ---
    s_aboutPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_aboutPage);
    // Explicit opaque backing, same defensive reasoning as s_resetPage below:
    // this page is meant to be a full-screen splash with nothing else visible,
    // unlike every other sub-page (see the comment above s_screen's own bg),
    // which deliberately stay transparent so the shared Settings backdrop
    // shows through. Without this, the persistent "SETTINGS" header label
    // (drawn directly on s_screen, never hidden by show_page()) showed
    // through whenever the splash image didn't land as a perfectly opaque
    // pixel-for-pixel 466x466 cover.
    lv_obj_set_style_bg_color(s_aboutPage, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_aboutPage, LV_OPA_COVER, 0);
    lv_obj_set_size(s_aboutPage, SCREEN_W, SCREEN_H); lv_obj_center(s_aboutPage);
    lv_obj_clear_flag(s_aboutPage, LV_OBJ_FLAG_SCROLLABLE);
    s_aboutImg = lv_img_create(s_aboutPage);
    lv_obj_center(s_aboutImg);

    // The version, the config address and the data credits are drawn by splash_lines on
    // entry, from splash_style.json. They were three labels nailed here at CENTER +120,
    // +152 and +186; the compiled defaults in theme_style::Splash are those same offsets,
    // so a theme that says nothing about them is unchanged.

    // --- Reset confirm: warning + push-to-confirm/turn-to-cancel, same red as the
    // other destructive-action warning (main.cpp's g_holdWarning) ---
    s_resetPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_resetPage);
    lv_obj_set_size(s_resetPage, SCREEN_W, SCREEN_H); lv_obj_center(s_resetPage);
    lv_obj_set_style_bg_color(s_resetPage, lv_color_hex(0x3A0000), 0);
    lv_obj_set_style_bg_opa(s_resetPage, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_resetPage, LV_OBJ_FLAG_SCROLLABLE);
    {
        lv_obj_t *warn = lv_label_create(s_resetPage);
        lv_label_set_text(warn, "This erases Wi-Fi and\nall saved settings.");
        lv_obj_set_style_text_color(warn, lv_color_white(), 0);
        lv_obj_set_style_text_font(warn, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(warn, LV_ALIGN_CENTER, 0, -30);

        lv_obj_t *action = lv_label_create(s_resetPage);
        lv_label_set_text(action, "push to confirm\nturn to cancel");
        lv_obj_set_style_text_color(action, lv_color_hex(0xFFB2B2), 0);
        lv_obj_set_style_text_font(action, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(action, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(action, LV_ALIGN_CENTER, 0, 40);
    }

    // --- WiFi: scrolling network list (same look as the main menu) ---
    s_wifiListPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_wifiListPage);
    lv_obj_set_size(s_wifiListPage, SCREEN_W, SCREEN_H); lv_obj_center(s_wifiListPage);
    lv_obj_clear_flag(s_wifiListPage, LV_OBJ_FLAG_SCROLLABLE);
    {
        lv_obj_t *wtitle = lv_label_create(s_wifiListPage);
        lv_label_set_text(wtitle, "WiFi");
        lv_obj_set_style_text_color(wtitle, C_DIM, 0);
        lv_obj_set_style_text_font(wtitle, &lv_font_montserrat_16, 0);
        lv_obj_align(wtitle, LV_ALIGN_CENTER, 0, -122);   // below the persistent "SETTINGS" header

        s_wifiHl = lv_obj_create(s_wifiListPage);
        lv_obj_remove_style_all(s_wifiHl);
        lv_obj_set_size(s_wifiHl, 320, 40);
        lv_obj_set_style_radius(s_wifiHl, 10, 0);
        lv_obj_set_style_bg_color(s_wifiHl, C_HL, 0);
        lv_obj_set_style_bg_opa(s_wifiHl, LV_OPA_COVER, 0);
        for (int r = 0; r < WIFI_VISIBLE; ++r) {
            s_wifiRows[r] = lv_label_create(s_wifiListPage);
            lv_label_set_text(s_wifiRows[r], "");
            lv_label_set_long_mode(s_wifiRows[r], LV_LABEL_LONG_DOT);
            lv_obj_set_width(s_wifiRows[r], 300);
            lv_obj_set_style_text_align(s_wifiRows[r], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(s_wifiRows[r], &lv_font_montserrat_18, 0);
            lv_obj_align(s_wifiRows[r], LV_ALIGN_CENTER, 0, -(WIFI_VISIBLE - 1) * WIFI_ROW_DY / 2 + r * WIFI_ROW_DY);
        }
        s_wifiListHint = lv_label_create(s_wifiListPage);
        lv_label_set_text(s_wifiListHint, "");
        lv_obj_set_style_text_color(s_wifiListHint, C_GREY, 0);
        lv_obj_set_style_text_font(s_wifiListHint, &lv_font_montserrat_14, 0);
        lv_obj_align(s_wifiListHint, LV_ALIGN_CENTER, 0, 150);
    }

    // --- WiFi: password entry (character strip, same as the city search) ---
    s_wifiPassPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_wifiPassPage);
    lv_obj_set_size(s_wifiPassPage, SCREEN_W, SCREEN_H); lv_obj_center(s_wifiPassPage);
    lv_obj_clear_flag(s_wifiPassPage, LV_OBJ_FLAG_SCROLLABLE);
    {
        s_wifiPassTitle = lv_label_create(s_wifiPassPage);
        lv_label_set_text(s_wifiPassTitle, "Password");
        lv_label_set_long_mode(s_wifiPassTitle, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_wifiPassTitle, 300);
        lv_obj_set_style_text_color(s_wifiPassTitle, C_DIM, 0);
        lv_obj_set_style_text_font(s_wifiPassTitle, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(s_wifiPassTitle, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_wifiPassTitle, LV_ALIGN_CENTER, 0, -118);   // below the "SETTINGS" header

        s_passText = lv_label_create(s_wifiPassPage);
        lv_label_set_text(s_passText, "(enter password)");
        lv_label_set_long_mode(s_passText, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_passText, 320);
        lv_obj_set_style_text_align(s_passText, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_passText, C_WHITE, 0);
        lv_obj_set_style_text_font(s_passText, &lv_font_montserrat_20, 0);
        lv_obj_align(s_passText, LV_ALIGN_CENTER, 0, -70);

        for (int k = 0; k < 7; ++k) {
            s_wkStrip[k] = lv_label_create(s_wifiPassPage);
            lv_label_set_text(s_wkStrip[k], "");
            lv_obj_set_style_text_color(s_wkStrip[k], C_GREY, 0);
            lv_obj_set_style_text_font(s_wkStrip[k], &lv_font_montserrat_18, 0);
            lv_obj_align(s_wkStrip[k], LV_ALIGN_CENTER, (k - 3) * 48, 20);
        }
        s_wifiPassHint = lv_label_create(s_wifiPassPage);
        lv_label_set_text(s_wifiPassHint, "turn to a key, push to enter it\nscroll to OK to connect");
        lv_obj_set_style_text_align(s_wifiPassHint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_wifiPassHint, C_GREY, 0);
        lv_obj_set_style_text_font(s_wifiPassHint, &lv_font_montserrat_14, 0);
        lv_obj_align(s_wifiPassHint, LV_ALIGN_CENTER, 0, 120);
    }

    // --- WiFi: connect status page ---
    s_wifiStatusPage = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_wifiStatusPage);
    lv_obj_set_size(s_wifiStatusPage, SCREEN_W, SCREEN_H); lv_obj_center(s_wifiStatusPage);
    lv_obj_clear_flag(s_wifiStatusPage, LV_OBJ_FLAG_SCROLLABLE);
    {
        s_wifiStatusLbl = lv_label_create(s_wifiStatusPage);
        lv_label_set_text(s_wifiStatusLbl, "");
        lv_obj_set_style_text_color(s_wifiStatusLbl, C_WHITE, 0);
        lv_obj_set_style_text_font(s_wifiStatusLbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(s_wifiStatusLbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_wifiStatusLbl, LV_ALIGN_CENTER, 0, -20);

        s_wifiStatusHint = lv_label_create(s_wifiStatusPage);
        lv_label_set_text(s_wifiStatusHint, "");
        lv_obj_set_style_text_color(s_wifiStatusHint, C_GREY, 0);
        lv_obj_set_style_text_font(s_wifiStatusHint, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(s_wifiStatusHint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_wifiStatusHint, LV_ALIGN_CENTER, 0, 90);
    }

    // First boot: seed the recents list so "Recent cities" starts populated.
    {
        char   tn[RECENTS_MAX][40];
        double tla[RECENTS_MAX], tlo[RECENTS_MAX];
        if (host_recents_get(tn, tla, tlo, RECENTS_MAX) == 0)
            for (int i = SEED_COUNT - 1; i >= 0; --i)      // reversed so SEED_CITIES[0] lands on top
                host_recents_add(SEED_CITIES[i].name, SEED_CITIES[i].lat, SEED_CITIES[i].lon);
    }

    // The shared wheel-text canvas — created after every sub-page (so it's the
    // topmost child, on top of whichever page's own now-invisible labels) but
    // before the CRT/glass overlay below, so scanlines/glass still sit over
    // the text like a real screen, matching every other custom compositor's
    // background -> content -> overlay order.
    settings_text::init(s_screen);

    // CRT + glass on top of everything — created last, after every sub-page
    // above, so it's the topmost child of s_screen regardless of which page is
    // currently shown (same background -> content -> overlay order the other
    // custom compositors use).
    // Glass likewise built on entry, not at boot (see settings_art_acquire()).

    s_bri = host_get_brightness();
    show_page(MODE_MENU);
    lv_timer_create(search_tick, 200, nullptr);
    lv_timer_create(wifi_tick, 300, nullptr);
}

lv_obj_t *settingsview::screen() { return s_screen; }

// Called once from main.cpp's setup() when the device booted with the "needs WiFi
// setup" flag set (fresh out of the box, or just after a Reset) — jumps straight past
// the main menu into the WiFi list with a first-run prompt instead of the usual hint.
// UX-024. `alsoNeedsWifi` chains the two notices rather than letting one hide the other: a
// device out of the box with neither card nor network is missing two things and should say
// both, in the order they have to be fixed. The card comes first because it is the one that
// needs somebody to go and find a physical object.
void settingsview::openNoSdCardNotice(bool alsoNeedsWifi) {
    s_pendingWifiSetup = alsoNeedsWifi;
    s_sel = ITEM_WIFI;
    show_page(MODE_NO_SDCARD);
}

void settingsview::openWifiSetupPrompt() {
    // The choice screen first, not the scan. Both paths lead somewhere that works, and the
    // on-device one is pre-selected so the default is still a single press — see the
    // FB_LABELS block above for why this screen is allowed to ask at all.
    //
    // s_firstBootPrompt is set when the on-device row is taken rather than here, because it
    // is what swaps the WiFi list's hint text and the list is no longer where this lands.
    s_sel   = ITEM_WIFI;
    s_fbSel = FB_ONDEVICE;
    show_page(MODE_FIRSTBOOT);
}

// Called once from main.cpp's setup() right after a Launch Kit push leaves a
// custom splash active — jumps straight to the About page, which holds the same
// splash art up indefinitely (push the knob to leave) instead of the normal boot
// splash's 2s-then-fade, so a just-pushed design stays on screen to look at.
void settingsview::openAboutPage() {
    s_sel = ITEM_ABOUT;
    show_page(MODE_ABOUT);
}

// Called from the host's status loop. Stored rather than drawn immediately: the About
// page is usually hidden, and the loop runs far more often than anyone opens it.
void settingsview::setNetInfo(const char *line) {
    if (!line) return;
    snprintf(s_netInfo, sizeof(s_netInfo), "%s", line);
    if (s_mode == MODE_ABOUT) splash_lines::setNetwork(s_netInfo);
}
