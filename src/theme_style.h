#pragma once
// Per-theme visual style (colors/positions/formats/geometry) — the runtime half of
// the multi-theme SD system. Art (plate/overlay/hand/blip PNGs) already travels per
// theme via /themes/<slug>/*.png (see theme_sd.h + each screen's own decode_sd_first
// pattern). Until this module existed, STYLE (everything a Launch Kit push bakes as a
// CUSTOM_* #define into custom_clock.h/custom_radar.h/custom_settings.h/custom_menu.h)
// was compile-time only — one shared firmware binary, so switching the active SD theme
// via Settings > Design swapped the art but not the color/format/layout, which stayed
// stuck on whichever theme's screen was pushed last (see git history/PRs referencing
// "theme bleeding"). This reads a small per-theme JSON file alongside the art
// (/themes/<slug>/{clock,radar,settings,menu}_style.json, written by Launch Kit on
// every push — see server.js's writeSimSdAsset("*_style.json", ...) call sites) and
// overrides the compiled CUSTOM_* defaults with it, per field, per theme.
//
// NOT covered (known, deliberate limitations — same class of "compile-time only" gap
// that already existed for these before this module, unchanged by it):
//   - Fonts (CUSTOM_*_FONT) — real compiled LVGL glyph bitmaps, not simple values;
//     making these travel per-theme needs LVGL's binary font/lv_fs runtime-loading
//     path, a separate, much larger undertaking. Whichever theme's screen was pushed
//     last still wins the font family/size/weight.
//   - Radar blip icon pivot and radar layer order — plain numbers, but tightly coupled
//     to whichever blip PNG is actually decoded (a pivot only makes sense against its
//     own image's pixel dimensions).
//
// FIXED since this comment was written (these now DO travel per theme):
//   - Clock hand art, pivot, center, blend, draw order, and the per-hand show/hide
//     gates. Art comes from /themes/<slug>/clock_hand_{hour,minute,second}.png and
//     clock_static{1,2}.png; geometry from the "hands" block in clock_style.json.
//   - Which apps appear in the knob menu, from /themes/<slug>/theme.json.
//   - Radar's operational params (home lat/lon, range, max aircraft, hide-ground,
//     min-altitude) — device config baked alongside style in the same header, but
//     already just a one-time boot default the user can (and typically does)
//     override live afterward; a stale default is a minor inconvenience, not a
//     visual bug.
//   - The CUSTOM_HAS_* show/hide gates themselves (whether a banner/highlight/menu
//     slot exists at all) stay compile-time: whichever theme was pushed last decides
//     if the code path is compiled in. If it is, this module makes ITS VALUES correct
//     per active theme; if a different installed theme never used that element at
//     all, it may still show (with that theme's own values, or the compiled default)
//     rather than correctly staying hidden.
#include <lvgl.h>

namespace theme_style {

struct ClockText {
    bool     show   = false;
    int      x      = 233;
    int      y      = 233;
    uint32_t color  = 0xF2F5F9;
    int      glow   = 0;
    uint32_t glowColor = 0xF2F5F9;
    char     fmt[32] = "";
    bool     curved = false;
    int      curveR = 0;
    float    arcDeg = 0.0f;
    int      align  = 0;   // 0 left, 1 center, 2 right
};

// One rotating (or static) clock-face image layer. `show` is the runtime replacement for
// the CUSTOM_HAS_{HOUR,MINUTE,SECOND,STATIC1,STATIC2} compile-time gates: a theme that
// has no second hand sets show=false and no second hand is drawn, whatever the last
// flashed theme happened to compile in. Geometry travels with the art because a pivot is
// only meaningful against its own image's pixel dimensions.
struct Hand {
    bool show    = false;
    int  pivotX  = 0,   pivotY  = 0;
    int  centerX = 233, centerY = 233;
    int  blend   = 0;
};

struct Clock {
    uint32_t  bg = 0x000000;
    // Rotate the whole background plate in lockstep with a hand: 0 none, 1 hour,
    // 2 minute, 3 second. Launch Kit's "Rotate with" control on the background.
    //
    // The exported plate PNG is baked with only its static rotation applied (the editor
    // explicitly leaves the follow angle out), so the device adds the live hand angle on
    // top. Without this the border sat at one fixed angle while the hand moved, lining up
    // once an hour by coincidence.
    int       plateFollow = 0;
    ClockText text1;
    ClockText text2;
    Hand      hand[5];                        // 0=hour 1=minute 2=second 3=static1 4=static2
    int       order[5] = { 3, 4, 0, 1, 2 };   // back-to-front draw order, kind indices
    int       orderN   = 5;
};

// Which apps this theme puts in the knob menu. Was custom_apps.h, compiled in, so it was
// one roster for the whole device rather than one per theme. Settings is deliberately
// absent: it is a system screen, not an app, and a theme that could switch it off would
// strand the user with no way back to WiFi, brightness, or theme selection.
struct Apps {
    bool clock        = true;
    bool flight       = true;
    bool weather      = true;
    bool intel        = true;
    bool surveillance = true;
};

// Display names, kept strictly separate from the identifiers they label.
//
// The identifiers here — the theme's folder slug, and the app keys "clock", "flight" and
// so on — are permanent. They are paths on the card, NVS values, and struct fields, so
// renaming one orphans data on every card already in the wild. The names below are just
// labels: a theme may call Flight Tracker whatever suits it, and change its mind, without
// anything underneath moving.
//
// Not having this distinction cost real time: the theme displayed as "Modern" lives in a
// folder called `the-office` (its former name), so "push Modern to the Orb" and
// "/themes/the-office/" looked like unrelated things.
struct Names {
    char theme[32]        = "";              // the theme's own label, e.g. "Modern"
    char clock[20]        = "Clock";
    char flight[20]       = "Flight Tracker";
    char weather[20]      = "Weather Radar";
    char intel[20]        = "Intel";
    char surveillance[20] = "Surveillance";
    char settings[20]     = "Settings";      // renameable, but never hideable
};

struct RadarText {
    bool     show   = false;
    int      x      = 233;
    int      y      = 233;
    uint32_t color  = 0xFFFFFF;
    int      glow   = 0;
    uint32_t glowColor = 0xFFFFFF;
    char     fmt[80] = "";
    bool     curved = false;
    int      curveR = 0;
    float    arcDeg = 0.0f;
    int      align  = 0;
};

// A plain decorative overlay (Static 1/2, see custom_radar_static.h) — no
// rotation/pivot, just a position and opacity. New with this struct itself
// (no legacy compile-time macro carries a default), so a missing/older
// radar_style.json just leaves it hidden (show=false), not mis-positioned.
struct RadarStatic {
    bool  show = false;
    int   x = 233;
    int   y = 233;
    int   opacity = 255;
    float scale = 1.0f;
};

struct Radar {
    bool     sweepEnabled    = true;
    bool     sweepTypeImage  = false;  // new with this field — see RadarStatic above for why there's no compiled-macro fallback
    uint32_t sweepColor      = 0x39FF14;
    uint32_t sweepLeadColor  = 0xC8FFB0;
    int      sweepTrailDeg   = 38;
    int      sweepOpacity    = 60;    // 0..100
    int      sweepLength     = 233;
    int      sweepSpeed      = 45;

    bool     blipEnabled     = true;  // new with this field — see RadarStatic above for why there's no compiled-macro fallback
    bool     blipTypeImage   = false;
    bool     blipRotate      = true;  // new with this field — see RadarStatic above for why there's no compiled-macro fallback
    bool     blipKiteShape   = false;
    int      blipSize        = 9;
    int      blipKiteT       = 0;     // 0..100
    bool     blipFixedColorMode = false;
    uint32_t blipFixedColor  = 0x39FF14;
    uint32_t blipAltGround   = 0x888888;
    uint32_t blipAltLow      = 0xFF5A3C;
    uint32_t blipAltMid      = 0xFFB23C;
    uint32_t blipAltHigh     = 0xC8FF3C;
    uint32_t blipAltCruise   = 0x39FF14;
    uint32_t blipAltJet      = 0x3CE0FF;
    int      blipGlow        = 0;
    uint32_t blipGlowColor   = 0xFFFFFF;
    bool     blipImageTint   = true;

    bool     selEnabled      = true;
    int      selStyle        = 0;  // 0=ring, 1=glow the aircraft, 2=recolor the aircraft — new with this field, see RadarStatic above for why there's no compiled-macro fallback
    uint32_t selColor        = 0xFF9D3C;
    int      selWidth        = 2;
    int      selDiameter     = 30;
    int      selGlow         = 0;
    uint32_t selGlowColor    = 0xFF9D3C;

    bool     offRangeEnabled = true;
    uint32_t offRangeColor   = 0xFF9D3C;
    int      offRangeSize    = 5;

    bool     centerEnabled     = true;  // new with this field — see RadarStatic above for why there's no compiled-macro fallback
    int      centerRadius      = 6;
    uint32_t centerColor       = 0xFF9D3C;
    int      centerInnerRadius = 2;
    uint32_t centerInnerColor  = 0x0B1F0F;

    RadarText rtext[4];
    RadarStatic static1, static2;

    bool     overlayEnabled  = false;  // new with this field — see RadarStatic above for why there's no compiled-macro fallback
    uint32_t overlayColor    = 0x000000;
    int      overlayOpacity  = 0;      // 0..255, same convention as RadarStatic::opacity
    // Scope behaviour, not appearance — but theme data all the same, because these were
    // compile-time macros (CUSTOM_RADAR_MAXAC / MINALT / HIDEGROUND) baked in by a Launch
    // Kit firmware push. A theme installed as data alone, which is what Orb Studio makes,
    // had no way to express them. -1 means "not specified": keep whatever the welded
    // default or the user's saved setting already chose.
    int      maxAircraft     = -1;     // how many contacts the scope follows at once
    int      minAltFt        = -1;     // ignore anything below this altitude
    int      hideGround      = -1;     // 1 = never show aircraft on the ground, 0 = show, -1 = unset
};

struct MenuText {
    bool     show  = false;
    int      x     = 233;
    int      y     = 233;
    uint32_t color = 0xFFFFFF;
    int      glow  = 0;
    uint32_t glowColor = 0xFFFFFF;
    char     fmt[64] = "{name}";
    int      align = 0;
    // Word wrap for long app names. 0 = never wrap, draw on one line however wide it
    // gets (which is what ran "Flight Tracker" off the edge of the dial). Above 0, the
    // string breaks on spaces once it exceeds this many pixels, and the resulting stack
    // is centred as a block on y, so one-word and two-word names both sit right.
    int      wrapWidth = 0;
    int      lineGap   = 0;    // extra pixels between stacked lines
    // Exact pixel distance between stacked line centres, computed by Launch Kit. Used
    // verbatim when > 0; the lineH + lineGap fallback below only serves older themes.
    int      lineStep  = 0;
};

struct Menu {
    MenuText current;
    MenuText prev;
    MenuText next;
};

struct Settings {
    float    wheelR       = 170.0f;
    float    wheelRx      = 18.0f;
    float    wheelStepDeg = 22.0f;
    float    wheelCy      = 0.0f;
    float    wheelFade    = 2.0f;
    uint32_t selColor     = 0xFFFFFF;
    uint32_t itemColor    = 0x6A7078;
    int      glow         = 0;
    uint32_t glowColor    = 0xFFFFFF;
    bool     hlShow       = true;
    uint32_t hlColor      = 0x232A36;
    int      hlOpacity    = 255;
    int      hlW          = 300;
    int      hlH          = 44;
    int      hlRadius     = 10;
    int      defaultSel   = 0;
};

// Reads /themes/<slug>/{clock,radar,settings,menu}_style.json (theme_select::activeSlug())
// and populates the runtime structs below, field by field — any file that's missing, or
// any field a file doesn't set, keeps the CUSTOM_* compile-time default (so a theme
// exported before this module existed, or a stock/no-design build, behaves exactly as
// before). Call once at boot, right after theme_select::init() resolves the active slug
// (theme_select::set() always triggers a real reboot/re-exec, so init() — and this —
// naturally reruns on every theme switch too; no live-reload path needed).
void load();

const Clock    &clock();
const Radar     &radar();
const Menu      &menu();
const Settings  &settings();
const Apps      &apps();      // from /themes/<slug>/theme.json
const Names     &names();     // display labels; see the Names comment on why these are not ids

// The theme's display name, falling back to its slug when it has none. Use this anywhere
// a person reads it (Settings > Design, /health), never the raw slug.
const char *themeLabel();

// The display name for any installed theme, not just the active one — Settings > Design
// lists them all. Falls back to the slug when a theme declares no name. Reads that
// theme's theme.json, so call it when a page opens, not per frame.
void labelFor(const char *slug, char *out, size_t cap);

// Does the active theme actually contain this asset, e.g. "menu_plate.png"?
//
// theme.json carries an "assets" list of every image the theme ships. Launch Kit rebuilds
// it from what is genuinely on disk at push time, so a layer it decided not to ship (a
// fully transparent overlay, say) is absent from the list as well as from the folder.
//
// This exists because pushing a theme never deletes anything from the card: files from
// older pushes just sit there, and the firmware kept finding them, decoding them, baking
// them into flash and drawing them. Measured on the Steam Punk card: two empty overlay
// layers nobody had shipped in months, costing 1.3 MB of flash to draw nothing.
//
// A theme whose theme.json has no "assets" list answers true for everything, so older
// themes already on a card behave exactly as before.
bool hasAsset(const char *name);

// A hash of the declared asset list, or 0 when the theme declares none. theme_art stores
// this alongside a bake and re-bakes whenever it changes, so editing a theme's layers is
// picked up on the next boot without anyone remembering to invalidate a cache.
uint32_t assetsFingerprint();

} // namespace theme_style
