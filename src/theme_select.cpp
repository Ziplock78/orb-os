#include "theme_select.h"
#include "theme_style.h"
#include <string.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include "sdcard.h"
#else
#include <cstdio>
#include <string>
#include <dirent.h>
#endif

namespace {

char  s_slug[theme_select::MAX_SLUG_LEN] = "";
void (*s_restartHook)() = nullptr;

#ifndef ARDUINO
// Same reasoning as app_theme.cpp's NATIVE_THEME_FILE: a re-exec starts a fresh
// process, so the chosen slug has to survive it in a file, not just in RAM.
const char *NATIVE_SLUG_FILE = "/tmp/orb_sim_theme_slug";
const char *SIM_SD_ROOT      = "sim/sdcard";   // same stand-in root as theme_sd/roads_sd
#endif

} // namespace

namespace theme_select {

void init() {
#ifdef ARDUINO
    Preferences p;
    p.begin("capsuleradar", true);   // read-only
    String s = p.getString("themeSlug", "");
    p.end();
    strncpy(s_slug, s.c_str(), sizeof(s_slug) - 1);
    s_slug[sizeof(s_slug) - 1] = 0;
#else
    if (FILE *f = fopen(NATIVE_SLUG_FILE, "r")) {
        if (fgets(s_slug, sizeof(s_slug), f)) {
            const size_t n = strlen(s_slug);
            if (n && s_slug[n - 1] == '\n') s_slug[n - 1] = 0;
        }
        fclose(f);
    }
#endif
    // Every screen's own style (colors/positions/formats/geometry — see theme_style.h
    // for exactly what's covered) travels on the SD card per theme, same as the art.
    // set() always reboots/re-execs, so re-running this at boot is the only reload
    // point that's ever needed.
    theme_style::load();
}

const char *activeSlug() { return s_slug; }

void set(const char *slug) {
    if (!slug) slug = "";
#ifdef ARDUINO
    Preferences p;
    p.begin("capsuleradar", false);
    p.putString("themeSlug", slug);
    p.end();
    delay(700);       // hold the "restarting..." notice on screen long enough to actually read
    ESP.restart();    // reboot into the new theme — see theme_select.h
#else
    strncpy(s_slug, slug, sizeof(s_slug) - 1);
    s_slug[sizeof(s_slug) - 1] = 0;
    if (FILE *f = fopen(NATIVE_SLUG_FILE, "w")) { fprintf(f, "%s\n", s_slug); fclose(f); }
    if (s_restartHook) s_restartHook();   // sim_main.cpp's sim_restart() — an actual re-exec, same as hardware's reboot
#endif
}

void setRestartHook(void (*hook)()) { s_restartHook = hook; }

int listInstalled(char out[][MAX_SLUG_LEN]) {
    int n = 0;
#ifdef ARDUINO
    if (!sdcard::mounted()) return 0;
    File dir = SD.open("/themes");
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return 0; }
    File f = dir.openNextFile();
    while (f && n < MAX_THEMES) {
        if (f.isDirectory()) {
            // Some SD library versions return the full path from name(), others
            // just the leaf — take whatever's after the last '/' either way.
            const char *name = f.name();
            const char *leaf = strrchr(name, '/');
            leaf = leaf ? leaf + 1 : name;
            if (leaf[0] && leaf[0] != '.') {
                // Only a theme Launch Kit's whole-theme "Launch" flow actually
                // finished pushing counts — marked by this sentinel, written
                // only on a full, successful push (see prepareThemePush,
                // server-side). A folder that only has, say, a clock preview in
                // it (pushed from that one screen's own editor, which never
                // writes this file) would otherwise show up here too, and
                // switching to it shows broken/stale art on every screen that
                // was never actually part of a real launch.
                char markerPath[80];
                snprintf(markerPath, sizeof(markerPath), "/themes/%s/_installed", leaf);
                if (SD.exists(markerPath)) {
                    strncpy(out[n], leaf, MAX_SLUG_LEN - 1);
                    out[n][MAX_SLUG_LEN - 1] = 0;
                    n++;
                }
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
#else
    DIR *d = opendir((std::string(SIM_SD_ROOT) + "/themes").c_str());
    if (!d) return 0;
    struct dirent *e;
    while (n < MAX_THEMES && (e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;   // skip ".", "..", dotfiles
#ifdef DT_DIR
        if (e->d_type != DT_DIR && e->d_type != DT_UNKNOWN) continue;
#endif
        const std::string markerPath = std::string(SIM_SD_ROOT) + "/themes/" + e->d_name + "/_installed";
        if (FILE *mf = fopen(markerPath.c_str(), "r")) {
            fclose(mf);
            strncpy(out[n], e->d_name, MAX_SLUG_LEN - 1);
            out[n][MAX_SLUG_LEN - 1] = 0;
            n++;
        }
    }
    closedir(d);
#endif
    return n;
}

} // namespace theme_select
