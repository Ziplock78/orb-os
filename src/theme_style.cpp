#include "theme_style.h"
#include "theme_sd.h"
#include "theme_select.h"
#include <ArduinoJson.h>
#include <string.h>
#include <stdio.h>
#if !defined(ESP_PLATFORM)
// Desktop simulator: no Serial. This file logs exactly one line (a hand-written theme with
// a second inverted zone), and that line is worth keeping in the sim too, so shim it the
// same way radar_view.cpp and location_view.cpp already do.
static struct { void println(const char *s) const { puts(s); } } Serial;
#endif

// Compile-time fallback defaults (and, for a stock/no-design build, the only values
// that ever apply) — same headers each screen's own view already includes.
#include "custom_clock.h"     // CUSTOM_CLOCK.bg
#include "custom_text.h"      // CUSTOM_HAS_TEXT{1,2} / CUSTOM_TEXT{1,2}_*
#include "custom_radar.h"     // CUSTOM_SWEEP_* / CUSTOM_BLIP_* / CUSTOM_SEL_* / CUSTOM_OFFRANGE_* / CUSTOM_CENTER_* / CUSTOM_HAS_RTEXT{1..4} / CUSTOM_RTEXT{n}_*
#include "custom_settings.h"  // CUSTOM_SETTINGS_*
#include "custom_hands.h"    // CUSTOM_HAS_{HOUR,MINUTE,SECOND,STATIC1,STATIC2} / CUSTOM_*_PIVOT_* / CUSTOM_HAND_ORDER
#include "custom_apps.h"     // CUSTOM_APP_* — compiled fallback for the per-theme app roster
#include "custom_menu.h"      // CUSTOM_HAS_MENU_{CURRENT,PREV,NEXT} / CUSTOM_MENU_{...}_*

namespace theme_style {

namespace {

Clock    s_clock;
Radar    s_radar;
Menu     s_menu;
Settings s_settings;
Apps     s_apps;
Names    s_names;
// The theme's declared asset list (theme.json "assets"). s_assetN == 0 means the theme
// did not declare one, which hasAsset() treats as "allow everything".
constexpr size_t MAX_ASSETS = 24;
char   s_asset[MAX_ASSETS][28] = {};
size_t s_assetN = 0;
uint32_t s_assetsHash = 0;   // theme.json "assetsHash": covers contents, not just names

constexpr size_t MAX_STYLE_JSON_BYTES = 8192;

// Seed every runtime field from the currently-compiled CUSTOM_* macros — exactly
// what each screen drew before this module existed. A JSON style file (if present
// for the active theme) overrides these below, field by field; anything the file
// doesn't set, or no file at all, keeps this compiled default.
void seed_defaults() {
    s_clock = Clock{};
    s_clock.bg = (uint32_t)CUSTOM_CLOCK.bg;
    s_clock.plateFollow = 0;      // static plate unless the theme says otherwise
#if CUSTOM_HAS_TEXT1
    s_clock.text1.show = true;
    s_clock.text1.x = CUSTOM_TEXT1_X;
    s_clock.text1.y = CUSTOM_TEXT1_Y;
    s_clock.text1.color = (uint32_t)CUSTOM_TEXT1_COLOR;
    s_clock.text1.glow = CUSTOM_TEXT1_GLOW;
    s_clock.text1.glowColor = (uint32_t)CUSTOM_TEXT1_GLOWCOLOR;
    snprintf(s_clock.text1.fmt, sizeof(s_clock.text1.fmt), "%s", CUSTOM_TEXT1_FMT);
    s_clock.text1.curved = (bool)CUSTOM_TEXT1_CURVED;
    s_clock.text1.curveR = CUSTOM_TEXT1_CURVE_R;
    s_clock.text1.arcDeg = CUSTOM_TEXT1_ARCDEG;
    s_clock.text1.align = CUSTOM_TEXT1_ALIGN;
#endif
#if CUSTOM_HAS_TEXT2
    s_clock.text2.show = true;
    s_clock.text2.x = CUSTOM_TEXT2_X;
    s_clock.text2.y = CUSTOM_TEXT2_Y;
    s_clock.text2.color = (uint32_t)CUSTOM_TEXT2_COLOR;
    s_clock.text2.glow = CUSTOM_TEXT2_GLOW;
    s_clock.text2.glowColor = (uint32_t)CUSTOM_TEXT2_GLOWCOLOR;
    snprintf(s_clock.text2.fmt, sizeof(s_clock.text2.fmt), "%s", CUSTOM_TEXT2_FMT);
    s_clock.text2.curved = (bool)CUSTOM_TEXT2_CURVED;
    s_clock.text2.curveR = CUSTOM_TEXT2_CURVE_R;
    s_clock.text2.arcDeg = CUSTOM_TEXT2_ARCDEG;
    s_clock.text2.align = CUSTOM_TEXT2_ALIGN;
#endif

    // Clock hands (0=hour 1=minute 2=second 3=static1 4=static2). Art, geometry, and the
    // per-hand show gate were all compile-time until now, which is why switching the SD
    // theme swapped the clock plate but left the previous theme's hands sitting on it.
    {
        static const int px[5] = { CUSTOM_HOUR_PIVOT_X, CUSTOM_MINUTE_PIVOT_X, CUSTOM_SECOND_PIVOT_X, CUSTOM_STATIC1_PIVOT_X, CUSTOM_STATIC2_PIVOT_X };
        static const int py[5] = { CUSTOM_HOUR_PIVOT_Y, CUSTOM_MINUTE_PIVOT_Y, CUSTOM_SECOND_PIVOT_Y, CUSTOM_STATIC1_PIVOT_Y, CUSTOM_STATIC2_PIVOT_Y };
        static const int cx[5] = { CUSTOM_HOUR_CENTER_X, CUSTOM_MINUTE_CENTER_X, CUSTOM_SECOND_CENTER_X, CUSTOM_STATIC1_CENTER_X, CUSTOM_STATIC2_CENTER_X };
        static const int cy[5] = { CUSTOM_HOUR_CENTER_Y, CUSTOM_MINUTE_CENTER_Y, CUSTOM_SECOND_CENTER_Y, CUSTOM_STATIC1_CENTER_Y, CUSTOM_STATIC2_CENTER_Y };
        static const int bl[5] = { CUSTOM_HOUR_BLEND, CUSTOM_MINUTE_BLEND, CUSTOM_SECOND_BLEND, CUSTOM_STATIC1_BLEND, CUSTOM_STATIC2_BLEND };
        static const bool has[5] = { (bool)CUSTOM_HAS_HOUR, (bool)CUSTOM_HAS_MINUTE, (bool)CUSTOM_HAS_SECOND, (bool)CUSTOM_HAS_STATIC1, (bool)CUSTOM_HAS_STATIC2 };
        for (int i = 0; i < 5; ++i) {
            s_clock.hand[i].show    = has[i];
            s_clock.hand[i].pivotX  = px[i];
            s_clock.hand[i].pivotY  = py[i];
            s_clock.hand[i].centerX = cx[i];
            s_clock.hand[i].centerY = cy[i];
            s_clock.hand[i].blend   = bl[i];
        }
        static const int ord[] = CUSTOM_HAND_ORDER;
        s_clock.orderN = (CUSTOM_HAND_ORDER_N > 5) ? 5 : CUSTOM_HAND_ORDER_N;
        for (int i = 0; i < s_clock.orderN; ++i) s_clock.order[i] = ord[i];
    }

    // App roster. Settings is not here on purpose: it is a system screen, always present.
    s_apps = Apps{};
    s_names = Names{};      // stock labels; theme.json may relabel any of them
    s_assetsHash = 0;
    s_apps.clock        = (bool)CUSTOM_APP_CLOCK;
    s_apps.flight       = (bool)CUSTOM_APP_FLIGHT;
    s_apps.weather      = (bool)CUSTOM_APP_WEATHER;
    s_apps.intel        = (bool)CUSTOM_APP_INTEL;
    s_apps.surveillance = (bool)CUSTOM_APP_SURVEILLANCE;

    s_radar = Radar{};
    s_radar.sweepEnabled = (bool)CUSTOM_SWEEP_ENABLED;
    s_radar.sweepColor = (uint32_t)CUSTOM_SWEEP_COLOR;
    s_radar.sweepLeadColor = (uint32_t)CUSTOM_SWEEP_LEADCOLOR;
    s_radar.sweepTrailDeg = CUSTOM_SWEEP_TRAILDEG;
    s_radar.sweepOpacity = CUSTOM_SWEEP_OPACITY;
    s_radar.sweepLength = CUSTOM_SWEEP_LENGTH;
    s_radar.sweepSpeed = CUSTOM_SWEEP_SPEED;
    s_radar.blipTypeImage = (bool)CUSTOM_BLIP_TYPE_IMAGE;
    s_radar.blipKiteShape = (bool)CUSTOM_BLIP_KITE_SHAPE;
    s_radar.blipSize = CUSTOM_BLIP_SIZE;
    s_radar.blipKiteT = CUSTOM_BLIP_KITE_T;
    s_radar.blipFixedColorMode = (bool)CUSTOM_BLIP_FIXED_COLOR_MODE;
    s_radar.blipFixedColor = (uint32_t)CUSTOM_BLIP_FIXED_COLOR;
    s_radar.blipAltGround = (uint32_t)CUSTOM_BLIP_ALT_GROUND;
    s_radar.blipAltLow = (uint32_t)CUSTOM_BLIP_ALT_LOW;
    s_radar.blipAltMid = (uint32_t)CUSTOM_BLIP_ALT_MID;
    s_radar.blipAltHigh = (uint32_t)CUSTOM_BLIP_ALT_HIGH;
    s_radar.blipAltCruise = (uint32_t)CUSTOM_BLIP_ALT_CRUISE;
    s_radar.blipAltJet = (uint32_t)CUSTOM_BLIP_ALT_JET;
    s_radar.blipGlow = CUSTOM_BLIP_GLOW;
    s_radar.blipGlowColor = (uint32_t)CUSTOM_BLIP_GLOWCOLOR;
    s_radar.blipImageTint = (bool)CUSTOM_BLIP_IMAGE_TINT;
    s_radar.selEnabled = (bool)CUSTOM_SEL_ENABLED;
    s_radar.selColor = (uint32_t)CUSTOM_SEL_COLOR;
    s_radar.selWidth = CUSTOM_SEL_WIDTH;
    s_radar.selDiameter = CUSTOM_SEL_DIAMETER;
    s_radar.selGlow = CUSTOM_SEL_GLOW;
    s_radar.selGlowColor = (uint32_t)CUSTOM_SEL_GLOWCOLOR;
    s_radar.offRangeEnabled = (bool)CUSTOM_OFFRANGE_ENABLED;
    s_radar.offRangeColor = (uint32_t)CUSTOM_OFFRANGE_COLOR;
    s_radar.offRangeSize = CUSTOM_OFFRANGE_SIZE;
    s_radar.centerRadius = CUSTOM_CENTER_RADIUS;
    s_radar.centerColor = (uint32_t)CUSTOM_CENTER_COLOR;
    s_radar.centerInnerRadius = CUSTOM_CENTER_INNER_RADIUS;
    s_radar.centerInnerColor = (uint32_t)CUSTOM_CENTER_INNER_COLOR;
#if CUSTOM_HAS_RTEXT1
    s_radar.rtext[0].show = true;
    s_radar.rtext[0].x = CUSTOM_RTEXT1_X;
    s_radar.rtext[0].y = CUSTOM_RTEXT1_Y;
    s_radar.rtext[0].color = (uint32_t)CUSTOM_RTEXT1_COLOR;
    s_radar.rtext[0].glow = CUSTOM_RTEXT1_GLOW;
    s_radar.rtext[0].glowColor = (uint32_t)CUSTOM_RTEXT1_GLOWCOLOR;
    snprintf(s_radar.rtext[0].fmt, sizeof(s_radar.rtext[0].fmt), "%s", CUSTOM_RTEXT1_FMT);
    s_radar.rtext[0].align = CUSTOM_RTEXT1_ALIGN;
    s_radar.rtext[0].curved = (bool)CUSTOM_RTEXT1_CURVED;
    s_radar.rtext[0].curveR = CUSTOM_RTEXT1_CURVE_R;
    s_radar.rtext[0].arcDeg = CUSTOM_RTEXT1_ARCDEG;
#endif
#if CUSTOM_HAS_RTEXT2
    s_radar.rtext[1].show = true;
    s_radar.rtext[1].x = CUSTOM_RTEXT2_X;
    s_radar.rtext[1].y = CUSTOM_RTEXT2_Y;
    s_radar.rtext[1].color = (uint32_t)CUSTOM_RTEXT2_COLOR;
    s_radar.rtext[1].glow = CUSTOM_RTEXT2_GLOW;
    s_radar.rtext[1].glowColor = (uint32_t)CUSTOM_RTEXT2_GLOWCOLOR;
    snprintf(s_radar.rtext[1].fmt, sizeof(s_radar.rtext[1].fmt), "%s", CUSTOM_RTEXT2_FMT);
    s_radar.rtext[1].align = CUSTOM_RTEXT2_ALIGN;
    s_radar.rtext[1].curved = (bool)CUSTOM_RTEXT2_CURVED;
    s_radar.rtext[1].curveR = CUSTOM_RTEXT2_CURVE_R;
    s_radar.rtext[1].arcDeg = CUSTOM_RTEXT2_ARCDEG;
#endif
#if CUSTOM_HAS_RTEXT3
    s_radar.rtext[2].show = true;
    s_radar.rtext[2].x = CUSTOM_RTEXT3_X;
    s_radar.rtext[2].y = CUSTOM_RTEXT3_Y;
    s_radar.rtext[2].color = (uint32_t)CUSTOM_RTEXT3_COLOR;
    s_radar.rtext[2].glow = CUSTOM_RTEXT3_GLOW;
    s_radar.rtext[2].glowColor = (uint32_t)CUSTOM_RTEXT3_GLOWCOLOR;
    snprintf(s_radar.rtext[2].fmt, sizeof(s_radar.rtext[2].fmt), "%s", CUSTOM_RTEXT3_FMT);
    s_radar.rtext[2].align = CUSTOM_RTEXT3_ALIGN;
    s_radar.rtext[2].curved = (bool)CUSTOM_RTEXT3_CURVED;
    s_radar.rtext[2].curveR = CUSTOM_RTEXT3_CURVE_R;
    s_radar.rtext[2].arcDeg = CUSTOM_RTEXT3_ARCDEG;
#endif
#if CUSTOM_HAS_RTEXT4
    s_radar.rtext[3].show = true;
    s_radar.rtext[3].x = CUSTOM_RTEXT4_X;
    s_radar.rtext[3].y = CUSTOM_RTEXT4_Y;
    s_radar.rtext[3].color = (uint32_t)CUSTOM_RTEXT4_COLOR;
    s_radar.rtext[3].glow = CUSTOM_RTEXT4_GLOW;
    s_radar.rtext[3].glowColor = (uint32_t)CUSTOM_RTEXT4_GLOWCOLOR;
    snprintf(s_radar.rtext[3].fmt, sizeof(s_radar.rtext[3].fmt), "%s", CUSTOM_RTEXT4_FMT);
    s_radar.rtext[3].align = CUSTOM_RTEXT4_ALIGN;
    s_radar.rtext[3].curved = (bool)CUSTOM_RTEXT4_CURVED;
    s_radar.rtext[3].curveR = CUSTOM_RTEXT4_CURVE_R;
    s_radar.rtext[3].arcDeg = CUSTOM_RTEXT4_ARCDEG;
#endif

    s_settings = Settings{};
    s_settings.wheelR = CUSTOM_SETTINGS_WHEEL_R;
    s_settings.wheelRx = CUSTOM_SETTINGS_WHEEL_RX;
    s_settings.wheelStepDeg = CUSTOM_SETTINGS_WHEEL_STEPDEG;
    s_settings.wheelCy = CUSTOM_SETTINGS_WHEEL_CY;
    s_settings.wheelFade = CUSTOM_SETTINGS_WHEEL_FADE;
    s_settings.selColor = (uint32_t)CUSTOM_SETTINGS_SEL_COLOR;
    s_settings.itemColor = (uint32_t)CUSTOM_SETTINGS_ITEM_COLOR;
    s_settings.glow = CUSTOM_SETTINGS_GLOW;
    s_settings.glowColor = (uint32_t)CUSTOM_SETTINGS_GLOWCOLOR;
    s_settings.hlShow = (bool)CUSTOM_SETTINGS_HL_SHOW;
    s_settings.hlColor = (uint32_t)CUSTOM_SETTINGS_HL_COLOR;
    s_settings.hlOpacity = CUSTOM_SETTINGS_HL_OPACITY;
    s_settings.hlW = CUSTOM_SETTINGS_HL_W;
    s_settings.hlH = CUSTOM_SETTINGS_HL_H;
    s_settings.hlRadius = CUSTOM_SETTINGS_HL_RADIUS;
    s_settings.defaultSel = CUSTOM_SETTINGS_DEFAULT_SEL;

    s_menu = Menu{};
#if CUSTOM_HAS_MENU_CURRENT
    s_menu.current.show = true;
    s_menu.current.x = CUSTOM_MENU_CURRENT_X;
    s_menu.current.y = CUSTOM_MENU_CURRENT_Y;
    s_menu.current.color = (uint32_t)CUSTOM_MENU_CURRENT_COLOR;
    s_menu.current.glow = CUSTOM_MENU_CURRENT_GLOW;
    s_menu.current.glowColor = (uint32_t)CUSTOM_MENU_CURRENT_GLOWCOLOR;
    snprintf(s_menu.current.fmt, sizeof(s_menu.current.fmt), "%s", CUSTOM_MENU_CURRENT_FMT);
    s_menu.current.align = CUSTOM_MENU_CURRENT_ALIGN;
#endif
#if CUSTOM_HAS_MENU_PREV
    s_menu.prev.show = true;
    s_menu.prev.x = CUSTOM_MENU_PREV_X;
    s_menu.prev.y = CUSTOM_MENU_PREV_Y;
    s_menu.prev.color = (uint32_t)CUSTOM_MENU_PREV_COLOR;
    s_menu.prev.glow = CUSTOM_MENU_PREV_GLOW;
    s_menu.prev.glowColor = (uint32_t)CUSTOM_MENU_PREV_GLOWCOLOR;
    snprintf(s_menu.prev.fmt, sizeof(s_menu.prev.fmt), "%s", CUSTOM_MENU_PREV_FMT);
    s_menu.prev.align = CUSTOM_MENU_PREV_ALIGN;
#endif
#if CUSTOM_HAS_MENU_NEXT
    s_menu.next.show = true;
    s_menu.next.x = CUSTOM_MENU_NEXT_X;
    s_menu.next.y = CUSTOM_MENU_NEXT_Y;
    s_menu.next.color = (uint32_t)CUSTOM_MENU_NEXT_COLOR;
    s_menu.next.glow = CUSTOM_MENU_NEXT_GLOW;
    s_menu.next.glowColor = (uint32_t)CUSTOM_MENU_NEXT_GLOWCOLOR;
    snprintf(s_menu.next.fmt, sizeof(s_menu.next.fmt), "%s", CUSTOM_MENU_NEXT_FMT);
    s_menu.next.align = CUSTOM_MENU_NEXT_ALIGN;
#endif
}

// Merge helpers: only touch a field when the JSON actually has it, so a partial
// file (or a field a given theme never set) leaves the compiled default in place.
void merge_text(JsonVariantConst j, ClockText &t) {
    if (j.isNull()) return;
    if (j["show"].is<bool>()) t.show = j["show"].as<bool>();
    if (j["x"].is<int>()) t.x = j["x"].as<int>();
    if (j["y"].is<int>()) t.y = j["y"].as<int>();
    if (j["color"].is<uint32_t>()) t.color = j["color"].as<uint32_t>();
    if (j["glow"].is<int>()) t.glow = j["glow"].as<int>();
    if (j["glowColor"].is<uint32_t>()) t.glowColor = j["glowColor"].as<uint32_t>();
    if (j["fmt"].is<const char *>()) snprintf(t.fmt, sizeof(t.fmt), "%s", j["fmt"].as<const char *>());
    if (j["curved"].is<bool>()) t.curved = j["curved"].as<bool>();
    if (j["curveR"].is<int>()) t.curveR = j["curveR"].as<int>();
    if (j["arcDeg"].is<float>()) t.arcDeg = j["arcDeg"].as<float>();
    if (j["align"].is<int>()) t.align = j["align"].as<int>();
}

void merge_rtext(JsonVariantConst j, RadarText &t) {
    if (j.isNull()) return;
    if (j["show"].is<bool>()) t.show = j["show"].as<bool>();
    if (j["x"].is<int>()) t.x = j["x"].as<int>();
    if (j["y"].is<int>()) t.y = j["y"].as<int>();
    if (j["color"].is<uint32_t>()) t.color = j["color"].as<uint32_t>();
    if (j["glow"].is<int>()) t.glow = j["glow"].as<int>();
    if (j["glowColor"].is<uint32_t>()) t.glowColor = j["glowColor"].as<uint32_t>();
    if (j["fmt"].is<const char *>()) snprintf(t.fmt, sizeof(t.fmt), "%s", j["fmt"].as<const char *>());
    if (j["curved"].is<bool>()) t.curved = j["curved"].as<bool>();
    if (j["curveR"].is<int>()) t.curveR = j["curveR"].as<int>();
    if (j["arcDeg"].is<float>()) t.arcDeg = j["arcDeg"].as<float>();
    if (j["align"].is<int>()) t.align = j["align"].as<int>();
}

void merge_radar_static(JsonVariantConst j, RadarStatic &s) {
    if (j.isNull()) return;
    if (j["show"].is<bool>()) s.show = j["show"].as<bool>();
    if (j["x"].is<int>()) s.x = j["x"].as<int>();
    if (j["y"].is<int>()) s.y = j["y"].as<int>();
    if (j["opacity"].is<int>()) s.opacity = j["opacity"].as<int>();
    if (j["scale"].is<float>()) s.scale = j["scale"].as<float>();
}

void merge_hand(JsonVariantConst j, Hand &h) {
    if (j.isNull()) return;
    if (j["show"].is<bool>())   h.show    = j["show"].as<bool>();
    if (j["pivotX"].is<int>())  h.pivotX  = j["pivotX"].as<int>();
    if (j["pivotY"].is<int>())  h.pivotY  = j["pivotY"].as<int>();
    if (j["centerX"].is<int>()) h.centerX = j["centerX"].as<int>();
    if (j["centerY"].is<int>()) h.centerY = j["centerY"].as<int>();
    if (j["blend"].is<int>())   h.blend   = j["blend"].as<int>();
}

void merge_menu_text(JsonVariantConst j, MenuText &t) {
    if (j.isNull()) return;
    if (j["show"].is<bool>()) t.show = j["show"].as<bool>();
    if (j["x"].is<int>()) t.x = j["x"].as<int>();
    if (j["y"].is<int>()) t.y = j["y"].as<int>();
    if (j["color"].is<uint32_t>()) t.color = j["color"].as<uint32_t>();
    if (j["glow"].is<int>()) t.glow = j["glow"].as<int>();
    if (j["glowColor"].is<uint32_t>()) t.glowColor = j["glowColor"].as<uint32_t>();
    if (j["fmt"].is<const char *>()) snprintf(t.fmt, sizeof(t.fmt), "%s", j["fmt"].as<const char *>());
    if (j["align"].is<int>()) t.align = j["align"].as<int>();
    if (j["wrapWidth"].is<int>()) t.wrapWidth = j["wrapWidth"].as<int>();
    if (j["lineGap"].is<int>()) t.lineGap = j["lineGap"].as<int>();
    if (j["lineStep"].is<int>()) t.lineStep = j["lineStep"].as<int>();
}

// Reads /themes/<slug>/<name> into `doc`. Returns false (doc left empty) if the
// theme has no such file yet — expected for any theme exported before this
// module existed, or a screen that's never been (re)pushed since.
bool read_style_json(const char *slug, const char *name, JsonDocument &doc) {
    char path[96];
    snprintf(path, sizeof(path), "/themes/%s/%s", slug, name);
    size_t len = 0;
    uint8_t *buf = theme_sd::read_whole(path, len, MAX_STYLE_JSON_BYTES);
    if (!buf) return false;
    const DeserializationError err = deserializeJson(doc, buf, len);
    theme_sd::free(buf);
    return !err;
}

} // namespace

void load() {
    seed_defaults();

    const char *slug = theme_select::activeSlug();
    if (!slug || !slug[0]) return;   // no theme active (stock build) — compiled defaults stand

    {
        JsonDocument doc;
        if (read_style_json(slug, "clock_style.json", doc)) {
            if (doc["bg"].is<uint32_t>()) s_clock.bg = doc["bg"].as<uint32_t>();
            // "plateFollow": which hand the background plate rotates with — 0 none,
            // 1 hour, 2 minute, 3 second. The exported plate PNG carries only its static
            // rotation, so this angle is applied live on top (see clock_view draw_custom).
            if (doc["plateFollow"].is<int>()) s_clock.plateFollow = doc["plateFollow"].as<int>();
            merge_text(doc["text1"], s_clock.text1);
            merge_text(doc["text2"], s_clock.text2);
            // "hands": { "hour": {...}, "minute": {...}, "second": {...},
            //            "static1": {...}, "static2": {...}, "order": [3,4,0,1,2] }
            JsonVariantConst hands = doc["hands"];
            if (!hands.isNull()) {
                static const char *names[5] = { "hour", "minute", "second", "static1", "static2" };
                for (int i = 0; i < 5; ++i) merge_hand(hands[names[i]], s_clock.hand[i]);
                JsonArrayConst ord = hands["order"].as<JsonArrayConst>();
                if (!ord.isNull() && ord.size() > 0) {
                    int n = 0;
                    for (JsonVariantConst v : ord) {
                        if (n >= 5) break;
                        const int k = v.as<int>();
                        if (k >= 0 && k < 5) s_clock.order[n++] = k;   // ignore junk indices
                    }
                    if (n > 0) s_clock.orderN = n;
                }
                // One light for the whole dial. Absent means off, which is what every
                // theme made before this existed intends.
                JsonVariantConst sh = hands["shadow"];
                if (!sh.isNull()) {
                    s_clock.shadowOn = sh["on"] | false;
                    s_clock.shadowDX = sh["dx"] | 0;
                    s_clock.shadowDY = sh["dy"] | 0;
                }
            }
        }
    }
    {
        JsonDocument doc;
        if (read_style_json(slug, "radar_style.json", doc)) {
            if (doc["sweepEnabled"].is<bool>()) s_radar.sweepEnabled = doc["sweepEnabled"].as<bool>();
            if (doc["sweepTypeImage"].is<bool>()) s_radar.sweepTypeImage = doc["sweepTypeImage"].as<bool>();
            if (doc["sweepColor"].is<uint32_t>()) s_radar.sweepColor = doc["sweepColor"].as<uint32_t>();
            if (doc["sweepLeadColor"].is<uint32_t>()) s_radar.sweepLeadColor = doc["sweepLeadColor"].as<uint32_t>();
            if (doc["sweepTrailDeg"].is<int>()) s_radar.sweepTrailDeg = doc["sweepTrailDeg"].as<int>();
            if (doc["sweepOpacity"].is<int>()) s_radar.sweepOpacity = doc["sweepOpacity"].as<int>();
            if (doc["sweepLength"].is<int>()) s_radar.sweepLength = doc["sweepLength"].as<int>();
            if (doc["sweepSpeed"].is<int>()) s_radar.sweepSpeed = doc["sweepSpeed"].as<int>();
            if (doc["blipEnabled"].is<bool>()) s_radar.blipEnabled = doc["blipEnabled"].as<bool>();
            if (doc["blipTypeImage"].is<bool>()) s_radar.blipTypeImage = doc["blipTypeImage"].as<bool>();
            if (doc["blipRotate"].is<bool>()) s_radar.blipRotate = doc["blipRotate"].as<bool>();
            if (doc["blipKiteShape"].is<bool>()) s_radar.blipKiteShape = doc["blipKiteShape"].as<bool>();
            if (doc["blipSize"].is<int>()) s_radar.blipSize = doc["blipSize"].as<int>();
            if (doc["blipKiteT"].is<int>()) s_radar.blipKiteT = doc["blipKiteT"].as<int>();
            if (doc["blipFixedColorMode"].is<bool>()) s_radar.blipFixedColorMode = doc["blipFixedColorMode"].as<bool>();
            if (doc["blipFixedColor"].is<uint32_t>()) s_radar.blipFixedColor = doc["blipFixedColor"].as<uint32_t>();
            if (doc["blipAltGround"].is<uint32_t>()) s_radar.blipAltGround = doc["blipAltGround"].as<uint32_t>();
            if (doc["blipAltLow"].is<uint32_t>()) s_radar.blipAltLow = doc["blipAltLow"].as<uint32_t>();
            if (doc["blipAltMid"].is<uint32_t>()) s_radar.blipAltMid = doc["blipAltMid"].as<uint32_t>();
            if (doc["blipAltHigh"].is<uint32_t>()) s_radar.blipAltHigh = doc["blipAltHigh"].as<uint32_t>();
            if (doc["blipAltCruise"].is<uint32_t>()) s_radar.blipAltCruise = doc["blipAltCruise"].as<uint32_t>();
            if (doc["blipAltJet"].is<uint32_t>()) s_radar.blipAltJet = doc["blipAltJet"].as<uint32_t>();
            if (doc["blipGlow"].is<int>()) s_radar.blipGlow = doc["blipGlow"].as<int>();
            if (doc["blipGlowColor"].is<uint32_t>()) s_radar.blipGlowColor = doc["blipGlowColor"].as<uint32_t>();
            if (doc["blipImageTint"].is<bool>()) s_radar.blipImageTint = doc["blipImageTint"].as<bool>();
            if (doc["selEnabled"].is<bool>()) s_radar.selEnabled = doc["selEnabled"].as<bool>();
            if (doc["selStyle"].is<int>()) s_radar.selStyle = doc["selStyle"].as<int>();
            if (doc["selColor"].is<uint32_t>()) s_radar.selColor = doc["selColor"].as<uint32_t>();
            if (doc["selWidth"].is<int>()) s_radar.selWidth = doc["selWidth"].as<int>();
            if (doc["selDiameter"].is<int>()) s_radar.selDiameter = doc["selDiameter"].as<int>();
            if (doc["selGlow"].is<int>()) s_radar.selGlow = doc["selGlow"].as<int>();
            if (doc["selGlowColor"].is<uint32_t>()) s_radar.selGlowColor = doc["selGlowColor"].as<uint32_t>();
            if (doc["offRangeEnabled"].is<bool>()) s_radar.offRangeEnabled = doc["offRangeEnabled"].as<bool>();
            if (doc["offRangeColor"].is<uint32_t>()) s_radar.offRangeColor = doc["offRangeColor"].as<uint32_t>();
            if (doc["offRangeSize"].is<int>()) s_radar.offRangeSize = doc["offRangeSize"].as<int>();
            if (doc["centerEnabled"].is<bool>()) s_radar.centerEnabled = doc["centerEnabled"].as<bool>();
            if (doc["centerRadius"].is<int>()) s_radar.centerRadius = doc["centerRadius"].as<int>();
            if (doc["centerColor"].is<uint32_t>()) s_radar.centerColor = doc["centerColor"].as<uint32_t>();
            if (doc["centerInnerRadius"].is<int>()) s_radar.centerInnerRadius = doc["centerInnerRadius"].as<int>();
            if (doc["centerInnerColor"].is<uint32_t>()) s_radar.centerInnerColor = doc["centerInnerColor"].as<uint32_t>();
            JsonVariantConst rtext = doc["rtext"];
            if (rtext.is<JsonArrayConst>()) {
                int i = 0;
                for (JsonVariantConst item : rtext.as<JsonArrayConst>()) {
                    if (i >= 4) break;
                    merge_rtext(item, s_radar.rtext[i]);
                    i++;
                }
            }
            merge_radar_static(doc["static1"], s_radar.static1);
            merge_radar_static(doc["static2"], s_radar.static2);
            if (doc["overlayEnabled"].is<bool>()) s_radar.overlayEnabled = doc["overlayEnabled"].as<bool>();
            if (doc["overlayColor"].is<uint32_t>()) s_radar.overlayColor = doc["overlayColor"].as<uint32_t>();
            if (doc["overlayOpacity"].is<int>()) s_radar.overlayOpacity = doc["overlayOpacity"].as<int>();
            if (doc["maxAircraft"].is<int>())    s_radar.maxAircraft    = doc["maxAircraft"].as<int>();
            if (doc["minAltFt"].is<int>())       s_radar.minAltFt       = doc["minAltFt"].as<int>();
            if (doc["hideGround"].is<bool>())    s_radar.hideGround     = doc["hideGround"].as<bool>() ? 1 : 0;
            if (doc["simulate"].is<bool>())      s_radar.simulate       = doc["simulate"].as<bool>();
            if (doc["sweepPivotX"].is<int>())    s_radar.sweepPivotX    = doc["sweepPivotX"].as<int>();
            if (doc["sweepPivotY"].is<int>())    s_radar.sweepPivotY    = doc["sweepPivotY"].as<int>();
            if (doc["sweepCenterX"].is<int>())   s_radar.sweepCenterX   = doc["sweepCenterX"].as<int>();
            if (doc["sweepCenterY"].is<int>())   s_radar.sweepCenterY   = doc["sweepCenterY"].as<int>();
            if (doc["blipPivotX"].is<int>())     s_radar.blipPivotX     = doc["blipPivotX"].as<int>();
            if (doc["blipPivotY"].is<int>())     s_radar.blipPivotY     = doc["blipPivotY"].as<int>();
            // "order": [3,4,5,0,1,2] — back-to-front, same kind indices as
            // CUSTOM_RADAR_LAYER_ORDER. Junk indices are ignored rather than trusted;
            // an empty or entirely junk list leaves orderN at 0, which means the welded
            // order stands.
            {
                JsonArrayConst ord = doc["order"].as<JsonArrayConst>();
                if (!ord.isNull() && ord.size() > 0) {
                    int n = 0;
                    for (JsonVariantConst v : ord) {
                        if (n >= 6) break;
                        const int k = v.as<int>();
                        if (k >= 0 && k < 6) s_radar.order[n++] = k;
                    }
                    if (n > 0) s_radar.orderN = n;
                }
            }
            s_radar.zoneCount = 0;
            bool s_radarHasInvertZone = false;
            if (doc["zones"].is<JsonArrayConst>()) {
                for (JsonVariantConst z : doc["zones"].as<JsonArrayConst>()) {
                    if (s_radar.zoneCount >= theme_style::Radar::MAX_ZONES) break;
                    const bool rect = z["rect"] | false;
                    const int  r = z["r"] | 0;
                    const int  w = z["w"] | 0;
                    const int  h = z["h"] | 0;
                    // A zone with no extent masks nothing, so it is dropped rather than
                    // shipped as a shape that silently does nothing.
                    if (rect ? (w <= 0 || h <= 0) : (r <= 0)) continue;
                    const bool invert = z["invert"] | false;
                    // At most ONE inverted zone. Inverted means "hide outside me", so two of
                    // them intersect: an aircraft must be inside both to show, and two that
                    // do not overlap hide everything, leaving a scope that looks broken
                    // rather than configured. The editors prevent it; this is the backstop
                    // for a hand-written theme, and it says so rather than failing quietly.
                    if (invert && s_radarHasInvertZone) {
                        Serial.println("[theme] ignoring extra inverted zone: only one is allowed");
                        continue;
                    }
                    if (invert) s_radarHasInvertZone = true;
                    theme_style::Radar::Zone &out = s_radar.zones[s_radar.zoneCount++];
                    out.x      = z["x"] | 233;
                    out.y      = z["y"] | 233;
                    out.r      = r;
                    out.w      = w;
                    out.h      = h;
                    out.rect   = rect;
                    out.invert = invert;
                }
            }
        }
    }
    {
        JsonDocument doc;
        if (read_style_json(slug, "settings_style.json", doc)) {
            if (doc["wheelR"].is<float>()) s_settings.wheelR = doc["wheelR"].as<float>();
            if (doc["wheelRx"].is<float>()) s_settings.wheelRx = doc["wheelRx"].as<float>();
            if (doc["wheelStepDeg"].is<float>()) s_settings.wheelStepDeg = doc["wheelStepDeg"].as<float>();
            if (doc["wheelCy"].is<float>()) s_settings.wheelCy = doc["wheelCy"].as<float>();
            if (doc["wheelFade"].is<float>()) s_settings.wheelFade = doc["wheelFade"].as<float>();
            if (doc["selColor"].is<uint32_t>()) s_settings.selColor = doc["selColor"].as<uint32_t>();
            if (doc["itemColor"].is<uint32_t>()) s_settings.itemColor = doc["itemColor"].as<uint32_t>();
            if (doc["glow"].is<int>()) s_settings.glow = doc["glow"].as<int>();
            if (doc["glowColor"].is<uint32_t>()) s_settings.glowColor = doc["glowColor"].as<uint32_t>();
            if (doc["hlShow"].is<bool>()) s_settings.hlShow = doc["hlShow"].as<bool>();
            if (doc["hlColor"].is<uint32_t>()) s_settings.hlColor = doc["hlColor"].as<uint32_t>();
            if (doc["hlOpacity"].is<int>()) s_settings.hlOpacity = doc["hlOpacity"].as<int>();
            if (doc["hlW"].is<int>()) s_settings.hlW = doc["hlW"].as<int>();
            if (doc["hlH"].is<int>()) s_settings.hlH = doc["hlH"].as<int>();
            if (doc["hlRadius"].is<int>()) s_settings.hlRadius = doc["hlRadius"].as<int>();
            if (doc["defaultSel"].is<int>()) s_settings.defaultSel = doc["defaultSel"].as<int>();
        }
    }
    {
        JsonDocument doc;
        if (read_style_json(slug, "menu_style.json", doc)) {
            merge_menu_text(doc["current"], s_menu.current);
            merge_menu_text(doc["prev"], s_menu.prev);
            merge_menu_text(doc["next"], s_menu.next);
        }
    }
    {
        // Theme-level file, not per screen: /themes/<slug>/theme.json
        //   { "apps": { "clock": true, "flight": true, "weather": false,
        //               "intel": false, "surveillance": false } }
        // A theme with no theme.json keeps the compiled roster, so older themes already
        // on the card behave exactly as before. A "settings" key is ignored on purpose:
        // Settings is a system screen and must never be switchable off.
        JsonDocument doc;
        if (read_style_json(slug, "theme.json", doc)) {
            JsonVariantConst a = doc["apps"];
            if (!a.isNull()) {
                if (a["clock"].is<bool>())        s_apps.clock        = a["clock"].as<bool>();
                if (a["flight"].is<bool>())       s_apps.flight       = a["flight"].as<bool>();
                if (a["weather"].is<bool>())      s_apps.weather      = a["weather"].as<bool>();
                if (a["intel"].is<bool>())        s_apps.intel        = a["intel"].as<bool>();
                if (a["surveillance"].is<bool>()) s_apps.surveillance = a["surveillance"].as<bool>();
            }
            // Display labels. Purely cosmetic: they never affect which folder is read or
            // which app is which, so a theme can rename Flight Tracker freely.
            if (doc["name"].is<const char *>())
                snprintf(s_names.theme, sizeof(s_names.theme), "%s", doc["name"].as<const char *>());
            JsonVariantConst nm = doc["names"];
            if (!nm.isNull()) {
                struct { const char *key; char *dst; size_t cap; } map[] = {
                    { "clock",        s_names.clock,        sizeof(s_names.clock)        },
                    { "flight",       s_names.flight,       sizeof(s_names.flight)       },
                    { "weather",      s_names.weather,      sizeof(s_names.weather)      },
                    { "intel",        s_names.intel,        sizeof(s_names.intel)        },
                    { "surveillance", s_names.surveillance, sizeof(s_names.surveillance) },
                    { "settings",     s_names.settings,     sizeof(s_names.settings)     },
                };
                for (auto &m : map) {
                    JsonVariantConst v = nm[m.key];
                    if (v.is<const char *>() && v.as<const char *>()[0])
                        snprintf(m.dst, m.cap, "%s", v.as<const char *>());
                }
            }
            // "assets": every image this theme actually ships. See hasAsset() in the
            // header for why an undeclared file on the card must be ignored rather than
            // trusted. Absent list -> s_assetN stays 0 -> hasAsset() answers true for
            // everything, which is the old behaviour.
            if (doc["assetsHash"].is<uint32_t>()) s_assetsHash = doc["assetsHash"].as<uint32_t>();
            JsonArrayConst list = doc["assets"].as<JsonArrayConst>();
            if (!list.isNull()) {
                s_assetN = 0;
                for (JsonVariantConst v : list) {
                    const char *n = v.as<const char *>();
                    if (!n || !*n) continue;
                    if (s_assetN >= MAX_ASSETS) {
                        printf("[theme_style] more than %d assets declared, ignoring the rest\n",
                                      (int)MAX_ASSETS);
                        break;
                    }
                    strncpy(s_asset[s_assetN], n, sizeof(s_asset[0]) - 1);
                    s_asset[s_assetN][sizeof(s_asset[0]) - 1] = '\0';
                    ++s_assetN;
                }
                printf("[theme_style] theme declares %d asset(s)\n", (int)s_assetN);
            }
        }
    }
}

const Clock &clock() { return s_clock; }
const Radar &radar() { return s_radar; }
const Menu &menu() { return s_menu; }
const Settings &settings() { return s_settings; }
const Apps &apps() { return s_apps; }
const Names &names() { return s_names; }

void labelFor(const char *slug, char *out, size_t cap) {
    if (!out || !cap) return;
    snprintf(out, cap, "%s", (slug && slug[0]) ? slug : "");   // slug is the fallback label
    if (!slug || !slug[0]) return;
    JsonDocument doc;
    if (!read_style_json(slug, "theme.json", doc)) return;
    if (doc["name"].is<const char *>() && doc["name"].as<const char *>()[0])
        snprintf(out, cap, "%s", doc["name"].as<const char *>());
}

const char *themeLabel() {
    if (s_names.theme[0]) return s_names.theme;
    const char *slug = theme_select::activeSlug();
    return (slug && slug[0]) ? slug : "Stock";
}

uint32_t assetsFingerprint() {
    // Prefer Launch Kit's "assetsHash", which covers the asset CONTENTS. The name-only
    // hash below cannot see a replaced image: swap a background for a different picture
    // of the same name and the fingerprint never moves, so the device keeps serving the
    // previously baked pixels and the new artwork silently never appears.
    if (s_assetsHash) return s_assetsHash;
    if (!s_assetN) return 0;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < s_assetN; ++i) {
        for (const char *p = s_asset[i]; *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
        h ^= (uint8_t)'\n'; h *= 16777619u;
    }
    return h ? h : 1u;      // never collide with the "no manifest" sentinel
}

bool hasAsset(const char *name) {
    if (!s_assetN) return true;      // theme declared no list: allow everything (old themes)
    if (!name || !*name) return false;
    for (size_t i = 0; i < s_assetN; ++i)
        if (strcmp(s_asset[i], name) == 0) return true;
    return false;
}

} // namespace theme_style
