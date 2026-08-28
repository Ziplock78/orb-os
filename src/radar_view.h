#pragma once
// Scope rendering API (M1 scope, M2 aircraft, M3 selection). See docs/ARCHITECTURE.md.
// Visual reference: upstream's assets/plane_radar_2.0_mockup.html, which describes the STOCK
// phosphor skin only. Everything a theme controls is in theme_style.h.
#include <vector>
#include "aircraft.h"

struct RadarSettings {
    double homeLat, homeLon;
    float  rangeKm;
    double rotationDeg = 0.0;   // 0 = north-up
    bool   mute = false;
};

// Selectable visual skins. (Phosphor and Amber CRT were retired — Orb, Military, and
// Aviator covered everything anyone actually used.)
enum RadarTheme {
    THEME_ORB      = 0,   // Orb scope: green gradient, grid, yellow blips
    THEME_MILITARY = 1,   // night-vision / military green scope
    THEME_AVIATOR  = 2,   // WWII aviator scope: brass/ivory chrome, matches the clock + Location dials
    THEME_COUNT    = 3
};

// Flattened, display-ready info for one aircraft (detail card / list view).
struct AcInfo {
    char  hex[8];
    char  call[12];
    char  type[8];
    float altFt;
    bool  onGround;
    float vsFpm;        // NaN if unknown
    float gsKt;         // NaN if unknown
    float distKm;
    float bearingDeg;
    int   squawk;       // -1 if unknown
    bool  emergency;
};

namespace radar {

// Build the radar scope (rings, crosshair, rose, sweep, center) under `parent`.
void init(void* lv_parent);                 // pass lv_obj_t*

// Rebuild the aircraft layer from the latest snapshot. Call at poll cadence.
void update(const std::vector<Aircraft>& aircraft, const RadarSettings& s);

// Nearest aircraft to (x,y) within a tap radius -> snapshot index, or -1.
int  hitTest(int x, int y);

// Selection (tracked by hex so it survives data updates). idx < 0 clears.
void select(int idx);
bool selected(AcInfo& out);                 // false if nothing selected/visible

// Knob-driven cycling through in-range aircraft, for a Launch Kit custom push:
// the shell captures the knob on Flight Tracker and each detent calls this.
// The cycle includes an explicit "nothing selected" stop (reachable from either
// end), so turning far enough always gets you back to no selection, not just a
// separate gesture. dir: +1 next (turn right), -1 previous (turn left).
void selectNext(int dir);

// Flight Tracker knob state machine, wired identically by the device (main.cpp)
// and the simulator (sim_main.cpp) so they can't drift. Default view = nothing
// selected, knob released (a turn opens the app switcher); a push enters selection
// mode (knob captured, a turn cycles aircraft); a second push or 5s of no input
// drops back to the default view. Hosts call knobEnter() at the tail of the app's
// onEnter (after showing the scope), and wire knobPress/knobTurn/knobExit as the
// app's onPress/onTurn/onExit.
void knobEnter();
void knobPress();
void knobTurn(int dir);
void knobExit();

// Snapshot access for the list / stats views.
int  count();
int  countInRange();                        // aircraft within the display range (for the HUD)
bool info(int idx, AcInfo& out);

// Sweep self-animates via an internal timer; kept for API compatibility.
void tickSweep();

// Selectable visual skin (THEME_ORB / THEME_MILITARY / THEME_AVIATOR).
void setTheme(int theme);
int  theme();
const char *themeName(int theme);                // "ORB" / "MILITARY" / "AVIATOR" (bounds-checked)
void cycleTheme();
void flashThemeName();                           // briefly show the current theme's name banner (on touch)
void setThemeChangedCb(void (*cb)(int theme));   // called when the theme changes (for persistence)
void setRangeLabelVisible(bool v);               // hide the built-in range label (UI shows its own)

// Late-arriving detail (the route line) reached a card that is already on screen. Restarts
// the selection idle countdown so the newly-arrived text gets a full reading window.
void noteSelectionDetailArrived();

// Retune the sweep timer at runtime. Frame pacing is a measured tradeoff between rate and
// evenness, and the right value depends on what the device can actually render — which
// changed materially once internal memory stopped being exhausted. Being able to sweep
// through candidate values without a reflash each time turns a 6-minute experiment into a
// 90-second one. 0 restores the compiled default.
void setSweepFrameMs(uint32_t ms);

// Tell the scope whether the WiFi is up and how long since the last aircraft. It shows a
// small banner naming the actual culprit once a gap is real (45 s), because a blank scope
// looks the same from the desk whether the network dropped, the firmware wedged, or the
// feed provider is having a bad night — and it is nearly always the last one.
void setFeedStatus(bool wifiUp, uint32_t staleSec);

// Replace the "Loading aircraft and location data" notice with the truth, while it is still
// up. That notice only clears when the first aircraft arrive, so a feed that never answers
// left it saying "loading" indefinitely, which is the device telling the person in front of
// it something that is not true. Pass nullptr to put the original wording back.
void setFeedNote(const char *note);
void setSimulatedBadge(bool on);                 // "TEST DATA" mark, forced on whenever the active theme fakes its traffic
// Diagnostic only: hide one layer at runtime so its per-frame cost can be priced
// by difference, instead of reflashing once per hypothesis. kind: 0=sweep,
// 1=aircraft, 2=text, 3=static1, 4=static2, 5=wash, 6=plate. Not persisted.
void debugHideLayer(int kind, bool hide);
void setSweepEnabled(bool on);                   // show/hide the rotating sweep line
bool sweepEnabled();
void setAirportsEnabled(bool on);                // show/hide airport markers on the scope
bool airportsEnabled();
void setTrailLength(int level);                  // 0=off 1=short 2=medium 3=long (aircraft trails + flow)
void setMaxOnScreen(int n);                       // how many (nearest) aircraft to draw on the scope
void setLargeText(bool on);                       // accessibility: bigger glyph labels. Call BEFORE init()

// Re-attach the baked plate/overlay image sources (decoding lazily if needed).
// Call from Flight Tracker's onEnter — a matching onExit calls
// radar_sprite_release() to free the decoded PSRAM while some other app is on
// screen, so this re-decode-on-entry keeps the image objects' src valid.
void refreshCustomStyle();

} // namespace radar
