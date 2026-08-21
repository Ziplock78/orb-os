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
//
// FIXED since this comment was written (these now DO travel per theme):
//   - Clock hand art, pivot, center, blend, draw order, and the per-hand show/hide
//     gates. Art comes from /themes/<slug>/clock_hand_{hour,minute,second}.png and
//     clock_static{1,2}.png; geometry from the "hands" block in clock_style.json.
//   - Which apps appear in the knob menu, from /themes/<slug>/theme.json.
//   - Radar sweep/blip rotation pivots, and the radar layer order.
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

// What this firmware understands of a theme, as one number a designer's tool can ask for.
//
// The version string is no use for this. FW_VERSION tracks releases and sat at 1.3.24
// across several new theme settings, so a design tool comparing versions would have said
// "up to date" about an Orb that silently dropped half of what it was sent. That is not a
// hypothetical: it cost an afternoon chasing a sweep hand that would not move behind the
// aircraft, on an Orb whose firmware simply had no idea the setting existed.
//
// So: bump this by one whenever the firmware learns to read a NEW theme setting, and add a
// line to the ledger. Never renumber, never reuse. Orb Studio keeps the matching table of
// which setting needs which level, and refuses to install a design the Orb would not
// honour rather than letting it look installed.
//
// Firmware older than this constant reports no caps field at all, which a tool should read
// as level 0: assume nothing, verify nothing.
//
//   1  radar layer order, keep-out areas, synthesised test traffic, and sweep/aircraft
//      rotation pivots as theme data
//   2  the menu's per-slot show flag: a design can drop the previous/next hints and keep
//      only the centred app name
//   3  the clock's two live text banners (text1/text2), drawn from the theme's own show
//      flag instead of whichever CUSTOM_HAS_TEXT{1,2} a past firmware push happened to
//      compile in
//   4  hand shadows cast by a FIXED light: a separate pre-blurred silhouette sprite per
//      hand, drawn at the same angle as the hand but offset in SCREEN space, so the shadow
//      falls the same way whatever hour it is. A theme baking its shadow into the hand
//      sprite instead needs nothing from the firmware and still works below this.
//   5  the flight tracker's own furniture, all of it added in one sitting and all of it
//      invisible to an Orb below this level:
//        - the selection card (radar_card.png plus the `card` block) and the readout lines'
//          runtime show/onCard flags. A line laid out for the card reads its across/down as
//          offsets from the card's centre, so on older firmware it does not merely lose the
//          card, it stacks near the middle of the dial with nothing behind it.
//        - the sweep's own trail geometry: sweepTrailWidth / sweepLeadWidth /
//          sweepTrailSteps, which were 5 / 2 / 20 welded into sweep_draw_cb.
//        - the map's colours: mapRoadsOn / mapRoadColor / mapRoadOpacity / mapAirportsOn /
//          mapAirportColor, previously a fixed grey drawn whether or not it was wanted.
//        - rangeKm: how far the rim is. Launch Kit could set this only by recompiling
//          (CUSTOM_RADAR_RANGE_KM), so a files-only theme had no way to state it at all.
//   6  the rings and crosshair as their OWN etched plate (radar_rings.png), drawn above
//      the map instead of baked into the background beneath it. Scope furniture could not
//      stay readable over busy roads while it lived under them. An Orb below this level
//      finds no such asset and draws a background with no rings on it at all, which is
//      why `ringsPlate` below exists to be refused rather than silently dropped.
//   7  mapRoadWidth: how heavy the roads are drawn. roads_sd::draw has always taken a
//      width and every call site passed a literal 1, so a theme could pick the roads'
//      colour and opacity but never their weight. An Orb below this draws hairlines.
//   8  the sweep's own hub: a disc at the pivot the hand turns about, with its own colour,
//      size and glow. The sweep drew lines out of a bare centre and the only thing ever at
//      the middle of the dial was the aircraft layer's centre mark, which belongs to the
//      aircraft and travels with them through the stack.
constexpr int THEME_CAPS = 8;

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
    // Shadows cast by ONE light that does not move with the hands.
    //
    // The hand's own sprite cannot carry this: a shadow painted into it turns with the
    // hand, which reads as a lamp orbiting the dial. So each hand gets a second sprite,
    // clock_shadow_{hour,minute,second}.png, holding its silhouette already blurred and
    // already in the shadow's colour and opacity. That art is drawn at the hand's angle
    // about the hand's own pivot, but centred dx/dy away in SCREEN space, which is what
    // keeps the shadow pointing the same way all the way round the dial.
    //
    // Only the offset lives here. Softness, colour and strength are baked, so they cost
    // the device nothing at all and the runtime work is one ordinary rotate-and-blend.
    bool      shadowOn = false;
    int       shadowDX = 0, shadowDY = 0;     // px, screen space, applied to every hand
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
    // Ride the selection card instead of sitting at a fixed spot on the scope. When set,
    // x/y stop being screen coordinates and become an offset from the card's own centre —
    // so the line travels with the card as the card chases the far side of the dial.
    bool     onCard = false;
};

// The selection card: a little plate that appears when an aircraft is picked, always on
// the OPPOSITE side of the scope from that aircraft, so the thing you just selected is
// never sitting under the words describing it. Vector (a rounded rect) or an image.
struct RadarCard {
    bool     enabled    = false;
    bool     typeImage  = false;
    int      radius     = 120;   // px from the scope's centre to the card's centre
    int      w          = 150;   // vector card size; an image card uses its own pixels
    int      h          = 60;
    int      corner     = 10;    // vector corner rounding
    uint32_t color      = 0x101418;
    int      opacity    = 220;   // 0..255
    uint32_t borderColor = 0x39FF8A;
    int      borderWidth = 1;
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
    // The trail's own line work, hard-coded until now (a 5 px trail of 20 steps behind a
    // 2 px leading edge). Defaults below are exactly those numbers, so a theme that does
    // not mention them looks the same as it always did.
    int      sweepTrailWidth = 5;     // px, thickness of each trail line
    int      sweepLeadWidth  = 2;     // px, thickness of the solid leading edge
    int      sweepTrailSteps = 20;    // how many lines the fading wedge is made of

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
    RadarCard card;
    // The map the Orb carries: real OSM roads around wherever it is, drawn under the
    // scope's chrome. Always on and always the same grey until now, which a dark themed
    // dial had no way to argue with. Defaults are the colours it has always used.
    bool     mapRoadsOn      = true;
    uint32_t mapRoadColor    = 0x707868;
    int      mapRoadOpacity  = 150;   // 0..255
    bool     mapAirportsOn   = true;
    uint32_t mapAirportColor = 0x8A93A6;
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
    // How far the rim is, in km. -1 means the theme has no opinion and the Orb keeps its
    // own zoom, the same sentinel minAltFt above uses. Launch Kit could set this only by
    // recompiling (CUSTOM_RADAR_RANGE_KM), so a files-only theme had no way to say it.
    float    rangeKm         = -1.0f;
    int      hideGround      = -1;     // 1 = never show aircraft on the ground, 0 = show, -1 = unset
    // Synthesised traffic instead of the live feed. For judging a design without waiting
    // on whatever happens to be overhead, and for watching masking behave against motion
    // that is predictable rather than whatever the sky is doing.
    bool     simulate        = false;

    // Rotation pivots for image-type sweeps and blips, in their own image's pixels.
    //
    // These were CUSTOM_SWEEP_IMAGE_PIVOT_* / CUSTOM_RADAR_BLIP_PIVOT_*, compiled in by
    // whichever Launch Kit firmware push ran last. A theme installed as files alone could
    // therefore ship a sweep sprite and have it spun around a point measured for somebody
    // else's artwork — which is not a subtle fault: a hand pivoting 40 px off its hub
    // wobbles instead of turning. -1 keeps the welded value, so an older theme is
    // unaffected.
    int      sweepPivotX     = -1;
    int      sweepPivotY     = -1;
    int      sweepCenterX    = -1;   // where on the dial that pivot sits
    int      sweepCenterY    = -1;
    int      blipPivotX      = -1;
    int      blipPivotY      = -1;

    // Exclusion zones: circles on the 466x466 dial where aircraft are not drawn.
    //
    // These exist so decorative artwork can live in the BAKED BACKGROUND instead of in a
    // layer above the aircraft. Measured 2026-08-17: Steam Punk's brass bezel, as a layer
    // above the movers, cost 24% of the radar's frame rate; the same art baked into the
    // background costs nothing at all. A zone gets the same visual result — aircraft never
    // cross the decoration — for a handful of comparisons per poll.
    //
    // Aircraft inside a zone vanish and reappear on the far side. They are hidden, not
    // dropped: a tracked contact keeps its slot while masked, or the scope would discard
    // it on entering and adopt a replacement, which is the churn sticky tracking removes.
    //
    // A zone is a circle or an axis-aligned rectangle, and it can be inverted. Inverted
    // means "hide OUTSIDE this shape", which turns one zone into a containment ring: put
    // a big inverted circle just inside the dial's border and aircraft can never encroach
    // on it, replacing a border overlay — another layer above the movers, another 24%.
    static constexpr int MAX_ZONES = 6;
    struct Zone {
        int  x = 233, y = 233;
        int  r = 0;                 // circle radius, when rect is false
        int  w = 0, h = 0;          // full width/height centred on x,y, when rect is true
        bool rect   = false;
        bool invert = false;        // true = hide outside the shape instead of inside it
    };
    Zone     zones[MAX_ZONES];
    int      zoneCount       = 0;

    // Back-to-front draw order for the movable layers, same six kinds and the same
    // convention as CUSTOM_RADAR_LAYER_ORDER: 0=sweep, 1=aircraft, 2=text, 3=static1,
    // 4=static2, 5=colour wash.
    //
    // This was the last item still listed as compile-time only at the top of this file.
    // It mattered once a theme could be installed as files alone: a Studio theme that
    // wants its sweep hand passing OVER the aircraft rather than under them had no way
    // to say so, and inherited whatever order the last firmware push happened to weld
    // in. orderN == 0 means "not specified", which keeps exactly that welded order, so
    // every theme made before this field is unaffected.
    int      order[6]        = { 0, 1, 2, 3, 4, 5 };
    int      orderN          = 0;

    // This theme ships radar_rings.png and its background plate therefore has no rings
    // baked in. Purely a declaration for Orb Studio to check against THEME_CAPS: the
    // firmware draws whatever asset it finds either way, but an Orb that cannot draw it
    // must refuse the theme rather than show a dial with no grid on it.
    bool     ringsPlate      = false;

    // How heavy the roads are, in pixels. Welded at 1 until THEME_CAPS 7.
    int      mapRoadWidth    = 1;

    // The sweep's hub: the disc at the point the hand turns about. Drawn as part of the
    // sweep layer, so it travels with the hand through the stack rather than sitting at a
    // fixed depth. Off by default, which is what every theme made before this looked like.
    bool     sweepHubOn      = false;
    uint32_t sweepHubColor   = 0x39FF8A;
    int      sweepHubRadius  = 6;
    int      sweepHubGlow    = 0;      // px of halo beyond the disc, 0 = none
    uint32_t sweepHubGlowColor = 0x39FF8A;
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
