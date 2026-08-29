// Native (Mac/Linux) LVGL simulator for The Orb OS.
// Runs the SAME LVGL UI (include/lv_conf.h + ui_boot) in an SDL2 window — no
// hardware, no Arduino_GFX. Only compiled for the `native` PlatformIO env.
//
//   pio run -e native            # build
//   pio run -e native -t exec    # build + run (or run .pio/build/native/program)
//
// Note: the real panel is a 466x466 *round* AMOLED; this square window shows the
// full buffer, so the corners (hidden on the device) are visible here.
#include <SDL.h>
#include <math.h>
#include <lvgl.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "config.h"
#include "radar_view.h"
#include "radar_sprite.h"   // radar_sprite_release() — Flight Tracker's onExit
#include "roads_sd.h"       // roads_sd::set_root() — this desktop build's stand-in for the SD card
#include "ui.h"
#include "route.h"
#include "weather.h"
#include "wx_radar.h"
#include "wx_radar_client.h"
#include "cloud_image.h"
#include "aircraft.h"
#include "clock_view.h"
#include "intel_view.h"
#include "ticker_view.h"
#include "ticker.h"
#include "app_shell.h"
#include "app_theme.h"
#include "theme_select.h"
#include "update_ui.h"   // --updateshot, below
#include "theme_style.h"   // per-theme app roster (theme_style::apps())
#include "settings_view.h"
#include "custom_boot_target.h"  // CUSTOM_BOOT_TARGET — set by whichever Launch Kit push (clock/splash/radar) ran last
#include "custom_apps.h"         // CUSTOM_APP_* — which apps a theme flash includes in the menu
#include "custom_radar.h"        // CUSTOM_HAS_RADAR — a Launch Kit push changes the Flight Tracker knob's behavior
#include "knob.h"           // consumed-input API (implemented by sim_knob.cpp on native)
#include "sim_knob.h"       // inject SDL events into the knob:: backend
#include "input_router.h"   // shared knob->app_shell routing (same as the device)
#include "native_http.h"
#include <ArduinoJson.h>
#include <string>

// ---- host_* stubs (sim only): Settings reads these at render time; the real
// definitions live in main.cpp, which isn't part of the native build. Fake,
// reasonable values so the Settings screen renders with real copy and layout.
//
// Location is the exception: host_set_location[_named]/host_locate_current/host_geocode
// hit the SAME real endpoints main.cpp does (ip-api.com for IP-locate, Open-Meteo's
// geocoding API for search), just without the reboot-to-apply the device uses — the sim
// applies immediately via sim_apply_home_location (re-centers the mock radar, re-fetches
// live weather + Intel). Recents are simple in-memory storage here (no NVS on desktop);
// Settings seeds it with a handful of cities (incl. Phoenix) on first run.
static void sim_apply_home_location(const char *name, double lat, double lon);   // defined below

int  host_get_brightness() { return 80; }
void host_set_brightness(int, bool) {}
uint32_t host_get_idle_ms() { return 0; }
void host_set_idle_ms(uint32_t) {}
void host_set_location(double lat, double lon) { sim_apply_home_location("", lat, lon); }

namespace {
    struct SimRecent { char name[40]; double lat, lon; };
    constexpr int SIM_RECENTS_MAX = 8;
    SimRecent g_recents[SIM_RECENTS_MAX];
    int       g_recentCount = 0;
}

int host_recents_get(char names[][40], double *lats, double *lons, int maxN) {
    const int n = (g_recentCount < maxN) ? g_recentCount : maxN;
    for (int i = 0; i < n; ++i) {
        snprintf(names[i], 40, "%s", g_recents[i].name);
        lats[i] = g_recents[i].lat;
        lons[i] = g_recents[i].lon;
    }
    return n;
}

void host_recents_add(const char *name, double lat, double lon) {
    if (!name || !name[0]) return;
    const int n = (g_recentCount < SIM_RECENTS_MAX) ? g_recentCount + 1 : SIM_RECENTS_MAX;
    for (int i = n - 1; i > 0; --i) g_recents[i] = g_recents[i - 1];   // shift down, insert at front
    snprintf(g_recents[0].name, sizeof(g_recents[0].name), "%s", name);
    g_recents[0].lat = lat; g_recents[0].lon = lon;
    g_recentCount = n;
}

// Mirrors host_set_location_named() on the device: record in recents, then apply.
void host_set_location_named(const char *name, double lat, double lon) {
    host_recents_add(name, lat, lon);
    sim_apply_home_location(name, lat, lon);
}

// Approximate current location from the public IP (city-level) — same ip-api.com call
// and fields main.cpp's host_locate_current() uses. On-device this reboots to apply; the
// sim just applies directly and returns (no timezone-offset handling — the sim's clock
// is already a standalone mock, see the 1Hz loop below, so there's no TZ state to sync).
bool host_locate_current() {
    std::string body;
    if (!native_https_get("http://ip-api.com/json/?fields=status,message,city,region,lat,lon,offset",
                          ORB_USER_AGENT, body, 6000)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    if (strcmp(doc["status"] | "", "success") != 0) return false;
    const double lat = doc["lat"] | 1000.0;
    const double lon = doc["lon"] | 1000.0;
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
    const char *city = doc["city"] | "";
    const char *region = doc["region"] | "";
    char nm[40] = "";
    if (city[0]) snprintf(nm, sizeof(nm), "%s%s%s", city, region[0] ? ", " : "", region);
    if (nm[0]) host_recents_add(nm, lat, lon);   // matches the device: recorded directly, not via _named
    sim_apply_home_location(nm[0] ? nm : "current location", lat, lon);
    return true;
}

// Free city search (Open-Meteo geocoding, no key) — same endpoint main.cpp's
// host_geocode() uses. Settings already debounces this to one blocking call per pause
// in typing (see settings_view.cpp's search_tick), so a synchronous curl fetch is fine.
int host_geocode(const char *query, char names[][40], double *lats, double *lons, int maxN) {
    if (!query || strlen(query) < 2) return 0;
    std::string q;
    for (const char *p = query; *p; ++p) q += (*p == ' ') ? std::string("%20") : std::string(1, *p);
    char url[256];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d&language=en&format=json",
             q.c_str(), maxN);
    std::string body;
    if (!native_https_get(url, ORB_USER_AGENT, body, 6000)) return 0;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return 0;
    JsonArrayConst results = doc["results"].as<JsonArrayConst>();
    int n = 0;
    for (JsonObjectConst r : results) {
        if (n >= maxN) break;
        const char *name   = r["name"]   | "";
        const char *admin1 = r["admin1"] | "";
        const char *cc     = r["country_code"] | "";
        const char *sub    = admin1[0] ? admin1 : cc;
        snprintf(names[n], 40, "%s%s%s", name, sub[0] ? ", " : "", sub);
        lats[n] = r["latitude"]  | 1000.0;
        lons[n] = r["longitude"] | 1000.0;
        if (lats[n] <= 90 && lats[n] >= -90) n++;
    }
    return n;
}

int  host_get_volume() { return 70; }
void host_set_volume(int, bool) {}
bool host_sound_radar() { return true; }
void host_sound_set_radar(bool) {}
bool host_sound_chime() { return true; }
void host_sound_set_chime(bool) {}
void host_sound_preview_chime() {}
void host_sound_preview_beep() {}
int  host_chime_count() { return 1; }
const char *host_chime_name(int) { return "Westminster"; }
int  host_chime_index() { return 0; }
void host_chime_set(int) {}
void host_chime_preview(int) {}
void host_wifi_scan_start() {}
int  host_wifi_scan_result(char[][33], int8_t *, bool *, int) { return 0; }
void host_wifi_connect(const char *, const char *) {}
int  host_wifi_connect_status() { return 0; }
void host_wifi_connected_reboot() {}
void host_factory_reset() {}
bool host_wx_is_imperial() { return false; }
int  host_wx_units_mode() { return 0; }
void host_wx_units_set(int) {}
// Settings > Range. The real host persists to NVS and re-queries the feed; the sim just
// holds the value and re-renders, which is enough to exercise the menu and the scope.
static float s_simRangeKm = RANGE_KM_DEFAULT;
float host_get_range_km() { return s_simRangeKm; }
void  host_set_range_km(float km);   // defined below, once g_set/g_mockAcs are in scope
#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SIM_W SCREEN_W   // 466
#define SIM_H SCREEN_H   // 466

static SDL_Window   *s_win = NULL;
static SDL_Renderer *s_ren = NULL;
static SDL_Texture  *s_tex = NULL;   // the 466x466 LVGL framebuffer

// Copy what is on screen into a file. Written once because there were three copies of it
// already and --newsshot would have made a fourth, which is three too many places for a
// pixel format to be wrong in only one of them.
static void sim_save_frame(const char *path) {
    if (!path || !s_ren) return;
    SDL_RenderClear(s_ren);
    SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(s_ren, &ow, &oh);
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surf) return;
    SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
    SDL_SaveBMP(surf, path);
    SDL_FreeSurface(surf);
    printf("[sim] saved %s\n", path);
}

// ---- Orb chrome (interactive only) --------------------------------------------
// The window shows the physical Orb render (sim/orb-frame.bmp) with the live UI
// composited into its round lens, plus a control strip with three buttons that
// drive the encoder. In headless --shot/--gif mode none of this runs: the window
// stays the bare 466x466 buffer so screenshot output is unchanged.
static const int   FRAME_W = 1478, FRAME_H = 1260;    // orb-frame.bmp native size
// Lens center/radius, as fractions of the FULL (uncropped) frame — calibrated once
// visually with SIM_CALIB and left alone; the crop below only changes what part of
// the photo is shown, not where the lens is within it.
static float F_CX = 0.496f, F_CY = 0.3455f, F_R = 0.158f;
// The photo itself isn't centered on the Orb (lots of dead space top/right), so crop
// to the object's actual bounding box (measured directly from the pixels: object
// spans x[369,1182] y[123,1209] of the 1478x1260 source) before displaying it. This
// re-centers the object — and with it the lens — in the window.
static const int CROP_X = 79, CROP_Y = 72, CROP_W = 1394, CROP_H = 1188;

static bool          g_composite = false;
static SDL_Texture  *s_frameTex  = NULL;
static int   g_winW = SIM_W, g_winH = SIM_H, g_frameH = SIM_H, g_barH = 0;
static float g_scx = 0, g_scy = 0, g_sr = 0;   // lens center/radius in logical coords
static int   g_mouseX = 0, g_mouseY = 0;
static bool  g_selectHeld = false;

struct SimButton { SDL_Rect rect; int action; const char *label; Uint32 flashUntil; };  // action: -1 left, +1 right, 0 select, 2 restart
static SimButton g_btns[4];
static int g_btnCount = 0;
static constexpr Uint32 BTN_FLASH_MS = 180;   // click feedback on any button, not just SELECT's hold-glow

// ---- tiny 5x7 bitmap font (uppercase letters used by the button labels) --------
// Each glyph is 7 rows of 5 bits (bit 4 = leftmost). Avoids any font-lib dependency.
struct Glyph { char c; uint8_t rows[7]; };
static const Glyph FONT5x7[] = {
    { ' ', { 0,0,0,0,0,0,0 } },
    { 'A', { 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11 } },
    { 'C', { 0x0E,0x11,0x10,0x10,0x10,0x11,0x0E } },
    { 'E', { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F } },
    { 'F', { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 } },
    { 'G', { 0x0E,0x11,0x10,0x17,0x11,0x11,0x0E } },
    { 'H', { 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 } },
    { 'I', { 0x1F,0x04,0x04,0x04,0x04,0x04,0x1F } },
    { 'L', { 0x10,0x10,0x10,0x10,0x10,0x10,0x1F } },
    { 'O', { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E } },
    { 'R', { 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 } },
    { 'S', { 0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E } },
    { 'T', { 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 } },
};
static const Glyph *glyph_for(char c) {
    for (const Glyph &g : FONT5x7) if (g.c == c) return &g;
    return &FONT5x7[0];   // space fallback for anything unmapped
}
static int text_width(const char *s, int sc) {
    int n = 0; for (const char *p = s; *p; ++p) n++;
    return n > 0 ? n * 6 * sc - sc : 0;   // 5px glyph + 1px gap, no trailing gap
}
// Largest integer scale (min 1) that keeps the label inside maxW — button labels used to
// be drawn at a fixed scale that only happened to fit when there were 3 of them; adding a
// 4th narrowed every button and the text started overflowing into its neighbor.
static int fit_scale(const char *s, int maxW) {
    for (int sc = 3; sc > 1; --sc) if (text_width(s, sc) <= maxW) return sc;
    return 1;
}
static void draw_text(int x, int y, const char *s, int sc, SDL_Color col) {
    SDL_SetRenderDrawColor(s_ren, col.r, col.g, col.b, col.a);
    int cx = x;
    for (const char *p = s; *p; ++p) {
        const Glyph *g = glyph_for(*p);
        for (int r = 0; r < 7; ++r)
            for (int b = 0; b < 5; ++b)
                if (g->rows[r] & (1 << (4 - b))) {
                    SDL_Rect px = { cx + b * sc, y + r * sc, sc, sc };
                    SDL_RenderFillRect(s_ren, &px);
                }
        cx += 6 * sc;
    }
}

// LVGL -> SDL texture. Accumulate dirty areas; present here only in the bare
// (non-composite) path — the composite path presents once per loop instead.
static void sdl_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    SDL_Rect r = { area->x1, area->y1, w, h };
    SDL_UpdateTexture(s_tex, &r, px, w * (int)sizeof(lv_color_t));
    if (!g_composite && lv_disp_flush_is_last(drv)) {
        SDL_RenderClear(s_ren);
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        SDL_RenderPresent(s_ren);
    }
    lv_disp_flush_ready(drv);
}

// Mouse acts as the touch input device. In composite mode the screen is the round
// lens, so map window coords into the round display (only inside the circle).
static void sdl_mouse_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    int x, y;
    Uint32 btn = SDL_GetMouseState(&x, &y);
    const bool down = (btn & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    if (g_composite) {
        const float dx = x - g_scx, dy = y - g_scy;
        if (sqrtf(dx * dx + dy * dy) <= g_sr && g_sr > 0) {
            data->point.x = (lv_coord_t)lroundf(233.0f + (dx / g_sr) * 233.0f);
            data->point.y = (lv_coord_t)lroundf(233.0f + (dy / g_sr) * 233.0f);
            data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
        return;
    }
    data->point.x = x;
    data->point.y = y;
    data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// Draw the live 466 framebuffer into the round lens, cropped to a circle. Maps
// the inscribed circle of the square buffer (what the device actually shows) onto
// the lens circle, so the hidden corners never appear.
static void render_screen_circle() {
    const int N = 128;
    SDL_Vertex verts[N + 1];
    int idx[N * 3];
    const SDL_Color white = { 255, 255, 255, 255 };
    verts[0].position = { g_scx, g_scy };
    verts[0].color = white;
    verts[0].tex_coord = { 0.5f, 0.5f };
    for (int i = 0; i < N; ++i) {
        const float a = (float)i / N * 2.0f * (float)M_PI;
        const float ca = cosf(a), sa = sinf(a);
        verts[1 + i].position  = { g_scx + g_sr * ca, g_scy + g_sr * sa };
        verts[1 + i].color     = white;
        verts[1 + i].tex_coord = { 0.5f + 0.5f * ca, 0.5f + 0.5f * sa };
    }
    for (int i = 0; i < N; ++i) { idx[i*3] = 0; idx[i*3+1] = 1 + i; idx[i*3+2] = 1 + ((i + 1) % N); }
    SDL_RenderGeometry(s_ren, s_tex, verts, N + 1, idx, N * 3);
}

static void render_buttons(Uint32 now) {
    for (int i = 0; i < g_btnCount; ++i) {
        SimButton &b = g_btns[i];
        const bool hover  = g_mouseX >= b.rect.x && g_mouseX < b.rect.x + b.rect.w &&
                            g_mouseY >= b.rect.y && g_mouseY < b.rect.y + b.rect.h;
        // SELECT glows for as long as it's actually held (mirrors the real knob); the
        // other three are momentary actions, so they get a brief flash on click instead —
        // same highlight color, just timed rather than tied to a held/released state.
        const bool active = (b.action == 0 && g_selectHeld) || (int32_t)(now - b.flashUntil) < 0;
        if (active)     SDL_SetRenderDrawColor(s_ren, 0xC2, 0x41, 0x0C, 255);
        else if (hover) SDL_SetRenderDrawColor(s_ren, 0x3a, 0x34, 0x30, 255);
        else            SDL_SetRenderDrawColor(s_ren, 0x22, 0x1f, 0x1c, 255);
        SDL_RenderFillRect(s_ren, &b.rect);
        if (active) SDL_SetRenderDrawColor(s_ren, 0xFF, 0xB3, 0x66, 255);   // warm border to match the fill
        else        SDL_SetRenderDrawColor(s_ren, 0x50, 0x4a, 0x44, 255);
        SDL_RenderDrawRect(s_ren, &b.rect);

        const int innerPad = 10;
        const int sc = fit_scale(b.label, b.rect.w - innerPad * 2);
        const int tw = text_width(b.label, sc), th = 7 * sc;
        SDL_Color lc = { 0xEE, 0xE6, 0xD3, 255 };
        if (active) { lc.r = 0xFF; lc.g = 0xF3; lc.b = 0xE6; }
        // Clip to the button's own rect: a safety net so a label can never bleed into its
        // neighbor even under future relayouts, not just a one-time fit at today's sizes.
        SDL_RenderSetClipRect(s_ren, &b.rect);
        draw_text(b.rect.x + (b.rect.w - tw) / 2, b.rect.y + (b.rect.h - th) / 2, b.label, sc, lc);
        SDL_RenderSetClipRect(s_ren, nullptr);
    }
}

static void present_composite(Uint32 now) {
    SDL_SetRenderDrawColor(s_ren, 0x15, 0x15, 0x17, 255);
    SDL_RenderClear(s_ren);
    SDL_Rect src = { CROP_X, CROP_Y, CROP_W, CROP_H };
    SDL_Rect fr  = { 0, 0, g_winW, g_frameH };
    SDL_RenderCopy(s_ren, s_frameTex, &src, &fr);
    render_screen_circle();
    if (getenv("SIM_CALIB")) {   // trace the live-screen edge in green to check it lands on the lens bezel
        SDL_SetRenderDrawColor(s_ren, 0, 255, 0, 255);
        SDL_Point ring[181];
        for (int i = 0; i <= 180; ++i) { float a = i / 180.0f * 2.0f * (float)M_PI; ring[i] = { (int)lroundf(g_scx + g_sr * cosf(a)), (int)lroundf(g_scy + g_sr * sinf(a)) }; }
        SDL_RenderDrawLines(s_ren, ring, 181);
    }
    SDL_SetRenderDrawColor(s_ren, 0x2a, 0x27, 0x24, 255);
    SDL_Rect divider = { 0, g_frameH, g_winW, 1 };
    SDL_RenderFillRect(s_ren, &divider);
    render_buttons(now);
    SDL_RenderPresent(s_ren);
}

// Load the frame image and lay out the three control buttons. Sets g_composite
// true on success; on failure the sim falls back to the bare screen.
static void setup_chrome() {
    const char *envF = getenv("SIM_FRAME");
    SDL_Surface *fs = SDL_LoadBMP(envF && *envF ? envF : "sim/orb-frame.bmp");
    if (fs) { s_frameTex = SDL_CreateTextureFromSurface(s_ren, fs); SDL_FreeSurface(fs); }
    if (!s_frameTex) { printf("[sim] frame image sim/orb-frame.bmp not found; using bare screen.\n"); g_composite = false; return; }
    // Short labels on purpose: at 4-across, "SCROLL LEFT"/"SCROLL RIGHT" no longer fit
    // their button without shrinking the font past legibility (see fit_scale()).
    const struct { const char *label; int action; } defs[4] =
        { { "LEFT", -1 }, { "SELECT", 0 }, { "RIGHT", +1 }, { "RESTART", 2 } };
    const int pad = 18, gap = 14, bh = 54;
    const int by = g_frameH + (g_barH - bh) / 2;
    const int bw = (g_winW - pad * 2 - gap * 3) / 4;
    for (int i = 0; i < 4; ++i) {
        g_btns[i].rect = { pad + i * (bw + gap), by, bw, bh };
        g_btns[i].action = defs[i].action;
        g_btns[i].label = defs[i].label;
    }
    g_btnCount = 4;
    g_composite = true;
}

// ---- mock ADS-B data (sim only): 6 aircraft near Dénia that drift along track --
static std::vector<Aircraft> g_mockAcs;
static std::vector<Aircraft> g_mockInit;
static RadarSettings g_set;

// Home location for the whole simulator — the Launch Kit design's pushed
// Latitude/Longitude when a design is active, else config.h's default. The mock
// aircraft, the scope center, and the live weather/intel/cloud fetches all key
// off this, so the simulator centers exactly where the editor and the Orb do
// (Launch Kit is the source of truth for location). Compile-time, the mirror of
// the device's CUSTOM_HAS_RADAR_HOME override in main.cpp's loadSettings().
#if CUSTOM_HAS_RADAR_HOME
static constexpr double SIM_HOME_LAT = CUSTOM_RADAR_HOME_LAT;
static constexpr double SIM_HOME_LON = CUSTOM_RADAR_HOME_LON;
#else
static constexpr double SIM_HOME_LAT = HOME_LAT_DEFAULT;
static constexpr double SIM_HOME_LON = HOME_LON_DEFAULT;
#endif

static Aircraft mk(const char *call, const char *hex, double distKm, double brgDeg,
                   float altFt, float track, float gsKt, int sq) {
    Aircraft a;
    a.flight = call;
    a.hex = hex;
    const double br = brgDeg * M_PI / 180.0;
    const double latR = SIM_HOME_LAT * M_PI / 180.0;
    a.lat = SIM_HOME_LAT + (distKm * cos(br)) / 111.0;
    a.lon = SIM_HOME_LON + (distKm * sin(br)) / (111.0 * cos(latR));
    a.altBaro = altFt;
    a.onGround = false;
    a.track = track;
    a.gs = gsKt;
    a.squawk = sq;
    return a;
}

static void sim_range_cb(float km) { g_set.rangeKm = km; radar::update(g_mockAcs, g_set); }

// Settings > Range, sim side. Mirrors the device's onRangeChange() minus the NVS write
// and the feed re-query, neither of which the simulator has.
void host_set_range_km(float km) { s_simRangeKm = km; sim_range_cb(km); }

static void mock_init() {
    // This desktop build's stand-in for the microSD card — a plain folder next
    // to the repo, same "/roads/r{lat}_{lon}.bin" tile layout tools/gen_road_tiles.py
    // writes for the real card, so the simulator and the Orb behave identically.
    roads_sd::set_root("sim/sdcard");
    // ORBLAT / ORBLON put the simulator somewhere else for one run:
    //
    //   ORBLAT=27.95 ORBLON=-82.46 .pio/build/native/program --wxshot out
    //
    // Added because the weather radar cannot be checked from a place with no weather. The
    // owner's Orb sits in Phoenix, which in August is reliably clear, so "does the
    // precipitation draw, animate and tell heavy from light" was untestable on the only
    // hardware there is. RainViewer is global and the simulator fetches the same tiles the
    // device does, so pointing it at a storm answers the question honestly.
    g_set.homeLat = SIM_HOME_LAT;
    g_set.homeLon = SIM_HOME_LON;
    if (const char *e = getenv("ORBLAT")) g_set.homeLat = atof(e);
    if (const char *e = getenv("ORBLON")) g_set.homeLon = atof(e);
    if (getenv("ORBLAT") || getenv("ORBLON"))
        printf("[sim] home overridden to %.4f, %.4f\n", g_set.homeLat, g_set.homeLon);
    g_set.rangeKm = RANGE_KM_DEFAULT;
#if CUSTOM_HAS_RADAR_RANGE
    g_set.rangeKm = CUSTOM_RADAR_RANGE_KM;   // a pushed design's own Range slider, matching main.cpp
#endif
#if CUSTOM_HAS_RADAR_MAXAC
    radar::setMaxOnScreen(CUSTOM_RADAR_MAXAC);   // a pushed design's own "max aircraft shown" cap, matching main.cpp
#endif
    g_set.rotationDeg = 0.0;
    g_mockAcs.clear();
    // in-range (< 50 km)
    g_mockAcs.push_back(mk("RESCUE51", "306006",  3.0, 170.0,  1200,  20,  42, 7700));
    g_mockAcs.push_back(mk("IBE3174",  "301001",  5.0,  60.0,  6200, 250, 286, 4655));
    g_mockAcs.push_back(mk("EC-ABC",   "305005",  7.0, 200.0,  2800,  70,  96, 7000));
    g_mockAcs.push_back(mk("VLG28PK",  "303003",  8.0,   0.0, 34000, 180, 448, 1000));
    g_mockAcs.push_back(mk("RYR4521",  "302002", 11.0, 135.0, 11025, 225, 412, 3421));
    // out of range (> 50 km) -> shown as rim arrows pointing their way
    g_mockAcs.push_back(mk("AFR1234",  "401001", 55.0,  30.0, 37000, 210, 470, 1000));
    g_mockAcs.push_back(mk("DLH88X",   "402002", 62.0, 300.0, 39000, 120, 455, 2000));
    g_mockAcs.push_back(mk("BAW777",   "403003", 75.0, 200.0, 41000,  20, 480, 3000));
    g_mockAcs.push_back(mk("UAE9",     "404004", 90.0, 110.0, 38000, 290, 490, 4000));
#if CUSTOM_HAS_RADAR_DEADZONE
    // A pushed design's center dead zone, matching main.cpp: a pixel radius on
    // the glass becomes a km radius against the live range, so it clears the
    // same center artwork whatever the range is. Applied to the mock set itself
    // rather than per frame, so mock_step()'s respawn can't put traffic back
    // inside it. Same flat dLat/dLon distance mock_step already uses.
    if (CUSTOM_RADAR_DEADZONE_PX > 0) {
        const int    dzPx = CUSTOM_RADAR_DEADZONE_PX > RADAR_R_OUTER_PX ? RADAR_R_OUTER_PX : CUSTOM_RADAR_DEADZONE_PX;
        const double dzKm = ((double)dzPx / (double)RADAR_R_OUTER_PX) * g_set.rangeKm;
        const double latR = SIM_HOME_LAT * M_PI / 180.0;
        std::vector<Aircraft> kept;
        for (const Aircraft &a : g_mockAcs) {
            const double dLat = (a.lat - SIM_HOME_LAT) * 111.0;
            const double dLon = (a.lon - SIM_HOME_LON) * 111.0 * cos(latR);
            if (sqrt(dLat * dLat + dLon * dLon) >= dzKm) kept.push_back(a);
        }
        g_mockAcs.swap(kept);
    }
#endif
    g_mockInit = g_mockAcs;
}

static void mock_step(double dt) {
    const double latR = SIM_HOME_LAT * M_PI / 180.0;
    for (size_t i = 0; i < g_mockAcs.size(); ++i) {
        Aircraft &a = g_mockAcs[i];
        const double stepKm = (double)a.gs * 1.852 * (dt / 3600.0);   // kt -> km in dt s
        const double br = a.track * M_PI / 180.0;
        a.lat += (stepKm * cos(br)) / 111.0;
        a.lon += (stepKm * sin(br)) / (111.0 * cos(a.lat * M_PI / 180.0));
        const double dLat = (a.lat - SIM_HOME_LAT) * 111.0;
        const double dLon = (a.lon - SIM_HOME_LON) * 111.0 * cos(latR);
        if (sqrt(dLat * dLat + dLon * dLon) > RANGE_KM_DEFAULT * 1.04) {
            a.lat = g_mockInit[i].lat;     // respawn so the scene stays populated
            a.lon = g_mockInit[i].lon;
        }
    }
}

// Real RainViewer fetch (same pipeline the device uses, see wx_radar_client.cpp) for
// (lat,lon) into the wx_radar frame buffers. zoomTier 0 = 50mi, matching the app's
// default on-enter tier. The device spreads this over several adsb_task cycles to
// dodge its own heap pressure; the desktop build just does it all up front.
static uint32_t g_wxGen = 0;
static void sim_refresh_weather(double lat, double lon) {
    // Per wx_radar_fetch_frame()'s contract: -1 means retry the WHOLE cycle (a fresh
    // generation), not just skip that slot — a single flaky tile shouldn't blank the rest.
    for (int attempt = 0; attempt < 3; ++attempt) {
        ++g_wxGen;
        printf("[sim] fetching live weather radar for %.4f, %.4f (gen %u, attempt %d)...\n",
               lat, lon, g_wxGen, attempt + 1);
        int ok = 0;
        bool retry = false;
        for (int slot = 0; slot < WX_RADAR_FRAMES; ++slot) {
            const int r = wx_radar_fetch_frame(lat, lon, 0, g_wxGen, slot);
            if (r == 1) ++ok;
            else if (r == 0) break;              // fewer frames available than expected — done
            else { retry = true; break; }         // -1: fetch/decode error
        }
        printf("[sim] weather radar: %d/%d frames fetched\n", ok, WX_RADAR_FRAMES);
        if (!retry) break;
    }
}

// Fired when Settings' recents/search list is tapped (host_set_location_named) — moves
// the mock radar's home, and re-fetches live weather for the new spot.
static void sim_apply_home_location(const char *name, double lat, double lon) {
    g_set.homeLat = lat; g_set.homeLon = lon;
    radar::update(g_mockAcs, g_set);
    printf("[sim] location set: %s (%.4f, %.4f)\n", (name && name[0]) ? name : "(unnamed)", lat, lon);
    sim_refresh_weather(lat, lon);
}

// Register the real app lineup for interactive use, in the SAME order as the device
// (main.cpp setup()) so app indices line up: Clock(0), Flight Tracker(1), Weather
// Radar(2), Intel(3), Surveillance(4), Settings(5). Surveillance needs SD card
// hardware this desktop build doesn't have, so it gets a plain placeholder screen
// for now (Phase 3 will bring it in via the host_* stub pattern).
static void sim_register_apps(lv_obj_t *radarScreen) {
    clockview::init();
    settingsview::init();
    lv_obj_t *survScreen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(survScreen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(survScreen, LV_OPA_COVER, 0);

    // Same lineup + same hidden-app subset as the device (custom_apps.h): every app
    // is registered so indices line up, but the ones a theme flash turns off are
    // skipped when the knob cycles the menu.
    app_shell::add(clockview::screen(), theme_style::names().clock, nullptr, nullptr, false, clockview::onEnter, clockview::onExit, !theme_style::apps().clock);   // the clock answers neither a turn nor a press; it does take and give back its canvas
    // Exact same knob state machine as the device (main.cpp) — both wire the
    // shared radar::knob* handlers, so the simulator and Orb behave identically:
    // default view (knob released, a turn opens the switcher), push to enter
    // selection mode (turn cycles aircraft), push again or wait 5s to drop back.
    app_shell::add(radarScreen, theme_style::names().flight,
                   radar::knobPress,                              // onPress
                   radar::knobTurn,                               // onTurn (only while captured)
                   false,                                         // start uncaptured (default view)
                   []() { ui_show_view(0); radar::knobEnter(); }, // onEnter: show scope, then land in default view
                   radar::knobExit,                               // onExit: free style + reset selection
                   !theme_style::apps().flight);
    app_shell::add(radarScreen, theme_style::names().weather,
                   []() { static bool fc = false; fc = !fc; ui_set_weather_forecast(fc); },  // push toggles WX/forecast
                   nullptr, false, []() { wx_map_prepare(g_set.homeLat, g_set.homeLon, 0); ui_show_view(1); }, nullptr, !theme_style::apps().weather);
    app_shell::add(survScreen,  theme_style::names().surveillance, nullptr, nullptr, false, nullptr, nullptr, !theme_style::apps().surveillance);
    // init() FIRST, and this is not a style preference.
    //
    // screen() returns null until init() has built it, and add() quietly rejects a null
    // screen. So this call did nothing at all: the simulator's menu had five apps, News was
    // not among them, and every app after it moved up one — which broke the exact invariant
    // the comment below claims to protect, since selectApp(APP_INTEL) landed on Settings.
    // Nothing failed, nothing logged, and the screen simply could not be reached in the
    // simulator. Found by driving the knob to it and photographing Settings instead.
    intelview::init();
    // Fetch once, synchronously, the way the location app's own sim path does: the device
    // does this from its network task, which the simulator has no equivalent of, and a
    // headless screenshot of an empty screen would tell nobody anything.
    if (intelview::fetchStep()) intelview::onHeadlinesReady();
    app_shell::add(intelview::screen(), theme_style::names().headlines,
                   intelview::onPress, intelview::onTurn, false, intelview::onEnter, intelview::onExit, !theme_style::apps().headlines);
    // The Stock Ticker, between News and Settings, matching main.cpp. Fetched once here for
    // the same reason News is: the device does this from a network task the simulator has
    // no equivalent of, and a headless screenshot of an empty screen tells nobody anything.
    tickerview::init();
    if (ticker_fetch_step()) tickerview::onQuotesReady();
    app_shell::add(tickerview::screen(), theme_style::names().ticker,
                   tickerview::onPress, tickerview::onTurn, false,
                   tickerview::onEnter, tickerview::onExit, !theme_style::apps().ticker);
    // After the Ticker, matching main.cpp. The selftests below address apps by index, so the
    // two lineups have to stay in the same order or the simulator stops standing in for the
    // device at exactly the moment someone is using it to check one.
    app_shell::add(settingsview::screen(), theme_style::names().settings,
                   settingsview::onPress, settingsview::onTurn, true, settingsview::onEnter, settingsview::onExit, false);
    app_shell::begin();   // start on Clock (index 0), matching the device
}

// ---- restart: emulate a hardware reboot (holding the encoder's Select for 8s) -----
// Native has no ESP.restart(); the truest equivalent is a full process re-exec — every
// static resets and the boot splash replays, exactly like power-cycling the real device.
// This is also what a genuine 8s hold fires in the main loop below (knob::takeLongPress()
// was already being polled and discarded there) — the RESTART button just skips the wait.
static int    s_argc = 0;
static char **s_argv = nullptr;

static void sim_restart() {
    printf("[sim] restarting...\n");
    fflush(stdout);
    // lv_refr_now() (called by whoever triggered this, e.g. the theme-restart notice)
    // only updates LVGL's own texture — in composite mode that never reaches the screen
    // until the next present_composite(), which was never going to happen once we exec.
    // Present once more here so whatever notice is on screen right now is actually seen,
    // then hold it briefly — same idea as the device's delay(700) before ESP.restart().
    if (g_composite) { present_composite(SDL_GetTicks()); SDL_Delay(700); }
    SDL_Quit();
    execvp(s_argv[0], s_argv);
    perror("[sim] execvp failed, exiting instead");
    exit(1);
}

// ---- "updating" overlay -------------------------------------------------------------
// Shown while scripts/sim_dev.sh is rebuilding in the background, so the window stays up
// and visibly paused through a rebuild instead of disappearing for the whole compile.
// Polled via a plain stat() on a sentinel file rather than any IPC — sim_dev.sh touches/
// removes /tmp/orb_sim_updating around each `pio run -e native` it runs.
static const char *UPDATING_FLAG = "/tmp/orb_sim_updating";
static lv_obj_t   *s_updatingOverlay = nullptr;

static void build_updating_overlay() {
    s_updatingOverlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_updatingOverlay);
    lv_obj_set_size(s_updatingOverlay, SIM_W, SIM_H);
    lv_obj_center(s_updatingOverlay);
    lv_obj_set_style_bg_color(s_updatingOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_updatingOverlay, LV_OPA_70, 0);
    lv_obj_clear_flag(s_updatingOverlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *lbl = lv_label_create(s_updatingOverlay);
    lv_label_set_text(lbl, "Updating...");
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_center(lbl);
    lv_obj_add_flag(s_updatingOverlay, LV_OBJ_FLAG_HIDDEN);
}

static void poll_updating_overlay(uint32_t now) {
    static uint32_t lastCheck = 0;
    if (now - lastCheck < 250) return;   // 4x/sec is plenty responsive without stat()-ing every frame
    lastCheck = now;
    if (!s_updatingOverlay) return;
    struct stat st;
    if (stat(UPDATING_FLAG, &st) == 0) lv_obj_clear_flag(s_updatingOverlay, LV_OBJ_FLAG_HIDDEN);
    else                               lv_obj_add_flag(s_updatingOverlay, LV_OBJ_FLAG_HIDDEN);
}

int main(int argc, char **argv) {
    s_argc = argc; s_argv = argv;   // kept for sim_restart()'s execvp()
    app_theme::setRestartHook(sim_restart);   // app_theme::set() calls this on native instead of ESP.restart()
    app_theme::init();                        // load the theme saved by a previous sim_restart() (see app_theme.cpp)
    printf("[sim] app theme: %s\n", app_theme::name(app_theme::get()));
    theme_select::setRestartHook(sim_restart);   // theme_select::set() calls this on native instead of ESP.restart()
    theme_select::init();                        // load the slug saved by a previous sim_restart() (see theme_select.cpp)
    printf("[sim] theme slug: %s\n", theme_select::activeSlug()[0] ? theme_select::activeSlug() : "(none)");

    setvbuf(stdout, NULL, _IOLBF, 0);  // line-buffered: logs appear even when piped to a file
    if (getenv("BIGTEXT")) {           // BIGTEXT=1 ./program — preview the large-text mode
        ui_set_large_text(true);
        radar::setLargeText(true);
    }
    const char *shotPath = (argc >= 3 && strcmp(argv[1], "--shot") == 0) ? argv[2] : NULL;
    const char *gifPath  = (argc >= 3 && strcmp(argv[1], "--gif")  == 0) ? argv[2] : NULL;
    // --themeshot captures what the DEVICE actually renders: the real app lineup, the
    // active SD theme, no stock-skin override. The older --shot path deliberately forces
    // THEME_AVIATOR/ORB/MILITARY to document the stock looks, which makes it useless for
    // checking a custom design — it renders roads and an "AVIATOR" label no matter what
    // is on the card. Without this, verifying a theme meant photographing the hardware.
    const char *themeShot = (argc >= 3 && strcmp(argv[1], "--themeshot") == 0) ? argv[2] : NULL;
    // --updateshot captures the system update overlay, which has no other way to be looked
    // at: on hardware it appears only in the seconds before esptool takes the processor, and
    // the whole point of it is to be readable at a glance by someone who is worried. Three
    // labels at fixed offsets is exactly the layout that quietly overlaps when one of them
    // gains a line, and this screen has no second chance to be wrong.
    const char *updateShot = (argc >= 3 && strcmp(argv[1], "--updateshot") == 0) ? argv[2] : NULL;
    // The "Ready" notice, for the same reason: it exists for a few seconds on real
    // hardware after an update and there is no other way to look at it.
    const char *readyShot  = (argc >= 3 && strcmp(argv[1], "--readyshot")  == 0) ? argv[2] : NULL;
    // The bake screen, for the same reason as the two above: on hardware it exists only for
    // the fifteen seconds after picking a theme, and it is three labels at fixed offsets,
    // which is exactly the layout that quietly overlaps when one of them gains a line.
    const char *bakeShot   = (argc >= 3 && strcmp(argv[1], "--bakeshot")   == 0) ? argv[2] : NULL;
    // --wxshot <prefix> opens the weather map and captures it, twice, a moment apart. Two
    // frames because the thing being checked is that the sweep is THERE and MOVING: one
    // picture cannot tell a turning sweep from a stuck one.
    const char *wxShot     = (argc >= 3 && strcmp(argv[1], "--wxshot")     == 0) ? argv[2] : NULL;
    // --newsshot <prefix> drives the News screen the way a person does and captures both
    // halves of it: <prefix>-list.bmp with the knob turned twice (so the selection is on the
    // third headline and the two above it are dimmed) and <prefix>-brief.bmp after a press.
    // Neither state can be photographed any other way — the selection marks fade after six
    // seconds of stillness, and the briefing needs a real answer from the gateway to have
    // anything in it. This makes the same two round trips the Orb makes.
    const char *newsShot   = (argc >= 3 && strcmp(argv[1], "--newsshot")   == 0) ? argv[2] : NULL;
    // --settingsshot <prefix> walks the Settings wheel to the bottom of the list and
    // captures it. The bug this exists for only appears at the far end of a long list, and
    // it appears there because rows more than a quarter turn away were CLAMPED onto the top
    // of the dial instead of being dropped, so five of them drew on the same pixel.
    const char *setShot    = (argc >= 3 && strcmp(argv[1], "--settingsshot") == 0) ? argv[2] : NULL;
    // --newsshot is headless but drives the KNOB, so it needs the full app lineup that only
    // interactive mode registers. It is the one capture that walks the shell rather than
    // putting a single screen up directly.
    const bool  interactive = !shotPath && !gifPath && !updateShot && !readyShot && !bakeShot;
    (void)wxShot;   // live knob/app-shell only outside headless capture
    (void)setShot;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");   // smooth up/downscale (both the
                                                              // frame photo and the live LVGL
                                                              // circle) instead of blocky nearest
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("[sim] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    int reqW = SIM_W, reqH = SIM_H;
    if (interactive) {   // size the window to the (cropped, centered) Orb render + control strip.
        // Cap at 620px tall: the live screen is a fixed 466x466 buffer, and blowing the window
        // up much bigger than that just magnifies it for no gain (and, pre-linear-filter, is what
        // made the clock face look pixelated).
        const float scale = fminf(1.0f, 620.0f / (float)CROP_H);
        g_winW   = (int)lroundf(CROP_W * scale);
        g_frameH = (int)lroundf(CROP_H * scale);
        g_barH   = 92;
        g_winH   = g_frameH + g_barH;
        // Lens position is calibrated against the FULL frame (F_CX/F_CY/F_R); translate
        // into the crop's coordinate space before converting to window pixels.
        const float faCX = (F_CX * FRAME_W - CROP_X) / CROP_W;
        const float faCY = (F_CY * FRAME_H - CROP_Y) / CROP_H;
        const float faR  = (F_R  * FRAME_W)          / CROP_W;
        g_scx = faCX * g_winW;
        g_scy = faCY * g_frameH;
        // Inset the live face to ~74% of the lens opening (calibrated), centred, so a
        // black border of the render's glass shows around it, matching the device's
        // bezel between the glass edge and the active display area.
        g_sr  = faR  * g_winW * 0.7367f;
        reqW = g_winW; reqH = g_winH;
    }
    // Open in the top-left corner (small margin so the title bar clears the macOS
    // menu bar) rather than centred, so it doesn't hide behind the browser.
    s_win = SDL_CreateWindow("The Orb OS (sim)",
                             24, 44,
                             reqW, reqH, SDL_WINDOW_ALLOW_HIGHDPI);
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_win || !s_ren) {
        printf("[sim] window/renderer creation failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(s_ren, reqW, reqH);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, SIM_W, SIM_H);
    if (interactive) setup_chrome();   // load frame + font + buttons; sets g_composite
    printf("[sim] SDL video driver: %s\n", SDL_GetCurrentVideoDriver());

    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[SIM_W * 100];
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SIM_W * 100);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = sdl_flush;
    disp_drv.hor_res  = SIM_W;
    disp_drv.ver_res  = SIM_H;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&indev_drv);

    ui_create();
    lv_obj_t *radarScreen = lv_scr_act();   // captured now, before anything else switches the active screen
    mock_init();
    radar::update(g_mockAcs, g_set);
    ui_on_data_updated();
    WeatherSnapshot forecast = {};
    forecast.valid = true;
    snprintf(forecast.updated, sizeof(forecast.updated), "14:00");
    forecast.code = 1; forecast.tempC = 27; forecast.feelsC = 29;
    forecast.humidity = 61; forecast.windKmh = 18; forecast.windDeg = 85;
    const char *dates[] = {"2026-06-08", "2026-06-09", "2026-06-10", "2026-06-11"};
    const int codes[] = {1, 2, 61, 0};
    const float lows[] = {20, 19, 18, 21}, highs[] = {28, 27, 24, 29};
    const int rain[] = {10, 20, 75, 5};
    forecast.dayCount = 4;
    for (int i = 0; i < 4; ++i) {
        snprintf(forecast.days[i].date, sizeof(forecast.days[i].date), "%s", dates[i]);
        forecast.days[i].code = codes[i]; forecast.days[i].tempMinC = lows[i];
        forecast.days[i].tempMaxC = highs[i]; forecast.days[i].rainChance = rain[i];
    }
    weather_store(forecast);   // still-mock forecast panel (multi-day temps) — not the radar image itself
    wx_radar_begin();
    // g_set, not the compiled constants: ORBLAT/ORBLON override it, and the weather fetch
    // has to follow the home the rest of the simulator is using or the override silently
    // moves the map and not the weather.
    sim_refresh_weather(g_set.homeLat, g_set.homeLon);   // real RainViewer fetch, see above
    // Representative Meteosat-style mock. The native simulator doesn't yet have a native
    // JPEG decode path (TJpg_Decoder pulls in Arduino.h), so unlike the rain radar above,
    // this satellite/cloud view is still a placeholder — populate the shared satellite
    // buffer with a dark Earth field and layered cloud bands that exercise the exact same
    // UI path as hardware.
    // clients, so populate the shared satellite buffer with a dark Earth field
    // and layered cloud bands that exercise the exact same UI path as hardware.
    cloud_image_begin();
    if (uint16_t *sat = cloud_image_back_buffer()) {
        const int centre = WX_RADAR_SIZE / 2;
        for (int y = 0; y < WX_RADAR_SIZE; ++y) for (int x = 0; x < WX_RADAR_SIZE; ++x) {
            const int dx = x - centre, dy = y - centre;
            uint16_t c = 0;
            if (dx * dx + dy * dy < (centre - 2) * (centre - 2)) {
                const float wave1 = y - (116.0f + 0.42f * x + 20.0f * sinf(x * 0.035f));
                const float wave2 = y - (320.0f - 0.52f * x + 14.0f * sinf(x * 0.055f));
                const float cloud = expf(-(wave1 * wave1) / 950.0f) +
                                    0.75f * expf(-(wave2 * wave2) / 620.0f);
                const float texture = 0.72f + 0.28f * sinf(x * 0.19f + y * 0.11f) *
                                                       sinf(x * 0.047f - y * 0.16f);
                const float v = fminf(1.0f, cloud * texture);
                const int r = (int)(8 + 235 * v);
                const int g = (int)(23 + 225 * v);
                const int b = (int)(42 + 205 * v);
                c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
            }
            sat[y * WX_RADAR_SIZE + x] = c;
        }
        cloud_image_commit(1784397900UL, SIM_HOME_LAT, SIM_HOME_LON);
    }
    ui_on_data_updated();
    if (interactive) sim_register_apps(radarScreen);   // live 6-app switcher driven by the virtual knob
#if CUSTOM_BOOT_TARGET == 1
    // Set only by the splash push (the clock push clears it, even if a custom
    // splash is still baked in) — so this is genuinely "you just pushed the
    // splash," not "a custom splash happens to exist." Lands on the About page
    // (same splash art, held indefinitely, push the knob to leave) instead of
    // the normal boot sequence's 2s-hold-then-fade, matching the device build.
    if (interactive) {
        app_shell::selectApp(app_shell::APP_SETTINGS);
        app_shell::setCaptured(true);
        settingsview::openAboutPage();
    }
#elif CUSTOM_BOOT_TARGET == 2
    // Set only by a Flight Tracker push — lands straight on it instead of the
    // clock, matching the device build (see main.cpp). Selecting APP_FLIGHT runs Flight
    // Tracker's onEnter, which captures the knob for aircraft selection when a
    // custom design is active — left captured on purpose, so turning selects an
    // aircraft immediately. Press the knob to leave selection mode and reach
    // the switcher/menu.
    if (interactive) app_shell::selectApp(app_shell::APP_FLIGHT);
#endif
    if (interactive) build_updating_overlay();
    printf("[sim] The Orb OS simulator running (%dx%d) with 6 mock aircraft.\n", SIM_W, SIM_H);
    if (interactive)
        printf("[sim] controls: click Scroll left / Select / Scroll right (or arrows + Enter). T = theme, Esc = quit\n");

    // --themeshot <prefix>: capture every app exactly as the device renders it, with the
    // active SD theme applied and no stock-skin override, then exit. This is what makes
    // workflow rule R3 ("sim before silicon") actually possible — before it, checking a
    // theme change meant flashing hardware and photographing the screen.
    if (interactive && themeShot) {
        // The boot splash holds ~2s then fades over 600ms, and it lives on lv_layer_top
        // so it covers whatever app is selected. Wait it out before capturing anything.
        for (int i = 0; i < 600; ++i) { lv_timer_handler(); SDL_Delay(2); }   // let art decode
        // The boot splash and the app-switcher overlay both live on lv_layer_top, and
        // CUSTOM_BOOT_TARGET==1 parks this build on the About page which holds the splash
        // up indefinitely. Getting the top layer out of the way is what makes the app
        // underneath visible; waiting alone never dismisses it.
        //
        // HIDE, do not clean. lv_obj_clean() deleted the app-switcher overlay along with
        // the splash, and both app_shell and menu_text keep pointers to it — so the
        // switcher capture at the very end of this function built its canvas on a freed
        // parent and segfaulted. Every screenshot had already been written by then, which
        // is exactly why it went unnoticed for so long: the tool did its whole job and
        // then died, leaving nothing behind but a macOS crash dialog per run. Hiding gets
        // the same clear view of the app underneath, and show_overlay() un-hides the
        // overlay itself when openSwitcher() asks for it below.
        for (uint32_t i = 0; i < lv_obj_get_child_cnt(lv_layer_top()); ++i)
            lv_obj_add_flag(lv_obj_get_child(lv_layer_top(), i), LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 200; ++i) { lv_timer_handler(); SDL_Delay(2); }
        int ow, oh; SDL_GetRendererOutputSize(s_ren, &ow, &oh);
        // SIM_SETTLE_MS=7000 holds each app up before its capture. The default 400 ms is
        // enough for onEnter to decode its art, but not enough for anything that MOVES to
        // move: the radar's sweep has turned about five degrees by then, and a hand at five
        // degrees is indistinguishable from a hand at zero, which is exactly the frame that
        // hides a rotation bug. LVGL skips the transform entirely at angle 0.
        const int settleMs = getenv("SIM_SETTLE_MS") ? atoi(getenv("SIM_SETTLE_MS")) : 400;
        for (int idx = 0; idx < app_shell::count(); ++idx) {
            app_shell::selectApp(idx);
            // SIM_SETTINGS_ABOUT=1 pushes into the About sub-page once Settings is up, so a
            // themeshot can check what is otherwise three knob presses deep and never
            // reachable from the top-level app list this loop already walks.
            if (getenv("SIM_SETTINGS_ABOUT") && !strcmp(app_shell::name(), "Settings"))
                settingsview::openAboutPage();
            for (int i = 0; i < settleMs / 2; ++i) { lv_timer_handler(); SDL_Delay(2); }   // let onEnter decode
            lv_refr_now(NULL);
            SDL_RenderClear(s_ren);
            SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
            if (surf) {
                SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                char path[300];
                snprintf(path, sizeof(path), "%s-%d-%s.bmp", themeShot, idx, app_shell::name());
                for (char *c = path; *c; ++c) if (*c == ' ') *c = '_';
                SDL_SaveBMP(surf, path);
                SDL_FreeSurface(surf);
                printf("[sim] themeshot: %s\n", path);
            }
        }
        // ...and the app switcher itself, which no per-app capture ever shows because it
        // is an overlay, not an app. It is also the screen whose typography is hardest to
        // verify any other way: menu fonts are compiled in, so what it renders depends on
        // which theme was pushed last, not on anything the SD card carries.
        app_shell::openSwitcher();
        for (int i = 0; i < 200; ++i) { lv_timer_handler(); SDL_Delay(2); }
        lv_refr_now(NULL);
        SDL_RenderClear(s_ren);
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        if (SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888)) {
            SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
            char path[300];
            snprintf(path, sizeof(path), "%s-menu.bmp", themeShot);
            SDL_SaveBMP(surf, path);
            SDL_FreeSurface(surf);
            printf("[sim] themeshot: %s\n", path);
        }
        SDL_Quit();
        return 0;
    }

    // SIM_SELFTEST=1: headless proof that virtual knob -> input_router -> app_shell
    // cycles apps exactly like the device, then exit. Normal runs skip this entirely.
    if (interactive && getenv("SIM_SELFTEST")) {
        auto pump = [&]() {
            int32_t kd = knob::takeDelta();
            bool pressed = knob::takePress();
            input_router::dispatch((int)kd, pressed);
            lv_timer_handler();
        };
        // Force a known starting state. Boot position is not fixed: CUSTOM_BOOT_TARGET
        // (set by whichever Launch Kit push ran last) can land the device in Settings >
        // About with the knob captured, which silently invalidates every assertion below.
        app_shell::setCaptured(false);
        if (app_shell::browsing()) { simknob::injectPress(true, SDL_GetTicks()); simknob::injectPress(false, SDL_GetTicks()); lv_timer_handler(); }
        app_shell::selectApp(app_shell::APP_CLOCK);
        lv_timer_handler();

        printf("[selftest] roster from theme '%s': clock=%d flight=%d weather=%d surv=%d\n",
               theme_select::activeSlug(), theme_style::apps().clock, theme_style::apps().flight,
               theme_style::apps().weather, theme_style::apps().surveillance);
        {   // Hand geometry now travels per theme too (clock_style.json "hands"), so a
            // theme switch no longer leaves the previous theme's hands on the new face.
            const theme_style::Clock &cs = theme_style::clock();
            printf("[selftest] hands: hour show=%d pivot=%d,%d center=%d,%d blend=%d | second show=%d | orderN=%d\n",
                   cs.hand[0].show, cs.hand[0].pivotX, cs.hand[0].pivotY,
                   cs.hand[0].centerX, cs.hand[0].centerY, cs.hand[0].blend,
                   cs.hand[2].show, cs.orderN);
        }
        printf("[selftest] boot app: %s (idx %d)\n", app_shell::name(), app_shell::index());
        auto press = [&]() { simknob::injectPress(true, SDL_GetTicks()); simknob::injectPress(false, SDL_GetTicks()); pump(); };
        auto rock  = [&]() { simknob::injectTurn(-1); pump(); simknob::injectTurn(+1); pump(); };
        // Everything in this block happens in the space of a few milliseconds, which is not
        // how a knob is used: a leftward turn from one test phase would still be inside the
        // Rock window when the next phase turns right, and read as a gesture nobody made.
        // Waiting past the window between phases is what makes these assertions mean
        // anything about real use.
        auto settle = [&]() { SDL_Delay(420); pump(); };

        // THE ROCK. An ordinary turn belongs to the app on screen; only a quick left-then-
        // right opens the switcher. These two assertions are the whole contract, and they
        // used to say the opposite: a single turn opened the menu, which is what made the
        // knob unusable for anything else.
        settle();
        simknob::injectTurn(+1); pump();
        const bool plainTurnStayed = !app_shell::browsing();
        printf("[selftest] plain turn: browsing=%d (expect 0 = stays in the app)\n", app_shell::browsing());

        settle();
        rock();
        const bool rockOpened = app_shell::browsing();
        printf("[selftest] rock: browsing=%d (expect 1 = switcher opened)\n", app_shell::browsing());
        printf("[selftest] rock opens the menu: %s\n", (plainTurnStayed && rockOpened) ? "PASS" : "FAIL");

        // Right-then-left must NOT open it. Requiring one order is what keeps ordinary
        // direction changes from being read as the gesture.
        if (app_shell::browsing()) press();          // commit out of the switcher first
        settle();
        simknob::injectTurn(+1); pump();
        simknob::injectTurn(-1); pump();
        printf("[selftest] reverse rock: browsing=%d (expect 0 = wrong order, ignored)\n", app_shell::browsing());
        printf("[selftest] rock is directional: %s\n", !app_shell::browsing() ? "PASS" : "FAIL");

        // Browsing: turns cycle apps, a press commits.
        settle();
        rock(); pump();
        const int browseStart = app_shell::index();
        simknob::injectTurn(+1); pump();
        simknob::injectTurn(+1); pump();
        printf("[selftest] browsing turns: %s (idx %d, browsing=%d)\n", app_shell::name(), app_shell::index(), app_shell::browsing());
        press();
        printf("[selftest] press -> committed to %s (idx %d, browsing=%d)\n", app_shell::name(), app_shell::index(), app_shell::browsing());
        printf("[selftest] switcher cycles and commits: %s\n",
               (!app_shell::browsing() && app_shell::index() != browseStart) ? "PASS" : "FAIL");

        // The Flight Tracker takes a plain turn now. Nothing is captured any more: the knob
        // is never taken from the shell, because the Rock is what leaves rather than a press.
        settle();
        app_shell::selectApp(app_shell::APP_FLIGHT); pump();
        printf("[selftest] FT enter: app=%s captured=%d (expect 0)\n", app_shell::name(), app_shell::captured());
        settle();
        simknob::injectTurn(+1); pump();
        printf("[selftest] FT turn: browsing=%d captured=%d (expect 0, 0 = selecting, not browsing)\n",
               app_shell::browsing(), app_shell::captured());
        printf("[selftest] FT turn selects rather than browsing: %s\n",
               (!app_shell::browsing() && !app_shell::captured()) ? "PASS" : "FAIL");

        // Settings > Range, added when touch removal killed the on-screen zoom button.
        // Navigation is made deterministic by the main menu's clamping: turning down
        // past the end parks on the last item (Back), so counting up from there hits a
        // known item regardless of whichever theme's "default selection" we started on.
        // Menu order: Display Location Sound Units Range WiFi Design About Reset Back.
        // Close the switcher overlay first. input_router checks browsing() BEFORE
        // captured(), so leaving the overlay up sends every turn to the app switcher
        // and Settings never sees it. The previous step deliberately left it open.
        if (app_shell::browsing()) press();
        settle();
        app_shell::selectApp(app_shell::APP_SETTINGS); pump();          // Settings; onEnter resets to the menu
        settingsview::onEnter(); pump();
        printf("[selftest] Settings enter: app=%s captured=%d browsing=%d (expect 1, 0)\n",
               app_shell::name(), app_shell::captured(), app_shell::browsing());
        for (int i = 0; i < 15; ++i) { simknob::injectTurn(+1); pump(); }   // clamp on Back
        for (int i = 0; i < 5;  ++i) { simknob::injectTurn(-1); pump(); }   // Back -> Range
        const float before = host_get_range_km();
        press();                                   // open the Range page
        press();                                   // push the value item: cycle one step
        const float after = host_get_range_km();
        printf("[selftest] Settings>Range: %.0f km -> %.0f km (expect a change, steps 10/20/30/50/100)\n",
               (double)before, (double)after);
        printf("[selftest] Settings>Range: %s\n", (before != after) ? "PASS" : "FAIL (range did not move)");

        // Headlines scroll mode (THEME_CAPS 11): same press-to-own-the-knob grammar as
        // the Flight Tracker's selection, but only when the theme's type size actually
        // overflows the dial. With everything fitting, a push stays a refresh and must
        // NOT capture — both behaviours are asserted, whichever this theme exhibits.
        app_shell::setCaptured(false);
        if (app_shell::browsing()) press();
        app_shell::selectApp(app_shell::APP_INTEL); pump();          // Intel (the news screen); onEnter resets to the top
        int iFirst, iVis, iCount;
        intelview::scrollState(iFirst, iVis, iCount);
        const bool iScrollable = iCount > iVis;
        printf("[selftest] Intel enter: items=%d visible=%d first=%d captured=%d (expect captured 0)\n",
               iCount, iVis, iFirst, app_shell::captured());
        press();
        const bool iCap1 = app_shell::captured();
        printf("[selftest] Intel push: captured=%d (expect %d = %s)\n",
               iCap1, iScrollable ? 1 : 0, iScrollable ? "scroll mode" : "refresh, nothing to scroll");
        simknob::injectTurn(+1); pump();
        intelview::scrollState(iFirst, iVis, iCount);
        printf("[selftest] Intel turn: first=%d (expect %d)\n", iFirst, iScrollable ? 1 : 0);
        const bool iTurnOk = iFirst == (iScrollable ? 1 : 0);
        press();
        const bool iCap2 = app_shell::captured();
        printf("[selftest] Intel push again: captured=%d (expect 0)\n", iCap2);
        const bool iOk = iScrollable ? (iCap1 && iTurnOk && !iCap2)
                                     : (!iCap1 && iTurnOk && !iCap2);
        printf("[selftest] Intel scroll: %s\n", iOk ? "PASS" : "FAIL");

        SDL_Quit();
        return 0;
    }

    Uint32 last = SDL_GetTicks();
    Uint32 lastData = last;
    const Uint32 start = last;
    bool run = true;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { run = false; break; }
            if (!interactive) {   // headless capture: keep the old single T shortcut, nothing else
                if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_t) radar::cycleTheme();
                continue;
            }
            switch (e.type) {     // virtual encoder: on-screen buttons (+ keyboard fallback)
                case SDL_MOUSEMOTION:
                    g_mouseX = e.motion.x; g_mouseY = e.motion.y;
                    break;
                case SDL_MOUSEBUTTONDOWN:   // left-click a control button, or tap the screen (touch)
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        for (int i = 0; i < g_btnCount; ++i) {
                            const SDL_Rect &r = g_btns[i].rect;
                            if (e.button.x >= r.x && e.button.x < r.x + r.w &&
                                e.button.y >= r.y && e.button.y < r.y + r.h) {
                                g_btns[i].flashUntil = SDL_GetTicks() + BTN_FLASH_MS;   // visible feedback on every click
                                if (g_btns[i].action == 0)      { simknob::injectPress(true, SDL_GetTicks()); g_selectHeld = true; }
                                else if (g_btns[i].action == 2)  sim_restart();   // single click == the 8s hold's effect
                                else                              simknob::injectTurn(g_btns[i].action);
                                break;
                            }
                        }
                    }
                    break;
                case SDL_MOUSEBUTTONUP:
                    if (e.button.button == SDL_BUTTON_LEFT && g_selectHeld) {
                        simknob::injectPress(false, SDL_GetTicks()); g_selectHeld = false;
                    }
                    break;
                case SDL_KEYDOWN:
                    switch (e.key.keysym.sym) {
                        case SDLK_RIGHT: case SDLK_UP:   simknob::injectTurn(+1); break;
                        case SDLK_LEFT:  case SDLK_DOWN: simknob::injectTurn(-1); break;
                        case SDLK_RETURN: case SDLK_SPACE:
                            if (!e.key.repeat) simknob::injectPress(true, SDL_GetTicks()); break;
                        case SDLK_t: radar::cycleTheme(); break;
                        case SDLK_ESCAPE: run = false; break;
                        default: break;
                    }
                    break;
                case SDL_KEYUP:
                    if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_SPACE)
                        simknob::injectPress(false, SDL_GetTicks());
                    break;
                default: break;
            }
        }
        Uint32 now = SDL_GetTicks();

        // The network step, which this simulator used to run exactly once at boot. That was
        // enough while the only thing it fetched was the headline list, and stopped being
        // enough the moment pressing a headline could ask for something: a brief requested
        // here would sit in the store with nobody to go and get it, and the briefing would
        // read "Getting the story..." for ever while the device did it correctly.
        //
        // Blocking on the UI thread, unlike the device, which does this on core 0. A
        // simulator that stutters for the length of one HTTP request is a fair trade for one
        // that can show what the screen really does; the alternative is a thread this file
        // has no other reason to own.
        //
        // Only when the apps were actually registered. The headless capture modes put a
        // single screen up directly and never build the shell, so there is nothing here for
        // a fetched headline to be drawn into, and no reason to spend the request.
        if (interactive || newsShot) {
            static Uint32 lastNet = 0;
            if (now - lastNet > 400) {
                lastNet = now;
                if (intelview::fetchStep()) intelview::onHeadlinesReady();
            }
        }
        lv_tick_inc(now - last);
        last = now;
        if (interactive) {                       // drive the app shell through the SAME path as the device
            simknob::tick(now);
            if (knob::takeLongPress()) sim_restart();   // real 8s hold -> same as the RESTART button
            int32_t kd = knob::takeDelta();
            bool pressed = knob::takePress();
            input_router::dispatch((int)kd, pressed);
            poll_updating_overlay(now);
        }
        if (now - lastData >= 1000) {       // simulate a 1 Hz ADS-B poll
            lastData = now;
            mock_step(1.0);
            radar::update(g_mockAcs, g_set);
            ui_on_data_updated();
            char clk[8];
            snprintf(clk, sizeof(clk), "14:%02d", (int)((now / 1000) % 60));  // mock clock
            ui_set_status(true, true, -58, clk);   // mock: connected, fresh, strong signal
            ui_set_battery(78, false, true);   // mock battery
            ui_set_date("08 Jun 2026");        // mock date
            settingsview::setNetInfo("Configure at\n" ORB_MDNS_ADDR "\n192.168.1.42");  // mock net info
        }
        // fulfil route lookups with a mock (the sim has no network)
        char wc[12];
        if (route_pending(wc, sizeof(wc))) {
            static const char *cities[] = { "Madrid", "London", "Paris", "Berlin",
                                            "Rome", "Lisbon", "Amsterdam", "Dublin" };
            int h = 0;
            for (const char *p = wc; *p; ++p) h += (unsigned char)*p;
            route_store(wc, cities[h % 8], cities[(h / 2 + 3) % 8]);
        }
        lv_timer_handler();

        if (g_composite) {   // draw the Orb frame + live lens + control buttons
            present_composite(now);
            static bool fshotDone = false;
            const char *fshot = getenv("SIM_FRAMESHOT");   // one composite screenshot, then exit (alignment check)
            if (fshot && !fshotDone && now - start > 2500) {
                fshotDone = true;
                app_shell::selectApp(app_shell::APP_FLIGHT); ui_show_view(0);   // radar: content reaches the screen edge (best alignment check)
                lv_refr_now(NULL); present_composite(now);
                int ow, oh; SDL_GetRendererOutputSize(s_ren, &ow, &oh);
                SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 24, SDL_PIXELFORMAT_RGB24);
                if (surf) {
                    SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_RGB24, surf->pixels, surf->pitch);
                    SDL_SaveBMP(surf, fshot); SDL_FreeSurface(surf);
                    printf("[sim] saved composite %s\n", fshot);
                }
                run = false;
            }
            // one-off: grab whatever's on screen right now (Clock is the boot app, index 0,
            // already showing) — temporary dev tool, same idea as SIM_FRAMESHOT above.
            static bool cshotDone = false;
            const char *cshot = getenv("SIM_CLOCKSHOT");
            // SIM_SHOT_APP picks which app to photograph instead of whatever booted.
            // Anything past the radar is otherwise unreachable in a headless run, which is
            // exactly when a picture of one is wanted. Switched well before the shot rather
            // than in the same pass: selecting an app starts a transition, and a capture
            // taken immediately photographs the screen being left behind.
            static bool shotAppPicked = false;
            if (cshot && !shotAppPicked && now - start > 3000) {
                shotAppPicked = true;
                if (const char *which = getenv("SIM_SHOT_APP")) app_shell::selectApp(atoi(which));
            }
            if (cshot && !cshotDone && now - start > 5000) {
                cshotDone = true;
                lv_refr_now(NULL); present_composite(now);
                int ow, oh; SDL_GetRendererOutputSize(s_ren, &ow, &oh);
                SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 24, SDL_PIXELFORMAT_RGB24);
                if (surf) {
                    SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_RGB24, surf->pixels, surf->pitch);
                    SDL_SaveBMP(surf, cshot); SDL_FreeSurface(surf);
                    printf("[sim] saved clock shot %s\n", cshot);
                }
                run = false;
            }
        }

        // headless screenshot mode: grab the boot splash early (before it fades)
        static bool splashSaved = false;
        if (shotPath && !splashSaved && now - start > 900) {
            splashSaved = true;
            lv_refr_now(NULL);
            SDL_RenderClear(s_ren); SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
            int ow, oh; SDL_GetRendererOutputSize(s_ren, &ow, &oh);
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
            if (surf) {
                SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                char path[300]; snprintf(path, sizeof(path), "%s-splash.bmp", shotPath);
                SDL_SaveBMP(surf, path); SDL_FreeSurface(surf);
                printf("[sim] saved %s\n", path);
            }
        }

        // headless capture of the firmware-update overlay (--updateshot <path>) or the
        // ready notice (--readyshot <path>). Same block: both are the same panel.
        static bool updateSaved = false;
        if ((updateShot || readyShot || bakeShot) && !updateSaved && now - start > 1800) {
            updateSaved = true;
            if (readyShot)     update_ui::ready(true);           // the after-an-update variant
            else if (bakeShot) { update_ui::bake_begin(12); update_ui::bake_progress("clock_plate.png", 3, 12); }
            else               update_ui::firmware_incoming();   // paints and calls lv_refr_now itself
            sim_save_frame(readyShot ? readyShot : bakeShot ? bakeShot : updateShot);
            run = false;
        }

        // --newsshot: turn, turn, shoot; press, wait for the gateway, shoot.
        //
        // Wall-clock stepped rather than frame-counted because the second half genuinely
        // waits on the network: the brief is a real request to the real worker, and a state
        // machine that fires on frame numbers would photograph "Getting the story..." on a
        // slow morning and call it a pass.
        static int wxStep = 0;
        static Uint32 wxAt = 0;
        if (wxShot) {
            if (wxStep == 0 && now - start > 3000) {
                app_shell::selectApp(app_shell::APP_WEATHER);
                wxStep = 1; wxAt = now;
            } else if (wxStep == 1 && now - wxAt > 6000) {
                char path[300]; snprintf(path, sizeof(path), "%s-a.bmp", wxShot);
                sim_save_frame(path);
                wxStep = 2; wxAt = now;
            } else if (wxStep == 2 && now - wxAt > 1200) {
                char path[300]; snprintf(path, sizeof(path), "%s-b.bmp", wxShot);
                sim_save_frame(path);
                run = false;
            }
        }

        static int setStep = 0;
        static Uint32 setAt = 0;
        if (setShot) {
            if (setStep == 0 && now - start > 2500) {
                app_shell::selectApp(app_shell::APP_SETTINGS);
                setStep = 1; setAt = now;
            } else if (setStep == 1 && now - setAt > 1200) {
                // All the way to the bottom of the ten-row list, which is where every row
                // above the last two used to pile up.
                for (int q = 0; q < 12; ++q) input_router::dispatch(1, false);
                setStep = 2; setAt = now;
            } else if (setStep == 2 && now - setAt > 600) {
                char path[300]; snprintf(path, sizeof(path), "%s-bottom.bmp", setShot);
                sim_save_frame(path);
                // Back up three rows from Back to Theme and open it, so the picker gets
                // captured too. Two themes on a card can carry the same display name, and
                // two identical rows with no way to tell them apart is the thing this
                // second shot exists to check.
                for (int q = 0; q < 3; ++q) input_router::dispatch(-1, false);
                setStep = 3; setAt = now;
            } else if (setStep == 3 && now - setAt > 400) {
                input_router::dispatch(0, true);
                setStep = 4; setAt = now;
            } else if (setStep == 4 && now - setAt > 800) {
                char path[300]; snprintf(path, sizeof(path), "%s-themes.bmp", setShot);
                sim_save_frame(path);
                run = false;
            }
        }

        static int newsStep = 0;
        static Uint32 newsAt = 0;
        if (newsShot) {
            if (newsStep == 0 && now - start > 2500) {
                app_shell::selectApp(app_shell::APP_INTEL);
                printf("[newsshot] count=%d idx=%d name=%s\n", app_shell::count(), app_shell::index(), app_shell::name());
                for (int q = 0; q < app_shell::count(); ++q)
                    printf("[newsshot]   %d: %s%s\n", q, app_shell::nameAt(q), app_shell::hiddenAt(q) ? " (hidden)" : "");
                newsStep = 1; newsAt = now;
            } else if (newsStep == 1 && now - newsAt > 3500) {
                // Two detents: the selection lands on the third headline, which is the one
                // arrangement that shows a dimmed row both above and below it.
                input_router::dispatch(1, false);
                input_router::dispatch(1, false);
                newsStep = 2; newsAt = now;
            } else if (newsStep == 2 && now - newsAt > 400) {
                char path[300]; snprintf(path, sizeof(path), "%s-list.bmp", newsShot);
                sim_save_frame(path);
                input_router::dispatch(0, true);      // press: open the briefing
                newsStep = 3; newsAt = now;
            } else if (newsStep == 3 && now - newsAt > 6000) {
                char path[300]; snprintf(path, sizeof(path), "%s-brief.bmp", newsShot);
                sim_save_frame(path);
                run = false;
            }
        }

        // animated GIF capture (--gif <prefix>): grab frames after the splash fades
        static Uint32 lastGif = 0;
        static int gifFrame = 0;
        if (gifPath && now - start > 3000 && now - lastGif >= 55) {
            lastGif = now;
            if (gifFrame >= 60) { run = false; }
            else {
                SDL_RenderClear(s_ren); SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
                int ow, oh; SDL_GetRendererOutputSize(s_ren, &ow, &oh);
                SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
                if (surf) {
                    SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                    char path[300]; snprintf(path, sizeof(path), "%s-%03d.bmp", gifPath, gifFrame);
                    SDL_SaveBMP(surf, path); SDL_FreeSurface(surf);
                }
                gifFrame++;
            }
        }

        // headless screenshot mode (--shot <prefix>): settle, then grab all views/themes
        if (shotPath && now - start > 4000) {
            for (int k = 0; k < 150; ++k) {              // fast-forward to build up the flow map
                mock_step(1.0);
                radar::update(g_mockAcs, g_set);
            }
            radar::select(0);                            // select an aircraft so the card shows
            ui_on_data_updated();
            { char wc[12]; if (route_pending(wc, sizeof(wc))) route_store(wc, "Madrid", "London"); }
            ui_on_data_updated();                        // pick up the mock route for the card
            int ow, oh;
            SDL_GetRendererOutputSize(s_ren, &ow, &oh);
            // View indices are tile ids: 0 = Flight Tracker, 1 = Weather Radar. The old
            // "list" and "stats" shots went with those screens when touch was removed,
            // and Weather moved from tile 3 down to tile 1.
            struct Shot { const char *name; int view; int theme; bool forecast; };
            const Shot shots[5] = {
                { "aviator", 0, THEME_AVIATOR, false },
                { "orb", 0, THEME_ORB, false },
                { "military",0, THEME_MILITARY, false },
                { "weather", 1, THEME_AVIATOR, false },
                { "forecast",1, THEME_AVIATOR, true },
            };
            for (int v = 0; v < (int)(sizeof(shots) / sizeof(shots[0])); ++v) {
                radar::setTheme(shots[v].theme);
                ui_set_weather_forecast(shots[v].forecast);
                ui_show_view(shots[v].view);
                lv_refr_now(NULL);                       // force the view into the buffer
                SDL_RenderClear(s_ren);
                SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
                SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
                if (surf) {
                    SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                    char path[300];
                    snprintf(path, sizeof(path), "%s-%s.bmp", shotPath, shots[v].name);
                    SDL_SaveBMP(surf, path);
                    SDL_FreeSurface(surf);
                    printf("[sim] saved %s\n", path);
                }
            }
            radar::setTheme(THEME_AVIATOR);

            // Clock: whichever face the active theme resolves to. This used to walk all
            // three stock faces by calling the knob-push handler between shots, back when a
            // press cycled them. Pressing does nothing on this screen now — the face is a
            // property of the theme, not something to flip through — so there is one shot.
            clockview::init();
            lv_scr_load(clockview::screen());
            {
                lv_timer_handler();
                lv_refr_now(NULL);
                SDL_RenderClear(s_ren);
                SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
                SDL_Surface *fsurf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
                if (fsurf) {
                    SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, fsurf->pixels, fsurf->pitch);
                    char path[300];
                    snprintf(path, sizeof(path), "%s-clock.bmp", shotPath);
                    SDL_SaveBMP(fsurf, path);
                    SDL_FreeSurface(fsurf);
                    printf("[sim] saved %s\n", path);
                }
            }

            // App shell: register the real six apps (Intel/Surveillance get a plain
            // placeholder screen here, they need WiFi/SD not present in this desktop
            // build) so the switcher overlay and Settings render with the real lineup.
            lv_obj_t *unavailScreen1 = lv_obj_create(NULL);
            lv_obj_set_style_bg_color(unavailScreen1, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(unavailScreen1, LV_OPA_COVER, 0);
            lv_obj_t *unavailScreen2 = lv_obj_create(NULL);
            lv_obj_set_style_bg_color(unavailScreen2, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(unavailScreen2, LV_OPA_COVER, 0);
            settingsview::init();
            app_shell::add(clockview::screen(), "Clock");
            app_shell::add(radarScreen, "Flight Tracker", nullptr, nullptr, false, []() { ui_show_view(0); });
            app_shell::add(radarScreen, "Weather Radar", nullptr, nullptr, false, []() { wx_map_prepare(g_set.homeLat, g_set.homeLon, 0); ui_show_view(1); });
            app_shell::add(unavailScreen1, "News");
            app_shell::add(unavailScreen2, "Surveillance");
            app_shell::add(settingsview::screen(), "Settings", settingsview::onPress, settingsview::onTurn, true, settingsview::onEnter, settingsview::onExit);
            app_shell::begin();

            // Settings: jump straight there, no slide, and grab the base menu list.
            app_shell::selectApp(app_shell::APP_SETTINGS);
            lv_timer_handler();
            lv_refr_now(NULL);
            SDL_RenderClear(s_ren);
            SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
            SDL_Surface *setSurf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
            if (setSurf) {
                SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, setSurf->pixels, setSurf->pitch);
                char path[300]; snprintf(path, sizeof(path), "%s-settings.bmp", shotPath);
                SDL_SaveBMP(setSurf, path); SDL_FreeSurface(setSurf);
                printf("[sim] saved %s\n", path);
            }

            // Menu (app-switcher) overlay: land on Flight Tracker so both neighbours
            // (Clock, Weather Radar) show in the strip, then open the switcher.
            app_shell::selectApp(app_shell::APP_FLIGHT);
            app_shell::openSwitcher();
            lv_timer_handler();
            lv_refr_now(NULL);
            SDL_RenderClear(s_ren);
            SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
            SDL_Surface *menuSurf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
            if (menuSurf) {
                SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, menuSurf->pixels, menuSurf->pitch);
                char path[300]; snprintf(path, sizeof(path), "%s-menu.bmp", shotPath);
                SDL_SaveBMP(menuSurf, path); SDL_FreeSurface(menuSurf);
                printf("[sim] saved %s\n", path);
            }

            run = false;
        }
        SDL_Delay(5);
    }

    if (s_frameTex) SDL_DestroyTexture(s_frameTex);
    SDL_DestroyTexture(s_tex);
    SDL_DestroyRenderer(s_ren);
    SDL_DestroyWindow(s_win);
    SDL_Quit();
    return 0;
}
