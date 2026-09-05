#include "orb_link.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>         // esp_efuse_mac_get_default, for cmd_hello
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <SD.h>
#include "mbedtls/base64.h"

#include "config.h"          // FW_VERSION
#include "custom_weld.h"     // CUSTOM_WELD_HASH
#include "sdcard.h"
#include "clock_view.h"
#include "theme_select.h"
#include "chime_library.h"
#include "theme_style.h"
#include "update_ui.h"
#include "input_router.h"  // cmd_turn drives the real input path
#include "app_shell.h"   // selectApp/nameAt for the app + apps commands
#include <strings.h>     // strncasecmp
#include <esp_heap_caps.h> // heap_caps_get_info for the "mem" command
#include "radar_view.h"  // debugHideLayer for the "layer" command
void host_set_poll_override(uint32_t ms);   // main.cpp
void host_location_reset();                 // main.cpp
void host_wifi_saved_ssid(char *out, size_t n);   // main.cpp — NAME only, never the password
void host_wifi_restore_saved();                   // main.cpp — put the backed-up network back

namespace orb_link {
namespace {

// One command line. 96 is comfortably past the longest real request ("theme " + a 32-char
// slug); anything longer is noise or a paste accident, and is dropped rather than wrapped,
// so a runaway line can never be split into two half-commands that both look valid.
// Sized for put-data lines: 384 base64 chars carry 288 raw bytes per line, and the
// header plus margin fits comfortably. Everything else on this port is far shorter.
constexpr size_t CMD_LINE_MAX = 512;
char   s_line[CMD_LINE_MAX];
size_t s_len      = 0;
bool   s_overflow = false;

bool (*s_themeHook)(const char *) = nullptr;

// A reply is assembled in full here and written to the port in ONE call.
//
// It is tempting to just print the pieces as they are computed. That was the first version,
// and it was wrong: theme_style::labelFor() opens theme.json off the SD card, and the SD
// layer logs a timing line while it does. The log landed in the middle of the JSON array
// being printed, and the browser received a reply chopped in half by an unrelated sentence.
// Framing replies is not enough on its own when producing a reply can itself print. So all
// the work that might log happens first, into this buffer, and only then does anything
// reach the wire.
//
// Static, not stack: at 16 themes this is larger than is polite to put on the Arduino loop
// task's stack, and only loop() ever touches it.
constexpr size_t OUT_MAX = 2560;
char   s_out[OUT_MAX];
size_t s_out_len  = 0;
bool   s_out_full = false;

void out_reset() { s_out_len = 0; s_out_full = false; }
void out_ch(char c) {
    if (s_out_len < OUT_MAX - 2) s_out[s_out_len++] = c;
    else                         s_out_full = true;
}
void out_str(const char *s) { while (*s) out_ch(*s++); }

// Theme display names come out of a user-authored theme.json, so they can contain quotes
// and backslashes. Emitting them raw would produce JSON the browser cannot parse, and the
// failure would look like "the Orb stopped responding" rather than "your theme name has a
// quote in it". Control characters are dropped: they have no business in a label and
// escaping them properly would cost more than it buys.
void out_json_string(const char *s) {
    out_ch('"');
    for (const char *p = s; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out_ch('\\'); out_ch((char)c); }
        else if (c >= 0x20)        { out_ch((char)c); }
    }
    out_ch('"');
}

void out_fmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(s_out + s_out_len, OUT_MAX - 1 - s_out_len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= OUT_MAX - 1 - s_out_len) s_out_full = true;
    else                                               s_out_len += (size_t)n;
}

void reply_error(const char *msg);

// One write, tag and payload and newline together, so nothing can be interleaved into it.
void out_send() {
    if (s_out_full) { reply_error("reply too large"); return; }
    s_out[s_out_len++] = '\n';
    Serial.write((const uint8_t *)RESP_TAG, strlen(RESP_TAG));
    Serial.write((const uint8_t *)s_out, s_out_len);
}

void reply_error(const char *msg) {
    // Built directly, bypassing the shared buffer: this is the path that runs when that
    // buffer has already overflowed.
    Serial.print(RESP_TAG);
    Serial.print("{\"ok\":false,\"error\":\"");
    Serial.print(msg);
    Serial.println("\"}");
}

// Identity. This is what "Connect your Orb" shows, and it is deliberately the same set of
// facts /health reports over WiFi: one source of truth for what this device is running, so
// a cable diagnosis and a network diagnosis can never disagree.
void cmd_hello() {
    out_reset();
    out_str("{\"ok\":true,\"product\":");
    out_json_string(PRODUCT_NAME);
    // caps: what this firmware understands of a theme (theme_style::THEME_CAPS). proto is
    // the wire format and moves only when the conversation itself changes; caps moves every
    // time a new theme setting is readable. A tool needs the second one to know whether a
    // design will actually be honoured.
    out_fmt(",\"proto\":%d,\"caps\":%d,\"fw\":\"%s\"", PROTOCOL_VERSION, theme_style::THEME_CAPS, FW_VERSION);
    // The chip's burned-in base MAC, the same six bytes esptool prints, so Studio can
    // recognise THIS Orb again whatever firmware or theme is on it. Nothing else in this
    // reply survives an erase: the slug is NVS, the theme is the card, fw is what was just
    // written. It is what lets "have I set this Orb up before" be a fact rather than a guess
    // from silence, and the guess from silence is what wiped a board and seeded three
    // Aviators on 2026-09-01.
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    out_fmt(",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    out_str(",\"slug\":");
    out_json_string(theme_select::activeSlug());
    out_str(",\"theme\":");
    out_json_string(theme_style::themeLabel());
    out_fmt(",\"weld\":%lu,\"assets\":%lu,\"uptime_s\":%lu}",
            (unsigned long)CUSTOM_WELD_HASH,
            (unsigned long)theme_style::assetsFingerprint(),
            (unsigned long)(millis() / 1000UL));
    out_send();
}

// Where the Orb lives on the network, so a tool can tell someone the address to open.
//
// This exists because of the cable-free theme route. Themes reach the card either over this
// serial link or over the Orb's OWN web page, and the second one is better in every way
// except that you have to know where the Orb is. mDNS gives theorb.local, which some
// networks quietly refuse to resolve, so the raw address has to be obtainable too.
//
// `up` is the whole answer to "did WiFi setup actually work". Before this, a tool could
// only ask the person, and a mistyped password looked exactly like success until the
// Flight Tracker came up empty an hour later.
void cmd_wifi() {
    out_reset();
#ifdef ARDUINO
    const bool up = (WiFi.status() == WL_CONNECTED);
    out_fmt("{\"ok\":true,\"up\":%s", up ? "true" : "false");
    if (up) {
        out_str(",\"ssid\":");
        out_json_string(WiFi.SSID().c_str());
        out_str(",\"ip\":");
        out_json_string(WiFi.localIP().toString().c_str());
        out_fmt(",\"rssi\":%d", (int)WiFi.RSSI());
        out_str(",\"host\":");
        out_json_string(ORB_MDNS_ADDR);
    }
    out_str("}");
#else
    out_str("{\"ok\":true,\"up\":false}");
#endif
    out_send();
}

// Both the slug (folder id, stable) and the label (display name, themeable) go out. Sending
// only the slug is what once made "Modern" and "the-office" look like unrelated things.
void cmd_themes() {
    static char slugs[theme_select::MAX_THEMES][theme_select::MAX_SLUG_LEN];
    const int n = theme_select::listInstalled(slugs);

    out_reset();
    out_str("{\"ok\":true,\"active\":");
    out_json_string(theme_select::activeSlug());
    out_str(",\"themes\":[");
    for (int i = 0; i < n; ++i) {
        char label[64];
        theme_style::labelFor(slugs[i], label, sizeof(label));   // reads SD, and logs while it does
        if (i) out_ch(',');
        out_str("{\"slug\":");
        out_json_string(slugs[i]);
        out_str(",\"name\":");
        out_json_string(label);
        out_ch('}');
    }
    out_str("]}");
    out_send();
}

void cmd_theme(const char *slug) {
    if (!slug || !*slug)  { reply_error("missing slug");        return; }
    if (!s_themeHook)     { reply_error("switching unavailable"); return; }
    // The hook validates against what is actually on the card and defers the reboot; it
    // does not switch here. theme_select::set() restarts the chip, and restarting before
    // this reply is flushed would leave the browser watching a port that just vanished
    // with no answer, which is indistinguishable from a crash.
    if (!s_themeHook(slug)) { reply_error("no such theme on this Orb"); return; }
    out_reset();
    out_str("{\"ok\":true,\"switching\":");
    out_json_string(slug);
    out_ch('}');
    out_send();
    Serial.flush();   // the reboot is ~400 ms out; do not race it
}

// ---------------- apps ----------------
//
// Which screens this Orb has, and which one is showing. Added 2026-08-22 to chase a bug the
// device would only exhibit in one app: the ADS-B poll runs in Flight Tracker ONLY
// (main.cpp), so a fault in the feed prints nothing at all while the Orb sits on the clock,
// and reproducing it meant a person standing at the desk turning the knob for every
// attempt. Reading the state and being able to set it makes that a loop a tool can run.
//
// Safe from here: poll() is called from loop(), the same task LVGL runs in, which is the
// same context the knob's own handler switches apps from.
void cmd_apps() {
    out_reset();
    out_str("{\"ok\":true,\"current\":");
    out_fmt("%d", app_shell::index());
    out_str(",\"apps\":[");
    for (int i = 0; i < app_shell::count(); ++i) {
        if (i) out_ch(',');
        out_str("{\"i\":");
        out_fmt("%d", i);
        out_str(",\"name\":");
        out_json_string(app_shell::nameAt(i));
        out_str(app_shell::hiddenAt(i) ? ",\"hidden\":true}" : "}");
    }
    out_str("]}");
    out_send();
}

// By index, or by name, case-insensitively and on a prefix, so "flight" reaches "Flight
// Tracker" without anyone having to remember the exact label. A name that matches more than
// one app is refused rather than guessed at.
void cmd_app(const char *arg) {
    if (!arg || !*arg) { reply_error("missing app"); return; }
    const int n = app_shell::count();
    int want = -1;

    bool numeric = true;
    for (const char *p = arg; *p; ++p) if (*p < '0' || *p > '9') { numeric = false; break; }
    if (numeric) {
        want = atoi(arg);
        if (want < 0 || want >= n) { reply_error("no such app"); return; }
    } else {
        const size_t len = strlen(arg);
        int hits = 0;
        for (int i = 0; i < n; ++i) {
            const char *nm = app_shell::nameAt(i);
            if (strncasecmp(nm, arg, len) == 0) { want = i; ++hits; }
        }
        if (hits == 0) { reply_error("no such app"); return; }
        if (hits > 1)  { reply_error("that name matches more than one app"); return; }
    }

    app_shell::selectApp(want);
    out_reset();
    out_str("{\"ok\":true,\"app\":");
    out_json_string(app_shell::nameAt(want));
    out_fmt(",\"i\":%d}", want);
    out_send();
}

// A knob press, over the cable. Only meaningful on the Flight Tracker, where a press
// enters selection mode and puts the info card on screen.
//
// Added for the same reason as `app`: the route line on that card is fetched on demand and
// only ever requested for a SELECTED aircraft, so nothing about it — not the lookup, not
// the card's idle timeout, not whether the text arrives before the card closes — can be
// observed without someone standing at the device pressing the knob. Same task as the
// knob's own handler, so this is exactly the input path, not a parallel one.
// The host is about to hand this chip to a firmware flasher.
//
// Answered before anything is reset, because after that this firmware is not running to
// answer anything. All it does is put the update notice on screen: esptool's write leaves
// the panel frozen on whatever frame it last had for two minutes, and that frame is the
// only thing the owner has to go on. A stopped clock reads as a crash and gets the cable
// pulled mid-write; a screen that says "updating, do not unplug" does not.
//
// Note the ordering. update_ui paints synchronously and only then do we reply, so by the
// time Studio is free to start resetting the board, the pixels are already on glass.
void cmd_flashing() {
    update_ui::firmware_incoming();
    out_reset();
    out_str("{\"ok\":true,\"showing\":\"firmware-update\"}");
    out_send();
}

// Turn the knob from here.
//
// Added to measure the menu without a hand on the device: the question is how long a detent
// takes to reach the glass, and answering it needs the turn and the stopwatch on the same
// side of the cable. Goes through input_router, not straight into app_shell, so it takes
// exactly the path a real detent takes, Rock detection and all.
void cmd_menu() {
    app_shell::openSwitcher();
    out_reset();
    out_str("{\"ok\":true,\"menu\":true}");
    out_send();
}

void cmd_turn(const char *arg) {
    const int n = arg && *arg ? atoi(arg) : 1;
    input_router::dispatch(n, false);
    out_reset();
    out_fmt("{\"ok\":true,\"turned\":%d}", n);
    out_send();
}

void cmd_press() {
    app_shell::pressCurrent();
    out_reset();
    out_str("{\"ok\":true,\"pressed\":");
    out_json_string(app_shell::name());
    out_ch('}');
    out_send();
}

// ---------------- diagnostics over the cable ----------------
//
// Mirrors of the /rdbg web controls, on the wire that still works when the device is too
// short of memory to answer HTTP — which is the only condition this investigation cares
// about. Without these, every experiment needs the very resource under investigation.
void cmd_poll(const char *arg) {
    const uint32_t ms = (arg && *arg) ? (uint32_t)atol(arg) : 0;
    host_set_poll_override(ms);
    out_reset();
    out_fmt("{\"ok\":true,\"pollMs\":%lu}", (unsigned long)ms);
    out_send();
}

void cmd_layer(char *arg) {
    // "<kind> <0|1>"; 0=sweep 1=aircraft 2=readout 3/4=still art 5=wash 6=plate 7=map 8=glass
    if (!arg || !*arg) { reply_error("usage: layer <kind> <0|1>"); return; }
    char *sp = strchr(arg, ' ');
    if (!sp) { reply_error("usage: layer <kind> <0|1>"); return; }
    *sp++ = '\0';
    const int kind = atoi(arg);
    const bool hide = (atoi(sp) != 0);
    radar::debugHideLayer(kind, hide);
    out_reset();
    out_fmt("{\"ok\":true,\"layer\":%d,\"hidden\":%s}", kind, hide ? "true" : "false");
    out_send();
}

void cmd_sweepms(const char *arg) {
    const uint32_t ms = (arg && *arg) ? (uint32_t)atol(arg) : 0;
    radar::setSweepFrameMs(ms);
    out_reset();
    out_fmt("{\"ok\":true,\"sweepMs\":%lu}", (unsigned long)ms);
    out_send();
}

// Reproduce "this Orb has never been located" without a factory reset and without having
// to find a network with no internet on it. Clears locSet only; coordinates, WiFi, theme
// and every other setting are untouched. See host_location_reset() in main.cpp for why the
// cheap version of this test matters.
void cmd_interpms(const char *arg) {
    const uint32_t ms = (arg && *arg) ? (uint32_t)atol(arg) : 0;
    radar::setAcInterpMs(ms);
    out_reset();
    out_fmt("{\"ok\":true,\"interpMs\":%lu}", (unsigned long)ms);
    out_send();
}

void cmd_glide(const char *arg) {
    const int mode = (arg && *arg) ? atoi(arg) : -1;
    radar::setGlide(mode);
    out_reset();
    out_fmt("{\"ok\":true,\"glide\":%d}", mode);
    out_send();
}

void cmd_sweep(const char *arg) {
    const int mode = (arg && *arg) ? atoi(arg) : -1;
    clockview::setSweep(mode);
    out_reset();
    out_fmt("{\"ok\":true,\"sweep\":%d}", mode);
    out_send();
}

void cmd_sweepaa(const char *arg) {
    const int on = (arg && *arg) ? atoi(arg) : 1;
    radar::setSweepAA(on);
    out_reset();
    out_fmt("{\"ok\":true,\"aa\":%d}", on);
    out_send();
}

void cmd_locreset() {
    host_location_reset();
    out_reset();
    out_fmt("{\"ok\":true,\"locationSet\":false}");
    out_send();
}

// The stored network's name, so a failed-attempt test can be run and checked without a
// reboot and without anybody reading a secret off a wire. Deliberately name-only: the
// question this answers is "is the owner's network still there", and the password is not
// part of that answer.
void cmd_wifisaved() {
    char ssid[40];
    host_wifi_saved_ssid(ssid, sizeof(ssid));
    out_reset();
    out_str("{\"ok\":true,\"saved\":");
    out_json_string(ssid);
    out_str("}");
    out_send();
}

// The safety net for testing the one above. If a failed attempt does destroy the stored
// network despite the backup, this puts it back from our own namespace without the owner
// having to type a password again — which is the thing that has now cost him two evenings.
void cmd_wifirestore() {
    host_wifi_restore_saved();
    char ssid[40];
    host_wifi_saved_ssid(ssid, sizeof(ssid));
    out_reset();
    out_str("{\"ok\":true,\"saved\":");
    out_json_string(ssid);
    out_str("}");
    out_send();
}

void cmd_mem() {
    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
    out_reset();
    out_fmt("{\"ok\":true,\"free\":%u,\"largest\":%u,\"freeBlocks\":%u,\"allocBlocks\":%u,\"psramFree\":%u}",
            (unsigned)hi.total_free_bytes, (unsigned)hi.largest_free_block,
            (unsigned)hi.free_blocks, (unsigned)hi.allocated_blocks,
            (unsigned)ESP.getFreePsram());
    out_send();
}

// ---------------- file transfer (put-begin / put-data / put-end) ----------------
//
// Why this exists: Orb Studio is a public HTTPS page, and a secure page is forbidden by
// the browser from calling the Orb's plain-HTTP /sdput endpoint on the LAN (mixed
// content), never mind that it cannot resolve the address from outside. The cable is
// already the site's transport for everything else, so files ride it too.
//
// The shape mirrors the WiFi path deliberately: same /themes/-only path jail, same
// create-every-directory-level behaviour (SD.mkdir does not create intermediates), same
// on-screen update_ui narration so an install is never silent on the device. Content
// travels as base64 lines, each acknowledged before the next is sent — self-throttling,
// and on files this size (style JSON, a few KB) throughput is irrelevant.
File     s_putFile;
bool     s_putOpen     = false;
uint32_t s_putExpected = 0;
uint32_t s_putWritten  = 0;
char     s_putName[48] = "";
int      s_putCount    = 0;      // files received this session, for the on-screen counter

// Reading back out. Its own handle, so a read can never disturb an install in flight.
File     s_getFile;
bool     s_getOpen     = false;
uint32_t s_getLeft     = 0;

bool slug_ok(const char *t) {
    if (!t || !*t || strlen(t) >= theme_select::MAX_SLUG_LEN) return false;
    for (const char *p = t; *p; ++p)
        if (!islower((unsigned char)*p) && !isdigit((unsigned char)*p) && *p != '-') return false;
    return true;
}
bool fname_ok(const char *t) {
    if (!t || !*t || *t == '.' || strlen(t) >= sizeof(s_putName)) return false;
    for (const char *p = t; *p; ++p)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') return false;
    return strstr(t, "..") == nullptr;
}

void put_abort() {
    if (s_putOpen) s_putFile.close();
    s_putOpen = false;
}

void get_abort() {
    if (s_getOpen) s_getFile.close();
    s_getOpen = false;
    s_getLeft = 0;
}

// Where a put may write. /themes/ is the everyday case (Orb Studio installing a design);
// /roads/ is the map-tile store, which otherwise had no way onto the card at all except
// pulling the microSD out of the device. Both are device-owned data directories, and the
// filename rules below still forbid traversal, so widening to two named roots does not
// widen what a caller can reach.
bool root_ok(const char *slug, bool *isRoads) {
    if (!strcmp(slug, "roads")) { *isRoads = true; return true; }
    *isRoads = false;
    return slug_ok(slug);
}

// Remove an installed theme from the card.
//
// The only destructive command in the protocol, so it says no in three places rather than
// trusting the caller: an unknown slug, the theme currently being worn, and a transfer in
// flight all refuse. The reasons are returned as text because the browser shows them to a
// person, and "cannot delete the theme you are wearing" is an instruction, while a bare
// failure is a puzzle.
void cmd_delete(const char *slug) {
    if (!slug || !*slug)             { reply_error("missing slug");    return; }
    if (!sdcard::mounted())          { reply_error("no SD card");      return; }
    if (s_putOpen)                   { reply_error("install in progress"); return; }
    if (!strcmp(slug, theme_select::activeSlug())) {
        reply_error("that is the theme this Orb is wearing — switch to another one first");
        return;
    }
    if (!theme_select::removeInstalled(slug)) { reply_error("no such theme"); return; }
    // A deleted theme takes its chime out of the picker with it, and if that chime was the
    // one selected, the library falls back rather than ringing a file that is gone. Installing
    // needs no equivalent: it restarts the Orb.
    chime_library::rescan();
    // The baked copy in flash is deliberately left alone. It is only ever consulted for the
    // ACTIVE slug, so an orphan is invisible; the art partition already reclaims space by
    // wiping and re-baking when it runs low. Rewriting its index here would be a flash
    // write with real failure modes, in exchange for space nothing is waiting on.
    out_reset(); out_str("{\"ok\":true}"); out_send();
}

// Reading a file back off the card, which the protocol could never do: it could write a
// theme to an Orb and switch between themes, but everything it sent was one-way. A design
// installed from another browser, or from a machine since wiped, was on the device and
// nowhere else, and the editor had no way to ask for it.
//
// Host-driven, one reply per request, same as everything else here. The device never speaks
// unless spoken to, so the browser's "send a line, wait for a line" loop needs nothing new
// to understand a transfer that arrives in pieces.
void cmd_get_begin(char *args) {
    get_abort();
    char *slug = args;
    char *file = args ? strchr(args, ' ') : nullptr;
    if (file) { *file++ = '\0'; while (*file == ' ') ++file; }
    bool isRoads = false;
    if (!slug || !root_ok(slug, &isRoads)) { reply_error("bad get-begin"); return; }
    if (!fname_ok(file))                   { reply_error("bad get-begin"); return; }
    if (!sdcard::mounted())                { reply_error("no SD card");    return; }

    char path[96];
    if (isRoads) snprintf(path, sizeof(path), "/roads/%s", file);
    else         snprintf(path, sizeof(path), "/themes/%s/%s", slug, file);
    s_getFile = SD.open(path, FILE_READ);
    if (!s_getFile) { reply_error("no such file"); return; }
    s_getOpen = true;
    s_getLeft = (uint32_t)s_getFile.size();
    out_reset(); out_fmt("{\"ok\":true,\"size\":%lu}", (unsigned long)s_getLeft); out_send();
}

void cmd_get_data() {
    if (!s_getOpen) { reply_error("no transfer open"); return; }
    // 1536 raw bytes encodes to 2048 base64 characters, which fits the 2560-byte reply
    // buffer with room for the JSON around it. Bigger than a put chunk on purpose: writes
    // are acknowledged one at a time for flow control, reads are simply pulled.
    uint8_t raw[1536];
    const int n = s_getFile.read(raw, sizeof(raw));
    if (n <= 0) {
        get_abort();
        out_reset(); out_str("{\"ok\":true,\"done\":true}"); out_send();
        return;
    }
    unsigned char b64[2100];
    size_t b64Len = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &b64Len, raw, (size_t)n) != 0) {
        get_abort(); reply_error("encode failed"); return;
    }
    b64[b64Len] = '\0';
    s_getLeft = s_getLeft > (uint32_t)n ? s_getLeft - (uint32_t)n : 0;
    out_reset();
    out_fmt("{\"ok\":true,\"b64\":\"%s\",\"left\":%lu}", (const char *)b64, (unsigned long)s_getLeft);
    out_send();
}

void cmd_put_begin(char *args) {
    put_abort();   // a new begin implicitly abandons any half-finished transfer
    char *slug = args;
    char *file = args ? strchr(args, ' ') : nullptr;
    if (file) { *file++ = '\0'; while (*file == ' ') ++file; }
    char *size = file ? strchr(file, ' ') : nullptr;
    if (size) { *size++ = '\0'; while (*size == ' ') ++size; }
    bool isRoads = false;
    if (!slug || !root_ok(slug, &isRoads))          { reply_error("bad put-begin"); return; }
    if (!fname_ok(file) || !size)                   { reply_error("bad put-begin"); return; }
    if (!sdcard::mounted())                         { reply_error("no SD card");     return; }

    char path[96];
    if (isRoads) snprintf(path, sizeof(path), "/roads/%s", file);
    else         snprintf(path, sizeof(path), "/themes/%s/%s", slug, file);
    // Create every missing level (same reasoning as the WiFi path: SD.mkdir does not
    // create intermediates, so a virgin card fails at /themes otherwise).
    for (int i = 1; path[i]; ++i) {
        if (path[i] != '/') continue;
        path[i] = '\0';
        if (!SD.exists(path) && !SD.mkdir(path)) { path[i] = '/'; reply_error("mkdir failed"); return; }
        path[i] = '/';
    }
    s_putFile = SD.open(path, FILE_WRITE);   // truncates any existing file
    if (!s_putFile) { reply_error("open failed"); return; }
    s_putOpen     = true;
    s_putExpected = (uint32_t)strtoul(size, nullptr, 10);
    s_putWritten  = 0;
    strlcpy(s_putName, file, sizeof(s_putName));
    Serial.printf("[orb_link] put %s (%lu bytes)\n", path, (unsigned long)s_putExpected);
    out_reset(); out_str("{\"ok\":true}"); out_send();
}

void cmd_put_data(const char *b64) {
    if (!s_putOpen)       { reply_error("no transfer open"); return; }
    if (!b64 || !*b64)    { reply_error("empty chunk");      return; }
    unsigned char raw[400];
    size_t rawLen = 0;
    if (mbedtls_base64_decode(raw, sizeof(raw), &rawLen,
                              (const unsigned char *)b64, strlen(b64)) != 0) {
        put_abort(); reply_error("bad base64"); return;
    }
    if (s_putFile.write(raw, rawLen) != rawLen) {
        put_abort(); reply_error("short write (card full or removed?)"); return;
    }
    s_putWritten += rawLen;
    // Tell the screen a chunk landed. Without this the interrupted-watchdog only ever hears
    // about COMPLETED files, so any file taking more than twelve seconds looked like a dead
    // transfer while it was still arriving.
    update_ui::file_progress(s_putName, s_putCount, s_putWritten);
    out_reset(); out_fmt("{\"ok\":true,\"n\":%lu}", (unsigned long)s_putWritten); out_send();
}

void cmd_put_end() {
    if (!s_putOpen) { reply_error("no transfer open"); return; }
    s_putFile.close();
    s_putOpen = false;
    if (s_putWritten != s_putExpected) {
        reply_error("size mismatch");
        return;
    }
    // Same narration as a WiFi push: the Orb's own screen says the install is happening,
    // so the person standing at the device is never guessing (Zion's standing mandate).
    update_ui::file_received(s_putName, ++s_putCount);
    out_reset();
    out_fmt("{\"ok\":true,\"file\":\"%s\",\"bytes\":%lu}", s_putName, (unsigned long)s_putWritten);
    out_send();
}

void dispatch(char *line) {
    // Split the verb from the rest. Only one argument is ever needed, so the remainder is
    // taken whole rather than tokenised further: a slug never contains a space, and if one
    // somehow did, listInstalled validation rejects it anyway.
    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = '\0'; while (*arg == ' ') ++arg; }

    if      (!strcmp(line, "hello"))     cmd_hello();
    else if (!strcmp(line, "themes"))    cmd_themes();
    else if (!strcmp(line, "wifi"))      cmd_wifi();
    else if (!strcmp(line, "theme"))     cmd_theme(arg);
    else if (!strcmp(line, "delete"))    cmd_delete(arg);
    else if (!strcmp(line, "apps"))      cmd_apps();
    else if (!strcmp(line, "app"))       cmd_app(arg);
    else if (!strcmp(line, "flashing"))  cmd_flashing();
    else if (!strcmp(line, "menu"))      cmd_menu();
    else if (!strcmp(line, "turn"))      cmd_turn(arg);
    else if (!strcmp(line, "press"))     cmd_press();
    else if (!strcmp(line, "poll"))      cmd_poll(arg);
    else if (!strcmp(line, "layer"))     cmd_layer(arg);
    else if (!strcmp(line, "mem"))       cmd_mem();
    else if (!strcmp(line, "locreset"))  cmd_locreset();
    else if (!strcmp(line, "wifisaved")) cmd_wifisaved();
    else if (!strcmp(line, "wifirestore")) cmd_wifirestore();
    else if (!strcmp(line, "sweepms"))   cmd_sweepms(arg);
    else if (!strcmp(line, "interpms")) cmd_interpms(arg);
    else if (!strcmp(line, "glide"))    cmd_glide(arg);
    else if (!strcmp(line, "sweep"))    cmd_sweep(arg);
    else if (!strcmp(line, "sweepaa")) cmd_sweepaa(arg);
    else if (!strcmp(line, "get-begin")) cmd_get_begin(arg);
    else if (!strcmp(line, "get-data"))  cmd_get_data();
    else if (!strcmp(line, "put-begin")) cmd_put_begin(arg);
    else if (!strcmp(line, "put-data"))  cmd_put_data(arg);
    else if (!strcmp(line, "put-end"))   cmd_put_end();
    else                                 reply_error("unknown command");
}

}   // namespace

void setThemeRequestHook(bool (*hook)(const char *)) { s_themeHook = hook; }

void begin() { s_len = 0; s_overflow = false; }

bool transferActive() { return s_putOpen; }

void poll() {
    // Bounded per call. A host that floods the port cannot hold loop() hostage and stall
    // the knob; leftovers are simply read on the next pass a few milliseconds later.
    int budget = 640;   // a full put-data line per pass; still bounded, still knob-safe
    while (Serial.available() > 0 && budget-- > 0) {
        const char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c != '\n') {
            if (s_len < CMD_LINE_MAX - 1) s_line[s_len++] = c;
            else                      s_overflow = true;   // poisoned; discard at newline
            continue;
        }
        s_line[s_len] = '\0';
        const size_t len = s_len;
        s_len = 0;
        if (s_overflow) { s_overflow = false; continue; }

        // Untagged lines are somebody using the console, not talking to us. Silence is the
        // right response: echoing an error for every stray keystroke would bury the log.
        const size_t tag = strlen(REQ_TAG);
        if (len > tag && !strncmp(s_line, REQ_TAG, tag)) dispatch(s_line + tag);
    }
}

}   // namespace orb_link
