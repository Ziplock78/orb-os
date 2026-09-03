#include "chime_library.h"

#include "audio.h"
#include "theme_sd.h"
#include "theme_select.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <SD.h>
#endif

#include <stdio.h>
#include <string.h>

namespace {

// No ceiling at all any more, and that is the point. Chimes are STREAMED off the card rather
// than loaded, so the only thing a length costs is the seconds it takes to play. The limit
// existed because the whole file was held in memory, and on a chip where the clock alone takes
// two and a half megabytes there is no length that is both generous and safe. Somebody wanting
// a cuckoo clock, or a full Westminster with twelve strikes, can have one.

struct Entry {
    char slug[theme_select::MAX_SLUG_LEN];   // "" for a built-in
    char name[40];
    int  builtin;                            // index into the flash library, or -1
};

Entry  s_list[theme_select::MAX_THEMES + 4];
int    s_count = 0;
int    s_sel   = 0;

#ifdef ARDUINO
constexpr const char *NVS_NS  = "capsuleradar";
// The identity, not the index. "b:<n>" is a built-in, "t:<slug>" is a theme's.
constexpr const char *NVS_KEY = "chimeSel";
#endif

// A theme's display name, out of its own theme.json. Falls back to the slug, which is ugly but
// never wrong; a picker listing "modern-2th5" is worse than one listing "Modern" and far
// better than one listing nothing.
void theme_name(const char *slug, char *out, size_t outLen) {
    snprintf(out, outLen, "%s", slug);
#ifdef ARDUINO
    char path[64];
    snprintf(path, sizeof(path), "/themes/%s/theme.json", slug);
    File f = SD.open(path, FILE_READ);
    if (!f) return;
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return;
    const char *n = doc["name"].as<const char *>();
    if (n && *n) snprintf(out, outLen, "%s", n);
#endif
}

// Does this theme ship a chime? Read from its own manifest rather than by probing for the
// file, for the reason every other loader on this device does: a push never deletes from the
// card, so a chime.pcm from an older push of the same theme outlives the design that wanted
// it, and offering it would be listing a sound the theme has already dropped.
bool theme_has_chime(const char *slug) {
#ifdef ARDUINO
    char path[64];
    snprintf(path, sizeof(path), "/themes/%s/theme.json", slug);
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) return false;
    for (JsonVariantConst v : doc["assets"].as<JsonArrayConst>()) {
        const char *a = v.as<const char *>();
        if (a && !strcmp(a, "chime.pcm")) return true;
    }
#else
    (void)slug;
#endif
    return false;
}

// Nothing to load. A built-in needs its index handed to the flash player; a theme's is read
// off the card at the moment it rings.
void load_selected() {
    if (s_sel < 0 || s_sel >= s_count) return;
    const Entry &e = s_list[s_sel];
    if (e.builtin >= 0) audio_set_chime(e.builtin);
}

void path_for(const Entry &e, char *out, size_t outLen) {
    snprintf(out, outLen, "/themes/%s/chime.pcm", e.slug);
}

void save() {
#ifdef ARDUINO
    if (s_sel < 0 || s_sel >= s_count) return;
    char id[48];
    const Entry &e = s_list[s_sel];
    if (e.builtin >= 0) snprintf(id, sizeof(id), "b:%d", e.builtin);
    else                snprintf(id, sizeof(id), "t:%s", e.slug);
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    p.putString(NVS_KEY, id);
    p.end();
#endif
}

// Find the stored identity in the list that exists NOW. A theme that has been deleted since is
// simply not there, and the first built-in stands in rather than silence on the hour.
int restore_index() {
#ifdef ARDUINO
    Preferences p;
    if (!p.begin(NVS_NS, true)) return 0;
    String id = p.getString(NVS_KEY, "");
    p.end();
    if (id.length() == 0) return 0;
    if (id.startsWith("b:")) {
        const int n = id.substring(2).toInt();
        for (int i = 0; i < s_count; ++i) if (s_list[i].builtin == n) return i;
    } else if (id.startsWith("t:")) {
        const char *want = id.c_str() + 2;
        for (int i = 0; i < s_count; ++i) if (!strcmp(s_list[i].slug, want)) return i;
    }
    Serial.printf("[chime] saved choice \"%s\" is not installed; falling back\n", id.c_str());
#endif
    return 0;
}

void build() {
    s_count = 0;
    // Flash first, so the device always has something to ring with even on a card that holds
    // no themes at all.
    const int builtins = audio_chime_count();
    for (int i = 0; i < builtins && s_count < (int)(sizeof(s_list) / sizeof(s_list[0])); ++i) {
        Entry &e = s_list[s_count++];
        e.slug[0] = '\0';
        e.builtin = i;
        snprintf(e.name, sizeof(e.name), "%s", audio_chime_name(i));
    }
    char slugs[theme_select::MAX_THEMES][theme_select::MAX_SLUG_LEN];
    const int n = theme_select::listInstalled(slugs);
    for (int i = 0; i < n && s_count < (int)(sizeof(s_list) / sizeof(s_list[0])); ++i) {
        if (!theme_has_chime(slugs[i])) continue;
        Entry &e = s_list[s_count++];
        snprintf(e.slug, sizeof(e.slug), "%s", slugs[i]);
        e.builtin = -1;
        theme_name(slugs[i], e.name, sizeof(e.name));
    }
}

}  // namespace

void chime_library::begin() {
    build();
    s_sel = restore_index();
    load_selected();
#ifdef ARDUINO
    Serial.printf("[chime] %d available, playing \"%s\"\n", s_count, name(s_sel));
#endif
}

void chime_library::rescan() {
    // The identity is re-found in the rebuilt list, so installing an unrelated theme cannot
    // silently move somebody's chime to a different one.
    build();
    s_sel = restore_index();
    load_selected();
}

int chime_library::count() { return s_count; }

const char *chime_library::name(int idx) {
    if (idx < 0 || idx >= s_count) return "";
    return s_list[idx].name;
}

int chime_library::selected() { return s_sel; }

void chime_library::select(int idx) {
    if (idx < 0 || idx >= s_count) return;
    s_sel = idx;
    save();
    load_selected();
}

void chime_library::preview(int idx) {
    if (idx < 0 || idx >= s_count) return;
    const Entry &e = s_list[idx];
    if (e.builtin >= 0) { audio_preview_chime(e.builtin); return; }
    // Streamed, so scrolling the picker costs nothing but the reads, and there is no buffer
    // to keep alive or free under a task that might still be reading it.
    char path[64];
    path_for(e, path, sizeof(path));
    audio_play_file(path, true);
}

void chime_library::playSelected() {
    if (s_sel < 0 || s_sel >= s_count) return;
    const Entry &e = s_list[s_sel];
    if (e.builtin >= 0) { audio_play(AUDIO_CHIME); return; }
    char path[64];
    path_for(e, path, sizeof(path));
    audio_play_file(path, false);
}
