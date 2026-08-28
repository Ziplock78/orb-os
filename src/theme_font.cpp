#include "theme_font.h"
#include "theme_art.h"
#include "theme_select.h"
#include "custom_text.h"
#include "custom_menu.h"
#include "custom_settings.h"
#include "custom_radar.h"
#include <string.h>
#include <stdio.h>      // snprintf — not pulled in by Arduino.h on the desktop build
#include <new>          // std::nothrow — a failed handle alloc must not throw into LVGL
#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace theme_font {
namespace {

// ---- lv_fs driver over the memory-mapped themeart partition -----------------
//
// LVGL's font loader reads through lv_fs, so the cheapest way to hand it a baked font is
// to present the flash partition as a read-only "drive". Every file is already a
// contiguous span of mapped flash, so a "read" is a memcpy from a pointer and a "seek" is
// arithmetic: no card access, no buffering, no allocation for the file itself.
//
// Drive letter 'T' for theme. Registered once in begin().
constexpr char DRIVE_LETTER = 'T';

struct Handle {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
};

void *fs_open(lv_fs_drv_t *, const char *path, lv_fs_mode_t mode) {
    if (mode != LV_FS_MODE_RD) return nullptr;      // read-only by design
    const uint8_t *data = nullptr;
    size_t len = 0;
    if (!theme_art::find_blob(theme_select::activeSlug(), path, data, len)) return nullptr;
    Handle *h = new (std::nothrow) Handle{ data, len, 0 };
    return h;
}
lv_fs_res_t fs_close(lv_fs_drv_t *, void *fp) { delete (Handle *)fp; return LV_FS_RES_OK; }
lv_fs_res_t fs_read(lv_fs_drv_t *, void *fp, void *buf, uint32_t btr, uint32_t *br) {
    Handle *h = (Handle *)fp;
    const size_t avail = (h->pos < h->len) ? (h->len - h->pos) : 0;
    const size_t n = btr < avail ? btr : avail;
    memcpy(buf, h->data + h->pos, n);
    h->pos += n;
    // LVGL's font loader calls lv_fs_read() with a NULL byte-count on its fast path
    // (lv_font_loader.c:444). The wrapper substitutes its own, but a driver that
    // dereferences blindly is one refactor away from a null write, and that is exactly
    // the failure mode that just cost a boot loop.
    if (br) *br = (uint32_t)n;
    return LV_FS_RES_OK;
}
lv_fs_res_t fs_seek(lv_fs_drv_t *, void *fp, uint32_t pos, lv_fs_whence_t whence) {
    Handle *h = (Handle *)fp;
    size_t base = (whence == LV_FS_SEEK_CUR) ? h->pos : (whence == LV_FS_SEEK_END) ? h->len : 0;
    h->pos = base + pos;
    if (h->pos > h->len) h->pos = h->len;
    return LV_FS_RES_OK;
}
lv_fs_res_t fs_tell(lv_fs_drv_t *, void *fp, uint32_t *pos) {
    *pos = (uint32_t)((Handle *)fp)->pos;
    return LV_FS_RES_OK;
}

lv_fs_drv_t s_drv;
bool        s_ready = false;
int         s_loaded = 0;

// One slot per place a theme can style text. Order matches the accessors below.
enum Slot { S_CLOCK1, S_CLOCK2, S_MENU_CUR, S_MENU_PREV, S_MENU_NEXT, S_SETTINGS, S_SETTINGS_SEL,
            S_RADAR1, S_RADAR2, S_RADAR3, S_RADAR4,
            S_INTEL_TITLE, S_INTEL_TEXT, S_INTEL_SOURCE, S_INTEL_AGE, S_COUNT };

// Asset names Launch Kit ships. Kept here rather than derived, so the contract between
// the two programs is one readable list instead of a naming convention nobody can see.
const char *SLOT_FILE[S_COUNT] = {
    "font_clock1.bin", "font_clock2.bin",
    "font_menu_current.bin", "font_menu_prev.bin", "font_menu_next.bin",
    "font_settings.bin", "font_settings_sel.bin",
    "font_radar1.bin", "font_radar2.bin", "font_radar3.bin", "font_radar4.bin",
    "font_intel_title.bin", "font_intel_text.bin", "font_intel_source.bin", "font_intel_age.bin",
};

const lv_font_t *s_font[S_COUNT] = { nullptr };

// The compiled font for each slot: what this firmware was built with, and what a slot
// falls back to when the theme ships nothing loadable.
const lv_font_t *compiled(Slot s) {
    switch (s) {
#if CUSTOM_HAS_TEXT1
        case S_CLOCK1: return CUSTOM_TEXT1_FONT;
#endif
#if CUSTOM_HAS_TEXT2
        case S_CLOCK2: return CUSTOM_TEXT2_FONT;
#endif
        case S_MENU_CUR:  return CUSTOM_MENU_CURRENT_FONT;
        case S_MENU_PREV: return CUSTOM_MENU_PREV_FONT;
        case S_MENU_NEXT: return CUSTOM_MENU_NEXT_FONT;
        case S_SETTINGS:  return CUSTOM_SETTINGS_FONT;
        // The selected row falls back to the same compiled face as the rest. A theme that
        // does not ask for a second weight ships no second file, and this slot then IS the
        // other one: same face, same size, same weight, and nothing on screen changes.
        case S_SETTINGS_SEL: return CUSTOM_SETTINGS_FONT;
#if CUSTOM_HAS_RTEXT1
        case S_RADAR1: return CUSTOM_RTEXT1_FONT;
#endif
#if CUSTOM_HAS_RTEXT2
        case S_RADAR2: return CUSTOM_RTEXT2_FONT;
#endif
#if CUSTOM_HAS_RTEXT3
        case S_RADAR3: return CUSTOM_RTEXT3_FONT;
#endif
#if CUSTOM_HAS_RTEXT4
        case S_RADAR4: return CUSTOM_RTEXT4_FONT;
#endif
        default: break;
    }
    return LV_FONT_DEFAULT;
}

const lv_font_t *get(Slot s) { return s_font[s] ? s_font[s] : compiled(s); }

} // namespace

void begin() {
    if (s_ready) return;
    s_ready = true;

    lv_fs_drv_init(&s_drv);
    s_drv.letter   = DRIVE_LETTER;
    s_drv.open_cb  = fs_open;
    s_drv.close_cb = fs_close;
    s_drv.read_cb  = fs_read;
    s_drv.seek_cb  = fs_seek;
    s_drv.tell_cb  = fs_tell;
    lv_fs_drv_register(&s_drv);

    for (int i = 0; i < S_COUNT; ++i) {
        const uint8_t *data = nullptr;
        size_t len = 0;
        // Cheap existence check before asking LVGL to parse: a theme that ships no font
        // for a slot is the normal case, not an error worth a log line each boot.
        if (!theme_art::find_blob(theme_select::activeSlug(), SLOT_FILE[i], data, len)) continue;
        // lv_font_load() parses the whole face into LVGL's heap. That heap is PSRAM now
        // (LV_MEM_CUSTOM in lv_conf.h); while it was the 64 KB internal pool, a 44 KB face
        // exhausted it, LVGL did not check the failed allocation, and load_glyph() wrote
        // through the null pointer — a boot loop before any screen drew.
        //
        // Still a copy rather than a read in place, which is not the ideal shape given the
        // bytes are already memory-mapped. It is bounded (tens of KB against megabytes
        // free) and uses LVGL's own tested parser, so the remaining zero-copy version is
        // an optimisation, not a correctness fix.
        char path[40];
        snprintf(path, sizeof(path), "%c:%s", DRIVE_LETTER, SLOT_FILE[i]);
        const lv_font_t *f = lv_font_load(path);
        if (!f) {
#ifdef ARDUINO
            Serial.printf("[theme_font] %s failed to parse — using the compiled font\n", SLOT_FILE[i]);
#endif
            continue;
        }
        s_font[i] = f;
        ++s_loaded;
    }
#ifdef ARDUINO
    Serial.printf("[theme_font] %d of %d slots loaded from the theme\n", s_loaded, (int)S_COUNT);
#endif
}

const lv_font_t *clock_text1()   { return get(S_CLOCK1); }
const lv_font_t *clock_text2()   { return get(S_CLOCK2); }
const lv_font_t *menu_current()  { return get(S_MENU_CUR); }
const lv_font_t *menu_prev()     { return get(S_MENU_PREV); }
const lv_font_t *menu_next()     { return get(S_MENU_NEXT); }
const lv_font_t *settings_item() { return get(S_SETTINGS); }
// Falls back to the LIST's face, NOT through get() to the compiled one.
//
// get() answers "this slot's file, or what the firmware was built with", which is right for
// every slot that stands alone. This one does not: a theme wanting a single weight ships
// only font_settings.bin, and the selected row has to be THAT face. Through get() it would
// have been the compiled stock face instead, so every theme in existence would have drawn
// one row of its Settings list in the wrong typeface the moment this slot was added.
const lv_font_t *settings_sel()  { return s_font[S_SETTINGS_SEL] ? s_font[S_SETTINGS_SEL] : settings_item(); }
// The Headlines screen had no slots at all until THEME_CAPS 15 and drew compiled
// Montserrat throughout, which made it the one screen whose type a design could not touch.
// Its compiled fallback is deliberately LV_FONT_DEFAULT rather than a CUSTOM_* macro: no
// firmware ever baked a face for this screen, so there is no legacy value to honour.
// Whether the THEME supplied this face, as opposed to the fallback. The Headlines screen
// needs the distinction that no other caller does: it owns a size slider, and a loaded
// font is baked at one size and cannot honour it. Knowing which it has is what lets that
// screen apply the slider to the compiled face and stand down for a themed one.
bool intel_has_font(int slot) {
    switch (slot) {
        case 0: return s_font[S_INTEL_TITLE]  != nullptr;
        case 1: return s_font[S_INTEL_TEXT]   != nullptr;
        case 2: return s_font[S_INTEL_SOURCE] != nullptr;
        case 3: return s_font[S_INTEL_AGE]    != nullptr;
        default: return false;
    }
}

const lv_font_t *intel_title()  { return get(S_INTEL_TITLE); }
const lv_font_t *intel_text()   { return get(S_INTEL_TEXT); }
const lv_font_t *intel_source() { return get(S_INTEL_SOURCE); }
const lv_font_t *intel_age()    { return get(S_INTEL_AGE); }

const lv_font_t *radar_text(int idx) {
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    return get((Slot)(S_RADAR1 + idx));
}
int loaded_count() { return s_loaded; }

} // namespace theme_font
