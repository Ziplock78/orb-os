#include "theme_select.h"
#include "theme_style.h"
#include <string.h>
#include <strings.h>   // strcasecmp — the theme list sorts on display names
#include <ctype.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include "sdcard.h"
#else
#include <cstdio>
#include <string>
#include <dirent.h>
#include <unistd.h>   // rmdir, for the simulator's delete path
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
    // Nothing chosen, but themes on the card: wear the first one.
    //
    // A factory-fresh Orb has an erased NVS, so there is no stored slug, and it fell back
    // to the compiled-in Stock face — a black dial wearing whichever hands the last
    // firmware push happened to bake in. Someone who has just flashed a board and put a
    // card in it sees none of the themes actually sitting on that card, and nothing says
    // why. Picking one is strictly better than pretending there are none.
    //
    // Only ever when the slug is empty. Choosing anything, here or in Settings, writes it,
    // so this cannot override a real choice — including a deliberate return to Stock, which
    // is reached by deleting the themes rather than by clearing the pointer.
    if (!s_slug[0]) {
        static char slugs[MAX_THEMES][MAX_SLUG_LEN];
        const int n = listInstalled(slugs);
        if (n > 0) {
            strncpy(s_slug, slugs[0], sizeof(s_slug) - 1);
            s_slug[sizeof(s_slug) - 1] = 0;
#ifdef ARDUINO
            Preferences w;
            w.begin("capsuleradar", false);
            w.putString("themeSlug", s_slug);
            w.end();
            Serial.printf("[theme] nothing chosen; wearing '%s' from the card\n", s_slug);
#endif
        }
    }

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

    // Sorted here, once, so every caller inherits it rather than each one sorting for
    // itself — five of them read this list. Rule six: the shared path, not a note beside
    // one caller.
    //
    // Before this there was no order at all. Both branches above append whatever the
    // directory hands back, which is FAT slot order: roughly creation order, except that
    // deleting a theme leaves a hole the next install reuses, so the list silently
    // rearranges itself when somebody removes one and adds another. Zion read his as
    // steampunk, modern, Cold War, Zwaa, aviator and asked what decided it. Nothing did.
    //
    // By DISPLAY NAME, not by slug, and that is the part worth getting right. The theme
    // shown as "Modern" lives in a folder called "the-office", so sorting on slugs files it
    // under T and produces an order that looks random to the person reading the screen. The
    // collision comment in settings_view.cpp already warns about that exact confusion.
    //
    // Case-insensitive, so "Zwaa" and "aviator" do not end up in separate alphabets.
    //
    // Costs one labelFor() per theme, which reads that theme's theme.json. The pages that
    // call this already rescan the card to build the list, so it is a handful of small
    // reads on a screen that was doing file I/O anyway, at most MAX_THEMES of them.
    {
        char label[MAX_THEMES][32];
        for (int i = 0; i < n; ++i) theme_style::labelFor(out[i], label[i], sizeof(label[i]));
        // Insertion sort: n is at most MAX_THEMES and this runs once per screen entry, so
        // the simple thing that keeps the two arrays in step is the right thing.
        for (int i = 1; i < n; ++i) {
            char keySlug[MAX_SLUG_LEN]; char keyLabel[32];
            strncpy(keySlug, out[i], sizeof(keySlug));   keySlug[sizeof(keySlug) - 1] = 0;
            strncpy(keyLabel, label[i], sizeof(keyLabel)); keyLabel[sizeof(keyLabel) - 1] = 0;
            int j = i - 1;
            while (j >= 0 && strcasecmp(label[j], keyLabel) > 0) {
                strncpy(out[j + 1], out[j], MAX_SLUG_LEN);   out[j + 1][MAX_SLUG_LEN - 1] = 0;
                strncpy(label[j + 1], label[j], sizeof(label[j])); label[j + 1][sizeof(label[j]) - 1] = 0;
                --j;
            }
            strncpy(out[j + 1], keySlug, MAX_SLUG_LEN);   out[j + 1][MAX_SLUG_LEN - 1] = 0;
            strncpy(label[j + 1], keyLabel, sizeof(label[j + 1])); label[j + 1][sizeof(label[j + 1]) - 1] = 0;
        }
    }
    return n;
}

bool removeInstalled(const char *slug) {
    if (!slug || !slug[0]) return false;
    // Never the one being worn. Every screen is drawing from that folder's art and reading
    // its style files, and the flash cache is keyed to it: pulling it out from under them
    // leaves a device wearing a theme that no longer exists. Switch first.
    if (!strcmp(slug, s_slug)) return false;

    // Safe characters only, which is what keeps this away from the rest of the card: no
    // slash, no dot, so no path can be built that climbs out of /themes.
    //
    // Deliberately NOT "must appear in listInstalled". That list only counts folders
    // carrying the _installed sentinel, and an install that dies halfway leaves a folder
    // without one on purpose, so it cannot masquerade as a working theme. Requiring
    // membership here made those folders invisible AND permanently undeletable, which is a
    // worse place to leave someone than the problem the sentinel was solving.
    for (const char *p = slug; *p; ++p)
        if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && *p != '-') return false;
    if (strlen(slug) >= MAX_SLUG_LEN) return false;

#ifdef ARDUINO
    if (!sdcard::mounted()) return false;
    char dirPath[80];
    snprintf(dirPath, sizeof(dirPath), "/themes/%s", slug);
    // Still say no to a folder that was never there, so "no such theme" keeps meaning what
    // it says rather than becoming the answer to every delete.
    if (!SD.exists(dirPath)) return false;

    char path[96];
    // The sentinel first: from this moment the folder is no longer a theme, whatever else
    // happens below.
    snprintf(path, sizeof(path), "/themes/%s/_installed", slug);
    SD.remove(path);

    File dir = SD.open(dirPath);
    if (dir && dir.isDirectory()) {
        // Collect then delete. Removing entries while walking the same open directory
        // handle is where SD libraries differ from each other, and a half-walked delete is
        // exactly the state worth not inventing.
        static char names[64][MAX_SLUG_LEN + 24];
        int count = 0;
        File f = dir.openNextFile();
        while (f && count < (int)(sizeof(names) / sizeof(names[0]))) {
            if (!f.isDirectory()) {
                const char *nm = f.name();
                const char *leaf = strrchr(nm, '/');
                leaf = leaf ? leaf + 1 : nm;
                strncpy(names[count], leaf, sizeof(names[0]) - 1);
                names[count][sizeof(names[0]) - 1] = 0;
                ++count;
            }
            f.close();
            f = dir.openNextFile();
        }
        dir.close();
        for (int i = 0; i < count; ++i) {
            snprintf(path, sizeof(path), "/themes/%s/%s", slug, names[i]);
            SD.remove(path);
        }
    } else if (dir) {
        dir.close();
    }
    SD.rmdir(dirPath);
    Serial.printf("[theme_select] deleted '%s' from the card\n", slug);
    return true;
#else
    const std::string dirPath = std::string(SIM_SD_ROOT) + "/themes/" + slug;
    if (DIR *probe = opendir(dirPath.c_str())) { closedir(probe); } else { return false; }
    ::remove((dirPath + "/_installed").c_str());
    if (DIR *d = opendir(dirPath.c_str())) {
        struct dirent *e;
        while ((e = readdir(d)) != nullptr) {
            if (e->d_name[0] == '.') continue;
            ::remove((dirPath + "/" + e->d_name).c_str());
        }
        closedir(d);
    }
    ::rmdir(dirPath.c_str());
    printf("[theme_select] deleted '%s' from the card\n", slug);
    return true;
#endif
}

} // namespace theme_select
