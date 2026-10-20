// The Orb OS — the web server at http://theorb.local/: the front page, the theme install
// page and /sdput, /health and the other diagnostics, and the /legacy configuration form
// whose endpoints the old settings page still calls. Moved out of main.cpp unchanged;
// the route table that was inline in setup() is web_server_begin() below.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <nvs.h>
#include "config.h"
#include "orb_state.h"
#include "orb_web.h"
#include "orb_apps.h"
#include "orb_stats.h"
#include "radar_view.h"
#include "ui.h"
#include "display.h"
#include "audio.h"
#include "sdcard.h"
#include "update_ui.h"
#include "clock_view.h"
#include "theme_select.h"
#include "theme_style.h"
#include "theme_art.h"
#include "custom_weld.h"
#include "lv_psram_alloc.h"
#include "diag_log.h"
#if ORB_OTA_ENABLED
#include <Update.h>
#endif

// Web-selectable time zones (label + POSIX TZ). The <option> value is the index; the save
// handler maps it back to the POSIX string stored in NVS and used by configTzTime at boot.
// (Index avoids putting POSIX strings with '<>' / ',' into HTML attributes.)
// offMin = standard (winter) UTC offset in minutes; dst = 1 if the zone observes DST.
// The web page uses these to auto-pick the visitor's zone from their browser clock.
static const struct { const char *label; const char *tz; int offMin; int dst; } TZOPTS[] = {
    {"UTC",                      "UTC0",                              0, 0},
    {"London / Lisbon",          "GMT0BST,M3.5.0/1,M10.5.0",          0, 1},
    {"Madrid / Paris / Berlin",  "CET-1CEST,M3.5.0,M10.5.0/3",       60, 1},
    {"Athens / Helsinki",        "EET-2EEST,M3.5.0/3,M10.5.0/4",     120, 1},
    {"New York (US Eastern)",    "EST5EDT,M3.2.0,M11.1.0",          -300, 1},
    {"Chicago (US Central)",     "CST6CDT,M3.2.0,M11.1.0",          -360, 1},
    {"Denver (US Mountain)",     "MST7MDT,M3.2.0,M11.1.0",          -420, 1},
    {"Phoenix (Arizona)",        "MST7",                            -420, 0},
    {"Los Angeles (US Pacific)", "PST8PDT,M3.2.0,M11.1.0",          -480, 1},
    {"Anchorage (Alaska)",       "AKST9AKDT,M3.2.0,M11.1.0",        -540, 1},
    {"Honolulu (Hawaii)",        "HST10",                           -600, 0},
    {"Argentina / Brazil (E)",   "<-03>3",                          -180, 0},
    {"India (IST)",              "<+0530>-5:30",                     330, 0},
    {"China / Singapore",        "<+08>-8",                          480, 0},
    {"Japan / Korea",            "JST-9",                            540, 0},
    {"Sydney (AU Eastern)",      "AEST-10AEDT,M10.1.0,M4.1.0/3",     600, 1},
    {"Auckland (NZ)",            "NZST-12NZDT,M9.5.0,M4.1.0/3",      720, 1},
};
static const int TZOPTS_N = sizeof(TZOPTS) / sizeof(TZOPTS[0]);

// ----------------------------- configuration web --------------------------------
static WebServer g_web(80);

// The Orb's own front page at http://theorb.local/. What a person should find here: what
// this Orb is running and wearing, and the two things the browser can do for it that the
// knob cannot (install a downloaded theme file, update the firmware over WiFi). The page
// that used to be here was Capsule Radar's configuration form, renamed, with a map to
// drag, a palette picker and a dozen radar knobs that Orb Studio and the Settings screen
// now own; the first stranger to build one found it and asked, reasonably, whether it was
// meant to be there (CanadianAvenger, 2026-09-14). It is not gone, because its endpoints
// are still what the Settings screen calls and Zion still uses the form to poke at a
// device: it lives at /legacy, unadvertised.
//
// Streamed, not assembled. This built the whole page in an Arduino String with
// reserve(2600).
//
// The fault, as observed on 2.16.63: that String came out of the ~320 KB INTERNAL heap,
// which by the time a custom theme is up is fragmented into crumbs, and the page failed in
// SILENCE — Arduino String reports an exhausted heap by quietly truncating, and send() then
// posts the truncation as a 200. Confirmed by curl against a themed Orb: /health answered
// and / did not, the only difference between them being that /health builds into a
// 420-byte stack buffer and asks the heap for nothing. Measured at the time: 2.3 KB free
// internal with a 628-byte largest block.
//
// 2.16.65 lowered the extmem threshold to 512, so a 2.6 KB String now lands in PSRAM and
// the original would most likely survive. This stays, for two reasons that do not depend
// on that number: a page assembled in a heap buffer costs memory proportional to the page
// and this one costs none at all (the static halves stream straight out of flash, and the
// largest allocation is a 200-byte stack row), and silent truncation remains the failure
// mode if it ever is short. It is also how /install has always served its own page.
//
// The body now goes out in chunks straight from flash, so the largest internal block this
// page needs is the ~200-byte row below rather than the whole document. Nothing here
// allocates proportionally to the page any more.
static const char ROOT_HEAD[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>The Orb</title><style>"
    "body{background:#f5f5f4;color:#2a2622;font-family:system-ui,-apple-system,sans-serif;margin:0 auto;padding:28px 20px;max-width:520px}"
    "h1{font-size:26px;margin:0 0 4px;letter-spacing:-.02em}.sub{color:#7a7570;margin:0 0 22px}"
    ".card{background:#fff;border:1px solid #e0e0df;border-radius:14px;padding:16px 18px;margin-bottom:14px}"
    "dl{display:grid;grid-template-columns:auto 1fr;gap:6px 16px;margin:0;font-size:15px}dt{color:#7a7570}dd{margin:0}"
    "a.b{display:block;padding:12px 14px;border:1px solid #e0e0df;border-radius:10px;color:#2a2622;text-decoration:none;margin-top:8px;font-weight:500}"
    "a.b:hover{border-color:#a65e3f;color:#a65e3f}small{color:#7a7570;display:block;margin-top:14px;line-height:1.5}"
    "</style></head><body>"
    "<h1>The Orb</h1><p class=sub>This Orb, over your WiFi</p>"
    "<div class=card><dl>";

static const char ROOT_TAIL[] PROGMEM =
    "</dl></div>"
    "<div class=card>"
    "<a class=b href='/install'>Install a theme file</a>"
    // Upstream 2.16.64 guarded this link: with no OTA partition the page it points at
    // 404s, so advertising it is a lie. Preserved here because this function moved into a
    // PROGMEM literal in the same release — string-literal concatenation takes a #if
    // perfectly well, so the guard survives the move intact.
#if ORB_OTA_ENABLED
    "<a class=b href='/update'>Update the firmware over WiFi</a>"
#endif
    "<a class=b href='/health'>Health readout</a>"
    "</div>"
    "<small>Everything else is set on the Orb itself, with the knob, under Settings: location, "
    "units, range, brightness, when the screen dims, sound, WiFi. What the screens look like is "
    "designed in Orb Studio and installed from there over the cable, or as a file through the "
    "link above.</small>"
    "</body></html>";

static void handleRoot() {
    // CONTENT_LENGTH_UNKNOWN puts the reply in chunked transfer encoding, which is what
    // lets the body leave in pieces without knowing the total up front.
    g_web.setContentLength(CONTENT_LENGTH_UNKNOWN);
    g_web.send(200, "text/html", "");
    g_web.sendContent_P(ROOT_HEAD);
    char row[200];
    snprintf(row, sizeof(row), "<dt>Firmware</dt><dd>%s</dd><dt>Wearing</dt><dd>%s</dd><dt>Address</dt><dd>%s</dd>",
             FW_VERSION, theme_style::themeLabel(), WiFi.localIP().toString().c_str());
    // The (ptr, len) overloads throughout, never the String ones: sendContent(const char *)
    // converts through an Arduino String, which is an internal-heap allocation and exactly
    // what this page is being moved off.
    g_web.sendContent(row, strlen(row));
    g_web.sendContent_P(ROOT_TAIL);
    g_web.sendContent("", 0);    // the zero-length chunk that ends a chunked body
}

static void handleLegacyConfig() {
    const int th = radar::theme();
    const int ranges[] = {10, 15, 25, 30, 50, 100, 150, 250};
    // The value submitted stays in km (the device works in km); only the label is shown in
    // the user's chosen distance unit so the config page matches the screen.
    const float    ufac  = (g_units == 0) ? 0.539957f : (g_units == 2 ? 0.621371f : 1.0f);
    const char    *uname = (g_units == 0) ? "nm" : (g_units == 2 ? "mi" : "km");
    String ropts;
    for (int r : ranges) {
        char o[72];
        snprintf(o, sizeof(o), "<option value=%d%s>%.0f %s</option>",
                 r, (r == (int)(g_settings.rangeKm + 0.5f)) ? " selected" : "", r * ufac, uname);
        ropts += o;
    }
    String topts;
    for (int i = 0; i < THEME_COUNT; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == th ? " selected" : "", radar::themeName(i));
        topts += o;
    }
    const int idleSecs[] = {10, 20, 30, 60, 120, 300, 1800, 3600, 7200, 14400, 28800};
    const int curIdle = (int)(g_idleDimMs / 1000);
    String iopts;
    for (int sV : idleSecs) {
        char lbl[16];
        if      (sV < 60)   snprintf(lbl, sizeof(lbl), "%d s", sV);
        else if (sV < 3600) snprintf(lbl, sizeof(lbl), "%d min", sV / 60);
        else                snprintf(lbl, sizeof(lbl), "%d h", sV / 3600);
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", sV, sV == curIdle ? " selected" : "", lbl);
        iopts += o;
    }
    { char o[64]; snprintf(o, sizeof(o), "<option value=0%s>Never</option>", curIdle == 0 ? " selected" : ""); iopts += o; }
    const char *unames[] = {"Aviation (ft, kt, nm)", "Metric (m, km/h, km)", "Imperial (ft, mph, mi)"};
    String uopts;
    for (int i = 0; i < 3; ++i) {
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_units ? " selected" : "", unames[i]);
        uopts += o;
    }
    const char *tlnames[] = {"Off", "Short", "Medium", "Long"};
    String tlopts;
    for (int i = 0; i < 4; ++i) {
        char o[64];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_trailLen ? " selected" : "", tlnames[i]);
        tlopts += o;
    }
    const int mxvals[] = {4, 6, 8, 10, 12};          // max aircraft on the scope (<= feed cap)
    String mxopts;
    for (int mv : mxvals) {
        char o[64];
        snprintf(o, sizeof(o), "<option value=%d%s>%d</option>", mv, mv == g_maxAc ? " selected" : "", mv);
        mxopts += o;
    }
    // minimum-altitude filter options (stored in ft; labels show ft + km for clarity)
    const struct { int ft; const char *lbl; } mavals[] = {
        {0, "Off"}, {5000, "&gt; 5,000 ft (1.5 km)"}, {10000, "&gt; 10,000 ft (3 km)"},
        {20000, "&gt; 20,000 ft (6 km)"}, {33000, "&gt; 33,000 ft (10 km)"},
    };
    String maopts;
    for (auto &mv : mavals) {
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", mv.ft, mv.ft == g_minAltFt ? " selected" : "", mv.lbl);
        maopts += o;
    }
    const char *anames[] = {"Off", "Emergencies only", "New aircraft + emergencies"};
    String aopts;
    for (int i = 0; i < 3; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_alertMode ? " selected" : "", anames[i]);
        aopts += o;
    }
    const int proxUnit[] = {0, 2, 5, 10, 25};   // 0 = off; rest in the user's distance unit
    String popts;
    for (int pv : proxUnit) {
        const float pkm = (pv == 0) ? 0.0f : (pv / ufac);   // user unit -> km (value submitted)
        const bool  sel = (pv == 0) ? (g_proximityKm <= 0.0f) : (fabsf(g_proximityKm - pkm) < 0.4f);
        char lbl[24];
        if (pv == 0) snprintf(lbl, sizeof(lbl), "Off");
        else         snprintf(lbl, sizeof(lbl), "%d %s", pv, uname);
        char o[80];
        snprintf(o, sizeof(o), "<option value=%.3f%s>%s</option>", pkm, sel ? " selected" : "", lbl);
        popts += o;
    }
    String tzopts;   // time-zone dropdown (value = index into TZOPTS; mapped to POSIX TZ on save)
    for (int i = 0; i < TZOPTS_N; ++i) {
        char o[128];
        snprintf(o, sizeof(o), "<option value=%d data-off=%d data-dst=%d%s>%s</option>",
                 i, TZOPTS[i].offMin, TZOPTS[i].dst, g_tz == TZOPTS[i].tz ? " selected" : "", TZOPTS[i].label);
        tzopts += o;
    }
    static const size_t BUFSZ = 10240;
    // PSRAM: keep this big page buffer off the scarce internal heap (the contiguous RAM
    // mbedTLS needs).
    //
    // Retried, and loud when it fails. This was `static char *buf = ps_malloc(BUFSZ)`
    // followed by `if (!buf) return;`, which is two faults in two lines. The bare return
    // sent NO HTTP RESPONSE, so the browser sat there until its own timeout and the device
    // looked wedged rather than short of memory. And `static` cached the failure: one
    // unlucky allocation — a themed Orb mid-app-switch is the easy way to get one — meant
    // this page never worked again until a reboot, however much PSRAM freed up afterwards.
    // Allocate on each attempt until one succeeds, and otherwise SAY what was missing.
    static char *buf = nullptr;
    if (!buf) buf = (char *)ps_malloc(BUFSZ);
    if (!buf) {
        char why[192];
        snprintf(why, sizeof(why),
                 "out of memory for this page: wanted %u B of PSRAM, %u KB free, "
                 "largest block %u KB", (unsigned)BUFSZ,
                 (unsigned)(ESP.getFreePsram() / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
        Serial.printf("[web] /legacy: %s\n", why);
        g_web.send(503, "text/plain", why);
        return;
    }
    snprintf(buf, BUFSZ,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>The Orb OS</title>"
        "<link rel=stylesheet href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
        "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{background:radial-gradient(circle at 50%% -10%%,#0a1f15,#04100a 70%%);color:#cdd6d1;"
        "font-family:system-ui,-apple-system,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        ".hd{display:flex;align-items:center;gap:12px;margin-bottom:16px}"
        ".dot{width:44px;height:44px;border-radius:50%%;border:2px solid #1dff86;position:relative;"
        "overflow:hidden;flex:0 0 auto;box-shadow:0 0 16px rgba(29,255,134,.4)}"
        ".dot::before{content:'';position:absolute;inset:0;animation:sw 3s linear infinite;"
        "background:conic-gradient(from 0deg,rgba(29,255,134,.65),transparent 55%%)}"
        "@keyframes sw{to{transform:rotate(360deg)}}"
        "h1{color:#1dff86;font-size:20px;margin:0}.sub{color:#6f8c7d;font-size:12px;margin:2px 0 0}"
        ".t{color:#1dff86;font-size:11px;letter-spacing:1.5px;text-transform:uppercase;margin-bottom:10px;opacity:.85}"
        "label{display:block;margin:12px 0 4px;color:#9affc8;font-size:13px}"
        "input,select{width:100%%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #2a4a39;"
        "background:#0c1a12;color:#eafff3;font-size:16px}"
        "input:focus,select:focus{outline:none;border-color:#1dff86;box-shadow:0 0 0 2px rgba(29,255,134,.18)}"
        "button{margin-top:16px;width:100%%;padding:12px;border:0;border-radius:8px;background:#1dff86;"
        "color:#04140b;font-weight:700;font-size:16px}button:active{opacity:.85}"
        ".w{background:#ffb23c}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px;margin-bottom:14px}"
        ".ft{color:#5f7a6c;font-size:12px;text-align:center;margin-top:6px}.ft code{color:#9affc8}"
        ".ck{width:auto;display:inline;margin-right:8px;vertical-align:middle}"
        ".sec{background:#0c1a12!important;color:#1dff86!important;border:1px solid #2a4a39!important}"
        "#map{height:220px;border-radius:10px;margin:6px 0 8px;border:1px solid #2a4a39;z-index:0}"
        "</style></head><body>"
        "<div class=hd><div class=dot></div><div><h1>The Orb OS</h1><p class=sub>Live ADS-B radar &middot; configuration</p></div></div>"
        "<div class=card><div class=t>Location &amp; range</div><form method=POST action=/save>"
        "<label>Center point &mdash; tap the map or drag the pin</label>"
        "<div id=map></div>"
        "<label>Center latitude</label><input id=lat name=lat value='%.5f'>"
        "<label>Center longitude</label><input id=lon name=lon value='%.5f'>"
        // A "Use GPS for location" checkbox sat here on the -G board. Gone with the input
        // itself: the device has two location sources and no more (UX-025), and a fix that
        // quietly moved a location the owner had typed in was the manual one not winning.
        "<label>Display range</label><select name=range>%s</select>"
        "<label>Theme</label><select name=theme>%s</select>"
        "<label>Time zone</label><select name=tz>%s</select>"
        "<button>Save &amp; restart</button></form></div>"
        "<div class=card><div class=t>Display</div>"
        "<label>Brightness</label>"
        "<input type=range min=5 max=255 value='%d' oninput='b(this.value,0)' onchange='b(this.value,1)'>"
        "<label>Dim screen after</label><select onchange='d(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='sw(this.checked)'>Show radar sweep</label>"
        "<label><input type=checkbox class=ck %s onchange='ap(this.checked)'>Show airports</label>"
        "<label><input type=checkbox class=ck %s onchange='hg(this.checked)'>Hide aircraft on the ground</label>"
        "<label>Minimum altitude</label><select onchange='ma(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='mo(this.checked)'>Military aircraft only</label>"
        "<label>Aircraft trails</label><select onchange='tl(this.value)'>%s</select>"
        "<label>Max aircraft on screen</label><select onchange='mx(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='bt(this.checked)'>Large text (restarts the device)</label>"
        "<label>Screen rotation (degrees clockwise)</label>"
        "<input type=number min=0 max=359 step=1 value='%d' onchange='ro(this.value)'>"
        "<label>Units</label><select onchange='u(this.value)'>%s</select></div>"
        "<div class=card><div class=t>Sound</div>"
        "<label>Volume</label>"
        "<input type=range min=0 max=100 value='%d' oninput='v(this.value,0)' onchange='v(this.value,1)'>"
        "<label><input type=checkbox class=ck %s onchange='m(this.checked)'>Mute alerts</label>"
        "<label>Alert on</label><select onchange='al(this.value)'>%s</select>"
        "<label>Proximity alert</label><select onchange='px(this.value)'>%s</select>"
        "<button type=button class=sec onclick='t()'>Test ping</button></div>"
        "<div class=card><div class=t>Network</div>"
        "<p style='color:#9affc8;font-size:13px;margin:0 0 4px'>Forget the saved WiFi and reopen the setup portal.</p>"
        "<form method=POST action=/wifi><button class=w>Reset WiFi</button></form></div>"
        // The "Firmware update" link only exists when there is an OTA partition to write
        // into; otherwise it would advertise a page that 404s.
#if ORB_OTA_ENABLED
        "<p class=ft>Reach me at <code>" ORB_MDNS_ADDR "</code> &middot; <a href=/update style='color:#9affc8'>Firmware update</a> &middot; v" FW_VERSION "</p>"
#else
        "<p class=ft>Reach me at <code>" ORB_MDNS_ADDR "</code> &middot; Update over USB &middot; v" FW_VERSION "</p>"
#endif
        "<script>"
        "var C=[%.5f,%.5f];var MAP=L.map('map').setView(C,10);"
        "L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'(c) OpenStreetMap'}).addTo(MAP);"
        "var MK=L.marker(C,{draggable:true}).addTo(MAP);"
        "function S(p){document.getElementById('lat').value=p.lat.toFixed(5);document.getElementById('lon').value=p.lng.toFixed(5);}"
        "MK.on('dragend',function(){S(MK.getLatLng());});"
        "MAP.on('click',function(e){MK.setLatLng(e.latlng);S(e.latlng);});"
        "setTimeout(function(){MAP.invalidateSize();},300);"
        "function b(v,s){fetch('/bright?v='+v+(s?'&save=1':''))}"
        "function v(x,s){fetch('/vol?v='+x+(s?'&save=1':''))}"
        "function m(c){fetch('/vol?mute='+(c?1:0)+'&save=1')}"
        "function t(){fetch('/vol?test=1')}"
        "function d(v){fetch('/idle?v='+v+'&save=1')}"
        "function sw(c){fetch('/sweep?v='+(c?1:0)+'&save=1')}"
        "function ap(c){fetch('/airports?v='+(c?1:0)+'&save=1')}"
        "function hg(c){fetch('/ground?v='+(c?1:0)+'&save=1')}"
        "function ma(v){fetch('/altmin?v='+v+'&save=1')}"
        "function mo(c){fetch('/milonly?v='+(c?1:0)+'&save=1')}"
        "function tl(v){fetch('/trail?v='+v+'&save=1')}"
        "function mx(v){fetch('/maxac?v='+v+'&save=1')}"
        "function bt(c){fetch('/bigtext?v='+(c?1:0)+'&save=1')}"
        "function ro(v){fetch('/rotate?v='+v+'&save=1')}"
        "function u(v){fetch('/units?v='+v+'&save=1')}"
        "function al(v){fetch('/alerts?mode='+v+'&save=1')}"
        "function px(v){fetch('/alerts?prox='+v+'&save=1')}"
        // auto-pick the visitor's time zone from their browser clock (only if they haven't set one)
        "var TZSET=%d;(function(){if(TZSET)return;"
        "var d=new Date(),j=new Date(d.getFullYear(),0,1).getTimezoneOffset(),"
        "u=new Date(d.getFullYear(),6,1).getTimezoneOffset(),o=-Math.max(j,u),s=(j!=u)?1:0,"
        "e=document.querySelector('select[name=tz]'),b=-1,i;"
        "for(i=0;i<e.options.length;i++){if(+e.options[i].dataset.off===o&&+e.options[i].dataset.dst===s){b=i;break;}}"
        "if(b<0)for(i=0;i<e.options.length;i++){if(+e.options[i].dataset.off===o){b=i;break;}}"
        "if(b>=0)e.selectedIndex=b;})();</script></body></html>",
        g_settings.homeLat, g_settings.homeLon, ropts.c_str(), topts.c_str(),
        tzopts.c_str(),
        g_brightnessDay, iopts.c_str(), g_showSweep ? "checked" : "",
        g_showAirports ? "checked" : "", g_hideGround ? "checked" : "", maopts.c_str(), g_milOnly ? "checked" : "",
        tlopts.c_str(), mxopts.c_str(), g_bigText ? "checked" : "", g_rotation, uopts.c_str(),
        g_volume, g_muted ? "checked" : "", aopts.c_str(), popts.c_str(),
        g_settings.homeLat, g_settings.homeLon, (g_tz == TZ_STR ? 0 : 1));
    g_web.send(200, "text/html", buf);
}

// Nothing here used to say anything, in either direction. The page answered "Saved.
// Restarting..." and restarted whether or not a single value had been written, so a save
// that silently did nothing was indistinguishable from one that worked, and the only clue
// was the setting still being on its old value afterwards.
//
// Every branch reports itself now, and putDouble's return is actually read: it is a byte
// count, and zero means the store refused the write.
static void handleSave() {
    Preferences p;
    if (!p.begin("capsuleradar", false)) {
        Serial.println("[web] save: the settings store would not open; nothing written");
        g_web.send(500, "text/plain", "settings store unavailable");
        return;
    }
    Serial.printf("[web] save: %d argument(s)\n", g_web.args());
    for (int i = 0; i < g_web.args(); ++i)
        Serial.printf("[web]   %s = %s\n", g_web.argName(i).c_str(), g_web.arg(i).c_str());
    // Reject out-of-range coordinates so a typo can't leave the radar unusable.
    if (g_web.hasArg("lat")) {
        const double lat = g_web.arg("lat").toDouble();
        if (lat >= -90.0 && lat <= 90.0) {
            const size_t n = p.putDouble("homeLat", lat);
            Serial.printf("[web] save: homeLat=%.5f -> %u bytes\n", lat, (unsigned)n);
        } else {
            Serial.printf("[web] save: homeLat=%.5f out of range, ignored\n", lat);
        }
    }
    if (g_web.hasArg("lon")) {
        const double lon = g_web.arg("lon").toDouble();
        if (lon >= -180.0 && lon <= 180.0) {
            const size_t n = p.putDouble("homeLon", lon);
            Serial.printf("[web] save: homeLon=%.5f -> %u bytes\n", lon, (unsigned)n);
        } else {
            Serial.printf("[web] save: homeLon=%.5f out of range, ignored\n", lon);
        }
    }
    if (g_web.hasArg("range")) p.putFloat("rangeKm", g_web.arg("range").toFloat());
    if (g_web.hasArg("theme")) p.putInt("theme", g_web.arg("theme").toInt());
    if (g_web.hasArg("tz")) {
        const int i = g_web.arg("tz").toInt();
        if (i >= 0 && i < TZOPTS_N) p.putString("tz", TZOPTS[i].tz);
    }
    p.end();
    // The warning that used to be built here is gone with the thing it warned about. A
    // design could PIN the location, loadSettings() re-applied the pin on every boot, and
    // the coordinates typed into this box were written and immediately outranked — so this
    // page had to say "Saved" and then take it back in the next paragraph. Location is the
    // owner's now, no installed design can outrank it, and "Saved" is simply true.
    g_web.send(200, "text/html",
               "<meta http-equiv=refresh content='6;url=/'><body style='background:#06100a;"
               "color:#1dff86;font-family:sans-serif;padding:24px'>Saved. Restarting&hellip;</body>");
    delay(400);
    ESP.restart();
}

static void handleWifi() {
    g_web.send(200, "text/html",
        "<body style='background:#06100a;color:#ffb23c;font-family:sans-serif;padding:24px'>"
        "WiFi reset. Connect to the <b>The Orb Setup</b> network to reconfigure.</body>");
    delay(400);                     // let the response reach the browser
    // The driver stores the saved AP in its own NVS namespace ("nvs.net80211"). On Arduino
    // core 3.x both wm.resetSettings() and WiFi.disconnect(true,true) can silently no-op
    // (they fail once the driver is off), so v1.3.19's reset still reconnected. Erasing that
    // namespace directly is unconditional — it works whatever state the WiFi driver is in.
    g_wm.resetSettings();           // best-effort driver-level erase first...
    WiFi.disconnect(false, true);   // ...keep WiFi up so the erase can actually run
    delay(100);
    nvs_handle_t h;                 // ...then the guaranteed path: wipe the driver's namespace
    if (nvs_open("nvs.net80211", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    delay(300);                     // let NVS finish committing before the reboot
    ESP.restart();
}

static void handleBright() {
    if (g_web.hasArg("v")) {
        g_brightnessDay = constrain((int)g_web.arg("v").toInt(), 0, 255);
        applyBrightness();
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("bright", g_brightnessDay);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleVol() {
    if (g_web.hasArg("v"))    { g_volume = constrain((int)g_web.arg("v").toInt(), 0, 100); audio_set_volume(g_volume); }
    if (g_web.hasArg("mute")) { g_muted = g_web.arg("mute").toInt() != 0; audio_set_muted(g_muted); }
    if (g_web.hasArg("save")) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("vol", g_volume);
        p.putBool("mute", g_muted);
        p.end();
    }
    if (g_web.hasArg("test")) {
        if (g_web.arg("test").toInt() == 2) audio_selftest();   // long tone, ignores mute
        else audio_play(AUDIO_NEW);
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAlerts() {   // what triggers the alert sound (live)
    if (g_web.hasArg("mode")) g_alertMode   = constrain((int)g_web.arg("mode").toInt(), 0, 2);
    if (g_web.hasArg("prox")) {
        g_proximityKm = g_web.arg("prox").toFloat();   // km (0 = off)
        g_requeryKm = queryRadiusKm();                 // the query must cover the new alert circle
        g_requery = true;
    }
    if (g_web.hasArg("save")) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("alertmode", g_alertMode);
        p.putFloat("proxkm", g_proximityKm);
        p.end();
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleIdle() {   // idle auto-dim timeout (seconds; 0 = never)
    if (g_web.hasArg("v")) {
        const long s = g_web.arg("v").toInt();
        g_idleDimMs = (s <= 0) ? 0 : (uint32_t)s * 1000;
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putUInt("idledim", g_idleDimMs);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleUnits() {   // measurement units preset (live re-render)
    if (g_web.hasArg("v")) {
        g_units = constrain((int)g_web.arg("v").toInt(), 0, 2);
        ui_set_units(g_units);
        ui_on_data_updated();                  // re-render card/list/stats in the new units
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("units", g_units);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleSweep() {   // show/hide the rotating sweep line (live)
    if (g_web.hasArg("v")) {
        g_showSweep = g_web.arg("v").toInt() != 0;
        radar::setSweepEnabled(g_showSweep);          // loop()/core 1: safe to touch LVGL
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("sweep", g_showSweep);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleTrail() {   // aircraft trail length 0/1/2/3 (live)
    if (g_web.hasArg("v")) {
        g_trailLen = constrain((int)g_web.arg("v").toInt(), 0, 3);
        radar::setTrailLength(g_trailLen);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("traillen", g_trailLen);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAltMin() {   // minimum-altitude feed filter, ft (applies from the next poll)
    if (g_web.hasArg("v")) {
        g_minAltFt = constrain((int)g_web.arg("v").toInt(), 0, 60000);
        g_adsb.setMinAltFt((float)g_minAltFt);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("minalt", g_minAltFt);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleMilOnly() {   // military-only feed filter (applies from the next poll)
    if (g_web.hasArg("v")) {
        g_milOnly = g_web.arg("v").toInt() != 0;
        g_adsb.setMilitaryOnly(g_milOnly);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("milonly", g_milOnly);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleBigText() {   // accessibility: large fonts. Fonts are baked at UI creation,
    if (g_web.hasArg("v")) {    // so persist the flag and reboot cleanly to apply it.
        g_bigText = g_web.arg("v").toInt() != 0;
        Preferences p;
        p.begin("capsuleradar", false);
        p.putBool("bigtext", g_bigText);
        p.end();
        g_rebootAtMs = millis() + 1200;   // let this response reach the browser first
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleMaxAc() {   // max aircraft drawn on the scope (live)
    if (g_web.hasArg("v")) {
        g_maxAc = constrain((int)g_web.arg("v").toInt(), 1, ADSB_MAX_AIRCRAFT);
        radar::setMaxOnScreen(g_maxAc);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("maxac", g_maxAc);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAirports() {   // show/hide airport markers (live)
    if (g_web.hasArg("v")) {
        g_showAirports = g_web.arg("v").toInt() != 0;
        radar::setAirportsEnabled(g_showAirports);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("airports", g_showAirports);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleGround() {   // hide/show on-ground aircraft (applies from the next feed poll)
    if (g_web.hasArg("v")) {
        g_hideGround = g_web.arg("v").toInt() != 0;
        g_adsb.setHideGround(g_hideGround);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("hideground", g_hideGround);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleRotate() {   // arbitrary clockwise display rotation, applied live
    if (g_web.hasArg("v")) {
        g_rotation = constrain((int)g_web.arg("v").toInt(), 0, 359);
        display::setRotation((uint16_t)g_rotation);
        g_rotation = display::rotation();
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("rotDeg", g_rotation);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

#if ORB_OTA_ENABLED
// ---- browser OTA: upload an app .bin over WiFi and self-flash ----
static void handleUpdatePage() {
    g_web.send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>The Orb OS - Update</title><style>"
        "body{background:radial-gradient(circle at 50% -10%,#0a1f15,#04100a 70%);color:#cdd6d1;"
        "font-family:system-ui,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        "h1{color:#1dff86;font-size:20px}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px}"
        "input,button{width:100%;box-sizing:border-box;padding:11px;border-radius:8px;margin-top:8px;font-size:16px}"
        "input{background:#0c1a12;color:#eafff3;border:1px solid #2a4a39}"
        "button{border:0;background:#1dff86;color:#04140b;font-weight:700}"
        "#bar{height:12px;background:#0c1a12;border-radius:6px;overflow:hidden;margin-top:14px;display:none}"
        "#fill{height:100%;width:0;background:#1dff86;transition:width .2s}#msg{margin-top:10px;color:#9affc8;font-size:13px}"
        "a{color:#1dff86}p{color:#9affc8;font-size:13px}"
        "</style></head><body><h1>Firmware update (OTA)</h1><div class=card>"
        "<p>Upload the <b>app firmware</b> <code>TheOrbOS-ota.bin</code> from the GitHub release. "
        "Do NOT use the merged flash image here.</p>"
        "<input type=file id=f accept='.bin'>"
        "<button onclick=u()>Update over WiFi</button>"
        "<div id=bar><div id=fill></div></div><div id=msg></div></div>"
        "<p style='text-align:center;margin-top:14px'><a href=/>&larr; Back to settings</a></p>"
        "<script>function u(){var f=document.getElementById('f').files[0];if(!f){return}"
        "var x=new XMLHttpRequest(),fd=new FormData();fd.append('f',f);"
        "document.getElementById('bar').style.display='block';"
        "x.upload.onprogress=function(e){if(e.lengthComputable)document.getElementById('fill').style.width=(e.loaded/e.total*100)+'%'};"
        "x.onload=function(){document.getElementById('msg').innerText=x.responseText+' - rebooting...'};"
        "x.onerror=function(){document.getElementById('msg').innerText='Upload failed'};"
        "x.open('POST','/update');x.send(fd);}</script></body></html>");
}

static void handleUpdateUpload() {
    HTTPUpload &up = g_web.upload();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[update] start: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("[update] done: %u bytes\n", (unsigned)up.totalSize);
        else Update.printError(Serial);
    }
}
#endif  // ORB_OTA_ENABLED

// ---- theme file upload over WiFi -----------------------------------------
// POST /sdput?path=/themes/<slug>/<file>, multipart body, streamed to the microSD in
// chunks. Theme art no longer lives in the firmware binary (it stopped fitting: a
// detailed design needs several MB against a ~944 KB flash budget), so it has to reach
// the card some other way. The alternative was pulling the card out and using a reader
// on every push. The Orb is already on WiFi with a web server running, so Launch Kit
// POSTs the files here instead.
//
// No concurrency guard needed: g_web.handleClient() and every screen's SD decode both
// run inside loop() on core 1, so card access is serialised by construction. adsb_task
// (core 0) never touches SD.
static File   g_sdUpFile;
static bool   g_sdUpOk = false;
static String g_sdUpPath;

// Confined to /themes/. A malformed or hostile request must not be able to scribble
// over Spy Cam clips, road tiles, or anything else living on the card.
static bool sd_put_path_ok(const String &p) {
    return p.startsWith("/themes/") && p.indexOf("..") < 0 && p.length() > 8 && p.length() < 96;
}

// The Orb's own install page: pick a .orb file, watch it land on the card.
//
// This exists because Studio CANNOT push over the network. Studio is served over HTTPS, the
// Orb can only ever speak HTTP (there is not enough contiguous memory here for TLS), and a
// browser refuses to let a secure page call an insecure address. Turning the direction round
// solves it completely: an HTTP page on the Orb, talking to the Orb, upsets nobody. It also
// means an iPad or a phone can install a theme, which the USB cable can never do.
//
// The unpacking happens HERE, in the browser, not in C++. The file is a trivial container
// (see orb-bundle.ts) and the page POSTs its contents one at a time to /sdput, which already
// streams to the card. So a device with 320 KB of RAM never parses an archive, never holds a
// theme in memory, and this whole feature costs one static page and no new upload code.
static const char INSTALL_PAGE[] PROGMEM = R"HTML(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>My Orb</title><style>
*{box-sizing:border-box}
body{font:16px system-ui;margin:0;padding:22px 18px 60px;background:#101418;color:#e8edf2}
h1{font-size:20px;margin:0 0 2px}h2{font-size:12px;letter-spacing:.14em;text-transform:uppercase;color:#6a7480;margin:26px 0 10px}
p{color:#98a2ad;margin:4px 0 16px;line-height:1.5}
.row{display:flex;align-items:center;gap:10px;padding:12px 14px;border:1px solid #29323b;border-radius:12px;margin-bottom:8px;background:#161c22}
.row.on{border-color:#3c6338;background:#16211a}
.nm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tag{font-size:11px;padding:2px 8px;border-radius:999px;background:#22303a;color:#8fb98a;white-space:nowrap}
button,label.btn{font:14px system-ui;padding:7px 12px;border:1px solid #3a444f;border-radius:9px;background:none;color:#c7ced6;cursor:pointer}
button:hover,label.btn:hover{border-color:#e05a3a;color:#fff}
button:disabled{opacity:.4;cursor:default}
label.btn{display:inline-block}input[type=file]{display:none}
#log{margin-top:14px;font:13px ui-monospace,monospace;white-space:pre-wrap;color:#98a2ad}
.ok{color:#7ddb8a}.bad{color:#ff8a6a}
</style>
<h1>My Orb</h1>
<p id=sub>Loading…</p>

<h2>Install a theme</h2>
<p>Download a theme from Orb Studio, then choose the file here.</p>
<label class=btn>Choose a .orb file<input type=file accept=".orb" id=f></label>
<div id=log></div>

<h2>On this Orb</h2>
<div id=list></div>

<script>
const $=s=>document.querySelector(s), log=$('#log');
const say=(m,c)=>{const d=document.createElement('div');if(c)d.className=c;d.textContent=m;log.appendChild(d)};

async function refresh(){
 const r=await fetch('/themes.json',{cache:'no-store'}); const d=await r.json();
 const box=$('#list'); box.textContent='';
 for(const t of d.themes){
  const worn = t.slug===d.active;
  const row=document.createElement('div'); row.className='row'+(worn?' on':'');
  const nm=document.createElement('span'); nm.className='nm'; nm.textContent=t.name; row.appendChild(nm);
  if(worn){ const g=document.createElement('span'); g.className='tag'; g.textContent='Wearing this'; row.appendChild(g); }
  else{
   const w=document.createElement('button'); w.textContent='Wear'; w.onclick=async()=>{
    w.disabled=true; await fetch('/theme?slug='+encodeURIComponent(t.slug));
    // The Orb reboots into the theme, so there is nothing useful to wait for here.
    nm.textContent=t.name+' — switching, the Orb is restarting';
   }; row.appendChild(w);
   const x=document.createElement('button'); x.textContent='Delete'; x.onclick=async()=>{
    if(x.textContent==='Delete'){ x.textContent='Really delete?'; return; }
    x.disabled=true;
    const rr=await fetch('/themedel?slug='+encodeURIComponent(t.slug),{method:'POST'});
    if(rr.ok) refresh(); else { x.textContent=await rr.text(); }
   }; row.appendChild(x);
  }
  box.appendChild(row);
 }
 $('#sub').textContent=d.themes.length+(d.themes.length===1?' theme':' themes')+' on the card. It cannot delete the one it is wearing — switch first.';
}
refresh();

$('#f').onchange=async e=>{
 const file=e.target.files[0]; if(!file) return; log.textContent='';
 try{
  const buf=new Uint8Array(await file.arrayBuffer());
  const v=new DataView(buf.buffer),dec=new TextDecoder();
  if(dec.decode(buf.subarray(0,8))!=='ORBTHM01') throw new Error('That is not an Orb theme file.');
  let at=8;
  const sl=v.getUint16(at,true);at+=2;
  const slug=dec.decode(buf.subarray(at,at+sl));at+=sl;
  const n=v.getUint16(at,true);at+=2;
  const items=[];
  for(let i=0;i<n;i++){
   const nl=v.getUint16(at,true);at+=2;
   const name=dec.decode(buf.subarray(at,at+nl));at+=nl;
   const dl=v.getUint32(at,true);at+=4;
   items.push({name,data:buf.subarray(at,at+dl)});at+=dl;
  }
  // _installed LAST, always. The Orb only counts a folder that has it, so a transfer that
  // dies halfway leaves an invisible folder rather than a half-broken theme in the picker.
  items.sort((a,b)=>(a.name==='_installed')-(b.name==='_installed'));
  say('Installing '+items.length+' files');
  for(let i=0;i<items.length;i++){
   const it=items[i];
   const fd=new FormData();
   fd.append('f',new Blob([it.data]),it.name);
   const r=await fetch('/sdput?path=/themes/'+encodeURIComponent(slug)+'/'+encodeURIComponent(it.name),{method:'POST',body:fd});
   if(!r.ok) throw new Error(it.name+' failed to write ('+r.status+')');
   say((i+1)+'/'+items.length+'  '+it.name);
  }
  // Tell the Orb the send is over. It cannot work this out for itself: a finished
  // transfer and an abandoned one both end with files simply stopping, and without this
  // the device sat for twelve seconds and then announced "Update interrupted" over an
  // install that had completely succeeded.
  try { await fetch('/installed',{method:'POST'}); } catch(e) {}
  say('Done.','ok'); refresh();
 }catch(err){ say(String(err.message||err),'bad'); }
};
</script>)HTML";

static void handleSdPutUpload() {
    HTTPUpload &up = g_web.upload();
    if (up.status == UPLOAD_FILE_START) {
        g_sdUpOk = false;
        g_sdUpPath = g_web.arg("path");
        if (!sdcard::mounted()) { Serial.println("[sdput] no card mounted"); return; }
        if (!sd_put_path_ok(g_sdUpPath)) { Serial.printf("[sdput] rejected path '%s'\n", g_sdUpPath.c_str()); return; }
        // Create every missing level, not just the last one. SD.mkdir() does not create
        // intermediate directories, so on a card that has never held a theme the whole
        // path fails at "/themes" and every upload dies with a bare open failure.
        for (int i = 1; i < (int)g_sdUpPath.length(); ++i) {
            if (g_sdUpPath[i] != '/') continue;
            String part = g_sdUpPath.substring(0, i);
            if (!SD.exists(part) && !SD.mkdir(part)) {
                Serial.printf("[sdput] mkdir failed: %s\n", part.c_str());
                return;
            }
        }
        g_sdUpFile = SD.open(g_sdUpPath.c_str(), FILE_WRITE);   // truncates any existing file
        if (!g_sdUpFile) {
            Serial.printf("[sdput] open failed: %s (card %llu MB, writable?)\n",
                          g_sdUpPath.c_str(), (unsigned long long)(sdcard::sizeBytes() / (1024ULL * 1024ULL)));
            return;
        }
        g_sdUpOk = true;
        Serial.printf("[sdput] start %s\n", g_sdUpPath.c_str());
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (g_sdUpOk && g_sdUpFile.write(up.buf, up.currentSize) != up.currentSize) {
            g_sdUpOk = false;
            Serial.println("[sdput] short write (card full or removed?)");
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (g_sdUpFile) g_sdUpFile.close();
        if (g_sdUpOk) {
            Serial.printf("[sdput] done %s (%u bytes)\n", g_sdUpPath.c_str(), (unsigned)up.totalSize);
            // Tell the user the device is mid-update. Without this, files arrived in
            // silence and the reboot that follows read as a crash or a stale load.
            const int slash = g_sdUpPath.lastIndexOf('/');
            update_ui::file_received(g_sdUpPath.c_str() + (slash >= 0 ? slash + 1 : 0), ++g_sdUpCount);
        }
    }
}

static void handleSdPutDone() {
    g_web.send(g_sdUpOk ? 200 : 500, "text/plain", g_sdUpOk ? "ok" : "failed");
}


void web_server_begin() {
    // configuration web page (http://theorb.local/)
    g_web.on("/diag", []{ g_web.send(200, "text/plain", diag::text()); });
    // Does the hand-layer cache draw the same picture as a full compose? In pixels.
    //
    // Over HTTP because the answer takes two full composes (about a second on a heavy
    // design) and because "it looks right" is not a measurement — a layer that came out
    // empty and a layer that is simply mostly transparent produce the same phase timing.
    g_web.on("/layercheck", []{
        int worst = -1, wx = -1, wy = -1;
        const long differ = clockview::layerDiffersBy(&worst, &wx, &wy);
        char b[220];
        if (differ == -2)
            snprintf(b, sizeof(b), "{\"ok\":false,\"why\":\"layers not in use\"}");
        else if (differ < 0)
            snprintf(b, sizeof(b), "{\"ok\":false,\"why\":\"not a custom face, or no memory\"}");
        else
            snprintf(b, sizeof(b),
                     "{\"ok\":true,\"pixels_differing\":%ld,\"of\":%ld,\"worst_levels\":%d,"
                     "\"worst_at\":[%d,%d]}",
                     differ, (long)(466L * 466L), worst, wx, wy);
        g_web.send(200, "application/json", b);
    });
    // Per-task and heap-fragmentation detail, added for the 2026-08-22 investigation into
    // why ADS-B reads start timing out a minute or two into Flight Tracker. /health already
    // gives one internal-heap number; this is who is holding the rest of it, and how broken
    // up what remains actually is (a device can report 8 KB free and still be unable to
    // satisfy a 2 KB request if that 8 KB is forty scattered 200-byte crumbs).
    g_web.on("/taskmem", []{
        multi_heap_info_t hi;
        heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
        char b[900];   // the theme-config fields pushed it past 640
        snprintf(b, sizeof(b),
            "{\"internal\":{\"free_bytes\":%u,\"largest_free_block\":%u,"
            "\"free_blocks\":%u,\"allocated_blocks\":%u,\"total_blocks\":%u},"
            "\"stack_free_bytes\":{\"adsb\":%u,\"audio\":%u,\"loop\":%u},"
            "\"radar_active\":%s}",
            (unsigned)hi.total_free_bytes, (unsigned)hi.largest_free_block,
            (unsigned)hi.free_blocks, (unsigned)hi.allocated_blocks,
            (unsigned)(hi.free_blocks + hi.allocated_blocks),
            (unsigned)(g_adsbTaskHandle ? uxTaskGetStackHighWaterMark(g_adsbTaskHandle) : 0),
            (unsigned)audio_stack_free_bytes(),
            (unsigned)uxTaskGetStackHighWaterMark(nullptr),   // nullptr = the calling (loop) task
            g_radarViewActive ? "true" : "false");
        g_web.send(200, "application/json", b);
    });
    // Reboot on request. Added because capturing the boot log (the only place the PSRAM
    // ledger prints) otherwise means physically unplugging the device, and the serial
    // port cannot be held open during a flash anyway. Deliberately delayed so the HTTP
    // response reaches the caller first, same pattern as the settings handlers above.
    // Force a clean re-bake of the active theme's art, then restart into it.
    //
    // Why this has to exist: baked art is only ever dropped for the slug being re-baked, so
    // a theme the owner deleted from the card keeps its flash for ever, and the automatic
    // clean slate in install_begin() only fires once the remainder is already under 4 MB.
    // A rich theme arriving next to a stale one therefore loses whatever bakes LAST — and
    // what bakes last is the background animation frames, which have no SD fallback at all.
    // Measured on Steam Punk, 2026-10-06: partition 97.4% full, 252 KB free against 424 KB
    // per frame, the hand shadows falling back to SD and the frames simply absent.
    //
    // POST, not GET: it erases every baked asset on the device and takes a minute of
    // decoding, so a link preview or a crawler must not be able to set it off.
    g_web.on("/rebake", HTTP_POST, []{
        Serial.println("[theme_art] /rebake: requested; will run at the next boot");
        {   // Remembered, not done now. See the note in loop().
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("rebake", true);
            p.end();
        }
        g_web.send(200, "text/plain",
                   "the Orb will restart and re-bake the active theme during boot; "
                   "give it a minute, then check bg_frames_baked in /health");
        g_rebootAtMs = millis() + 400;
    });
    // What the active theme actually HAS, declares, and got baked — the three-way comparison
    // that tools/read-orb-bundle.py does for a .orb file, for a theme already on the card.
    //
    // Needed because a theme pushed straight from Orb Studio over the cable leaves no bundle
    // behind to inspect, and the bake skips a declared asset in SILENCE when the file is not
    // readable. "frames: 8 declared, 0 baked, 8.5 MB free" is not explicable from the device
    // without this: it could be a missing file, an undeclared one, or a decode that failed.
    //
    // declared = theme.json's asset list said so (nothing undeclared is ever baked).
    // baked    = it is in themeart now, which is the only place frames are read from.
    g_web.on("/themefiles", []{
        const char *slug = theme_select::activeSlug();
        char dir[64];
        snprintf(dir, sizeof(dir), "/themes/%s", slug);
        g_web.setContentLength(CONTENT_LENGTH_UNKNOWN);
        g_web.send(200, "text/plain", "");
        char line[160];
        snprintf(line, sizeof(line), "slug %s\n%-28s %9s  declared  baked\n", slug, "file", "bytes");
        g_web.sendContent(line, strlen(line));
        File d = SD.open(dir);
        if (!d || !d.isDirectory()) {
            const char *err = "(cannot open the theme directory)\n";
            g_web.sendContent(err, strlen(err));
            g_web.sendContent("", 0);
            return;
        }
        int n = 0;
        for (File f = d.openNextFile(); f; f = d.openNextFile()) {
            const char *nm = f.name();
            const int slash = (int)strlen(nm);
            const char *leaf = nm;
            for (int i = slash - 1; i >= 0; --i) if (nm[i] == '/') { leaf = nm + i + 1; break; }
            snprintf(line, sizeof(line), "%-28s %9lu  %-8s  %s\n", leaf,
                     (unsigned long)f.size(),
                     theme_style::hasAsset(leaf) ? "yes" : "NO",
                     theme_art::has(slug, leaf)  ? "yes" : "NO");
            g_web.sendContent(line, strlen(line));
            f.close();
            ++n;
        }
        d.close();
        snprintf(line, sizeof(line), "%d file(s)\n", n);
        g_web.sendContent(line, strlen(line));
        g_web.sendContent("", 0);
    });
    g_web.on("/reboot", []{
        g_web.send(200, "text/plain", "rebooting");
        update_ui::rebooting();     // no-op unless the update overlay is up
        g_rebootAtMs = millis() + 400;
    });
    // Switch the active theme, e.g. /theme?slug=modern. Until this existed, pushing a
    // theme from Launch Kit copied its files onto the card but left the device running
    // whichever theme it was already on, so "push Modern to the Orb" produced the old
    // theme's artwork wearing the new theme's compiled fonts. Settings > Design was the
    // only way to actually select one, which is a menu dive for something the push
    // already knows. theme_select::set() persists to NVS and reboots by itself.
    g_web.on("/theme", []{
        const String slug = g_web.arg("slug");
        if (!slug.length()) { g_web.send(400, "text/plain", "missing slug"); return; }
        // Only switch to a theme that is genuinely on the card; a typo would otherwise
        // leave the device pointing at an empty folder and drawing stock art.
        static char slugs[theme_select::MAX_THEMES][theme_select::MAX_SLUG_LEN];
        const int n = theme_select::listInstalled(slugs);
        bool found = false;
        for (int i = 0; i < n && !found; ++i) found = (slug == slugs[i]);
        if (!found) { g_web.send(404, "text/plain", "no such theme on the card"); return; }

        g_web.send(200, "text/plain", "switching");
        Serial.printf("[theme] switching to '%s' by request\n", slug.c_str());
        // Applied from loop(), not here: theme_select::set() reboots, which would cut the
        // HTTP response off before the client ever saw it.
        g_pendingSlug   = slug;
        g_applySlugAtMs = millis() + 400;
    });
    // Diagnostic: hide/show one radar layer live, so each layer's per-frame cost can be
    // priced by watching fps change instead of reflashing once per hypothesis. Nothing
    // persists it; a reboot or re-entering the app puts every layer back.
    g_web.on("/rdbg", []{
        if (g_web.hasArg("poll")) {
            g_pollOverrideMs = (uint32_t)g_web.arg("poll").toInt();
            Serial.printf("[adsb] poll interval override -> %lu ms\n", (unsigned long)g_pollOverrideMs);
            g_web.send(200, "text/plain", "poll set");
            return;
        }
        // Tear-free flush on/off, and the staged flush itself. These are the two that address
        // what can actually be SEEN; everything else on this endpoint prices a layer.
        if (g_web.hasArg("te")) {
            display_set_te(g_web.arg("te") != "0");
            g_web.send(200, "text/plain", display_te() ? "te on" : "te off");
            return;
        }
        if (g_web.hasArg("stage")) {
            display_set_stage(g_web.arg("stage") != "0");
            g_web.send(200, "text/plain", display_stage() ? "staged" : "per-strip");
            return;
        }
        // The fan's line count, mirrored from the cable's `?orb trailsteps`, because the cable is
        // exactly what is unavailable when the Orb is diagnosed from a machine with no COM port.
        // Measured the biggest single lever on the Flight Tracker's frame rate.
        if (g_web.hasArg("trailsteps")) {
            const int n = (int)g_web.arg("trailsteps").toInt();
            radar::setTrailSteps(n);
            g_web.send(200, "text/plain", n ? String("trailSteps=") + n : String("trailSteps=design"));
            return;
        }
        if (g_web.hasArg("blipaa")) {
            const bool on = (g_web.arg("blipaa") != "0");
            radar::setBlipAA(on ? 1 : 0);
            g_web.send(200, "text/plain", on ? "blip aa on" : "blip aa off");
            return;
        }
        if (g_web.hasArg("pace")) {
            radar::setPacePct((int)g_web.arg("pace").toInt());
            g_web.send(200, "text/plain", String("pace=") + g_web.arg("pace") + "%");
            return;
        }
        if (g_web.hasArg("loopdelay")) {
            g_loopDelayMs = (uint32_t)constrain(g_web.arg("loopdelay").toInt(), 1L, 20L);
            Serial.printf("[loop] delay -> %lu ms\n", (unsigned long)g_loopDelayMs);
            g_web.send(200, "text/plain", String("loopdelay=") + g_loopDelayMs);
            return;
        }
        const int  kind = g_web.arg("layer").toInt();
        const bool hide = (g_web.arg("hide") != "0");
        radar::debugHideLayer(kind, hide);
        g_web.send(200, "text/plain", hide ? "hidden" : "shown");
    });
    // Machine-readable device state, so diagnosis starts from facts instead of from a
    // photograph of the screen (workflow rule R2). largest_block matters as much as
    // free: an allocation can fail with plenty of total free PSRAM if churn has
    // fragmented it below the requested size.
    g_web.on("/health", []{
        // 1024, not 640, and the return value is CHECKED rather than trusted.
        //
        // A bigger buffer was never the fix. The note that raised this to 640 said exactly
        // what happens when the field list outgrows it -- "snprintf would have silently
        // truncated the JSON into something no client could parse" -- and on 2026-10-06 it
        // did, at 639 bytes, the moment a handful of display fields were added. A reading off
        // Zion's Orb on 2026-10-07 was 587 bytes, so 640 had 53 to spare against five compose
        // fields that are 70: it would have truncated on the first device to report them.
        //
        // Raising the number only moves the cliff. snprintf returns what it WOULD have
        // written and nothing looked, so the next field added breaks every reader in silence
        // -- including the readers diagnosing something else entirely, which is what makes a
        // truncated /health so expensive. So the length is tested below.
        char b[1024];
        float cFace = 0, cPlate = 0, cText = 0, cHands = 0;
        clockview::composeCost(cFace, cPlate, cText, cHands);
        // The CADENCE of the sweeping hand, as measured between wakes rather than as the
        // timer intended them. Added with 2.16.72's fix because the four diagnoses that
        // preceded it were all argued from what the code said rather than from what the
        // hand did, and this is what tells them apart next time: sweep_early must be 0
        // (a wake before its boundary — the freeze-and-snap signature), and the jitter
        // pair separates an even slow beat from an uneven fast one, which fps cannot.
        clockview::Cadence cad; clockview::cadence(cad);
        const int n = snprintf(b, sizeof(b),
                 // slug is the permanent folder id, theme is the display name. Reporting
                 // only the slug is what made "Modern" and "the-office" look unrelated.
                 // `assets` is the fingerprint of the theme data this device is actually
                 // running. Launch Kit compares it with the staged theme to decide whether
                 // anything needs sending at all, which is what turns a no-op push into a
                 // few seconds instead of shipping twenty unchanged files.
                 "{\"fw\":\"%s\",\"slug\":\"%s\",\"theme\":\"%s\",\"weld\":%lu,\"assets\":%lu,\"uptime_s\":%lu,"
                 "\"psram_free_kb\":%u,\"psram_largest_kb\":%u,"
                 "\"heap_free_kb\":%u,\"heap_largest_kb\":%u,"
                 "\"fps\":%u,\"lvgl_ms_per_s\":%u,\"flush_ms_per_s\":%u,"
                 "\"te\":%d,\"stage\":%d,\"te_ms_per_s\":%u,\"pushed_per_s\":%u,"
                 "\"loop_per_s\":%u,\"loop_delay_ms\":%u,"
                 "\"screens_per_s\":%u,"
                 // The allocation CENSUS, not just the free total. Added 2026-10-06 after
                 // /health showed internal free falling 59 KB -> 11 KB over a 30-minute
                 // uptime and the device then rebooting: a leak, which "free" alone can
                 // report but cannot attribute. alloc_blocks climbing is the signature;
                 // lv_int/lv_allocs say whether LVGL is the one doing it, which is the
                 // single most useful split because it separates the UI from the network
                 // stack. The same numbers print to serial as [lvmem]/[memdbg], but a leak
                 // takes half an hour to show and a cable cannot be left attached that
                 // long without ESP_RST_USB rebooting the thing being measured.
                 // Whether the animated background CAN play, which until now was only
                 // answerable with a cable and a lucky boot. bg_frames is what the theme
                 // declares, bg_frames_baked is how many of them are actually in themeart.
                 // Unequal means the partition filled before the frames were reached and
                 // the dial is stuck on frame 0 — see custom_plate_frame().
                 // The theme's own answers, because three separate diagnoses in one session
                 // went wrong through assuming them. Whether the dial sweeps decides which
                 // compose path runs at all, and loop/fps/everySec decide how often a
                 // background frame is asked for. All four are in clock_style.json, which
                 // nothing on the device could report until now.
                 "\"sweep\":%s,\"railway\":%s,\"beat_per_s\":%d,\"bph\":%d,"
                 // NAMED SO THEY CANNOT BE MISREAD, which the first version was.
                 //
                 // bgAnim.frames counts the EXTRA, numbered files; the rotation is
                 // `% (frames + 1)` and index 0 is the unnumbered clock_plate.png. So a
                 // theme shipping twelve pictures declares eleven, and reporting the raw
                 // field as "bg_frames" reads as one of them having gone missing. Report
                 // what someone actually wants to know: how many pictures are in the loop
                 // and how long the loop lasts.
                 "\"bg_extra_frames\":%d,\"bg_extra_baked\":%d,"
                 "\"bg_cycle_frames\":%d,\"bg_cycle_s\":%.2f,"
                 "\"bg_fps\":%d,\"bg_loop\":%s,\"bg_every_s\":%d,"
                 "\"alloc_blocks\":%u,\"free_blocks\":%u,"
                 "\"lv_int\":%u,\"lv_allocs\":%u,\"lv_peak_int\":%u,"
                 "\"compose_ms\":%d,\"plate_ms\":%d,\"text_ms\":%d,\"hands_ms\":%d,\"rest_ms\":%d,\""
                 "sweep_wakes\":%u,\"sweep_jitter_ms\":%u,\"sweep_jitter_max_ms\":%u,"
                 "\"sweep_early\":%u,\"sweep_late\":%u,\"sweep_dup\":%u,\"sweep_skip\":%u,"
                 "\"sweep_beat\":%d,\"sweep_frame_ms\":%d,"
                 "\"wifi_rssi\":%d,\"boot_reason\":\"%s\"}",
                 FW_VERSION, theme_select::activeSlug(), theme_style::themeLabel(),
                 (unsigned long)CUSTOM_WELD_HASH,
                 (unsigned long)theme_style::assetsFingerprint(),
                 (unsigned long)(millis() / 1000UL),
                 (unsigned)(ESP.getFreePsram() / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(ESP.getFreeHeap() / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                 host_fps(), host_lvgl_load(), host_flush_load(),
                 display_te() ? 1 : 0, display_stage() ? 1 : 0, host_te_load(), host_pushed_load(),
                 host_loop_per_s(), (unsigned)g_loopDelayMs,
                 host_screens_per_s(),
                 theme_style::clock().secondSweep   ? "true" : "false",
                 theme_style::clock().secondRailway ? "true" : "false",
                 clockview::sweepBeat(), clockview::sweepBeat() * 3600,
                 theme_style::clock().bgAnim.frames, host_bg_frames_baked(),
                 theme_style::clock().bgAnim.frames + 1,
                 (double)(theme_style::clock().bgAnim.frames + 1) /
                     (double)(theme_style::clock().bgAnim.fps < 1 ? 1 : theme_style::clock().bgAnim.fps),
                 theme_style::clock().bgAnim.fps,
                 theme_style::clock().bgAnim.loop ? "true" : "false",
                 theme_style::clock().bgAnim.everySec,
                 host_internal_alloc_blocks(), host_internal_free_blocks(),
                 (unsigned)orb_lv_live_int_bytes, (unsigned)orb_lv_live_int_count,
                 (unsigned)orb_lv_peak_int_bytes,
                 (int)(cFace + 0.5f), (int)(cPlate + 0.5f), (int)(cText + 0.5f),
                 (int)(cHands + 0.5f), (int)(cFace - cPlate - cText - cHands + 0.5f),
                 cad.wakes, cad.jitAvgMs, cad.jitMaxMs,
                 cad.early, cad.late, cad.dup, cad.skip,
                 cad.beat, (int)(cad.frameMs + 0.5f),
                 (int)WiFi.RSSI(),
                 // Was three cases and "other", which reported a USB-triggered reset
                 // and a panic as the same word. diag_log.cpp owns the full mapping; use
                 // it rather than keeping a second, shorter copy here that can disagree.
                 diag::resetReasonText());
        // snprintf reports what it WOULD have written, so this is the only way to notice.
        if (n < 0 || (size_t)n >= sizeof(b))
            Serial.printf("[health] JSON truncated: needed %d bytes, buffer is %u\n",
                          n, (unsigned)sizeof(b));
        g_web.send(200, "application/json", b);
    });
    g_web.on("/sdput", HTTP_POST, handleSdPutDone, handleSdPutUpload);   // Launch Kit pushes theme files here
    // "I am done sending." Without it the install overlay has no way to tell the end of a
    // transfer from the death of one — see update_ui::installed().
    g_web.on("/installed", HTTP_POST, []{
        update_ui::installed(g_sdUpCount);
        g_sdUpCount = 0;
        g_web.send(200, "text/plain", "ok");
    });
    // The page that drives /sdput from a browser, so a theme can arrive over WiFi from any
    // device on the network rather than only down a USB cable from a Chromium desktop.
    g_web.on("/install", []{ g_web.send_P(200, "text/html", INSTALL_PAGE); });
    // What is on the card, for the page above. The same answer the serial link gives, which
    // matters now that a cable is optional: Studio is served over HTTPS and can never call
    // this, so the device's own page is the only place the card's contents can be seen.
    g_web.on("/themes.json", []{
        static char slugs[theme_select::MAX_THEMES][theme_select::MAX_SLUG_LEN];
        const int n = theme_select::listInstalled(slugs);
        String out = "{\"active\":\"";
        out += theme_select::activeSlug();
        out += "\",\"themes\":[";
        for (int i = 0; i < n; ++i) {
            char label[64];
            theme_style::labelFor(slugs[i], label, sizeof(label));
            // Quotes are the only character that can appear in a theme name and break this;
            // a name is 40 characters of the user's own typing, not arbitrary bytes.
            String name(label);
            name.replace("\"", "'");
            if (i) out += ',';
            out += "{\"slug\":\""; out += slugs[i];
            out += "\",\"name\":\""; out += name; out += "\"}";
        }
        out += "]}";
        g_web.send(200, "application/json", out);
    });
    g_web.on("/themedel", HTTP_POST, []{
        const String slug = g_web.arg("slug");
        if (!slug.length()) { g_web.send(400, "text/plain", "missing slug"); return; }
        // removeInstalled refuses the theme being worn: every screen is drawing from that
        // folder right now. Switch first, then delete.
        if (!theme_select::removeInstalled(slug.c_str())) {
            g_web.send(409, "text/plain", "cannot delete the theme the Orb is wearing");
            return;
        }
        Serial.printf("[theme] deleted '%s' by request\n", slug.c_str());
        g_web.send(200, "text/plain", "ok");
    });
    g_web.on("/", handleRoot);
    g_web.on("/legacy", handleLegacyConfig);   // the old configuration form, unadvertised
    g_web.on("/save", HTTP_POST, handleSave);
    g_web.on("/wifi", HTTP_POST, handleWifi);
    g_web.on("/bright", handleBright);
    g_web.on("/vol", handleVol);
    g_web.on("/alerts", handleAlerts);
    g_web.on("/idle", handleIdle);
    g_web.on("/sweep", handleSweep);
    g_web.on("/airports", handleAirports);
    g_web.on("/ground", handleGround);
    g_web.on("/altmin", handleAltMin);
    g_web.on("/milonly", handleMilOnly);
    g_web.on("/trail", handleTrail);
    g_web.on("/maxac", handleMaxAc);
    g_web.on("/bigtext", handleBigText);
    g_web.on("/rotate", handleRotate);
    g_web.on("/units", handleUnits);
#if ORB_OTA_ENABLED
    g_web.on("/update", HTTP_GET, handleUpdatePage);
    g_web.on("/update", HTTP_POST,
        []() {
            const bool ok = !Update.hasError();
            g_web.send(200, "text/plain", ok ? "OK" : "FAIL");
            delay(800);
            if (ok) ESP.restart();
        },
        handleUpdateUpload);
#endif
    g_web.begin();
}

void web_server_handle() { g_web.handleClient(); }
