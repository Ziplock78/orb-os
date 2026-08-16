#include "app_theme.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#else
#include <cstdio>
#endif

namespace {

const AppPalette PALETTES[APP_THEME_COUNT] = {
    // APP_THEME_DEFAULT — the existing night-vision HUD look (matches ui.cpp's
    // own UI_GREEN/UI_PANEL etc.; kept here too so every screen can read one API).
    {
        /*bg*/        lv_color_hex(0x000000),
        /*panel*/     lv_color_hex(0x0C160F),
        /*highlight*/ lv_color_hex(0x232A36),
        /*ink*/       lv_color_hex(0xEAFFF3),
        /*soft*/      lv_color_hex(0x9AFFC8),
        /*dim*/       lv_color_hex(0x5F7A6C),
        /*accent*/    lv_color_hex(0x1DFF86),
        /*hairline*/  lv_color_hex(0x1C2620),
        /*onAccent*/  lv_color_hex(0x05100A),
    },
    // APP_THEME_OFFICE — modern/light: white background, charcoal ink, blue accent.
    {
        /*bg*/        lv_color_hex(0xFFFFFF),
        /*panel*/     lv_color_hex(0xF4F5F7),
        /*highlight*/ lv_color_hex(0x3B5BFF),
        /*ink*/       lv_color_hex(0x1C1C1E),
        /*soft*/      lv_color_hex(0x6E6E73),
        /*dim*/       lv_color_hex(0xA0A0A6),
        /*accent*/    lv_color_hex(0x3B5BFF),
        /*hairline*/  lv_color_hex(0xE3E3E6),
        /*onAccent*/  lv_color_hex(0x1C1C1E),   // the reference shows dark text on the blue selection pill, not white
    },
};

const char *NAMES[APP_THEME_COUNT] = { "Default", "Office" };

int s_theme = APP_THEME_DEFAULT;
void (*s_restartHook)() = nullptr;

#ifndef ARDUINO
// Native has no NVS. sim_restart() (sim_main.cpp) re-execs the whole process — a fresh
// process means a fresh, default-initialized s_theme — so without this the sim would
// always snap back to Default the instant it "reboots" into the very theme you just
// picked. This file is the one thing that survives across that re-exec.
const char *NATIVE_THEME_FILE = "/tmp/orb_sim_theme";
#endif

} // namespace

namespace app_theme {

void init() {
#ifdef ARDUINO
    Preferences p;
    p.begin("capsuleradar", true);   // read-only
    s_theme = p.getInt("appTheme", APP_THEME_DEFAULT);
    p.end();
#else
    if (FILE *f = fopen(NATIVE_THEME_FILE, "r")) {
        if (fscanf(f, "%d", &s_theme) != 1) s_theme = APP_THEME_DEFAULT;
        fclose(f);
    }
#endif
    if (s_theme < 0 || s_theme >= APP_THEME_COUNT) s_theme = APP_THEME_DEFAULT;
}

int get() { return s_theme; }

const char *name(int t) {
    return (t >= 0 && t < APP_THEME_COUNT) ? NAMES[t] : "";
}

const AppPalette &palette() {
    return PALETTES[s_theme];
}

void set(int t) {
    if (t < 0 || t >= APP_THEME_COUNT) return;
#ifdef ARDUINO
    Preferences p;
    p.begin("capsuleradar", false);
    p.putInt("appTheme", t);
    p.end();
    delay(700);       // hold the "restarting..." notice on screen long enough to actually read
    ESP.restart();    // reboot into the new theme — see app_theme.h
#else
    s_theme = t;
    if (FILE *f = fopen(NATIVE_THEME_FILE, "w")) { fprintf(f, "%d\n", t); fclose(f); }
    if (s_restartHook) s_restartHook();   // sim_main.cpp's sim_restart() — an actual re-exec, same as hardware's reboot
#endif
}

void setRestartHook(void (*hook)()) { s_restartHook = hook; }

} // namespace app_theme
