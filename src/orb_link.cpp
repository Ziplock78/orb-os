#include "orb_link.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <SD.h>
#include "mbedtls/base64.h"

#include "config.h"          // FW_VERSION
#include "custom_weld.h"     // CUSTOM_WELD_HASH
#include "sdcard.h"
#include "theme_select.h"
#include "theme_style.h"
#include "update_ui.h"

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
    out_fmt(",\"proto\":%d,\"fw\":\"%s\"", PROTOCOL_VERSION, FW_VERSION);
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
    else if (!strcmp(line, "theme"))     cmd_theme(arg);
    else if (!strcmp(line, "put-begin")) cmd_put_begin(arg);
    else if (!strcmp(line, "put-data"))  cmd_put_data(arg);
    else if (!strcmp(line, "put-end"))   cmd_put_end();
    else                                 reply_error("unknown command");
}

}   // namespace

void setThemeRequestHook(bool (*hook)(const char *)) { s_themeHook = hook; }

void begin() { s_len = 0; s_overflow = false; }

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
