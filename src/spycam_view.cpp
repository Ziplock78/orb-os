// Spy Cam: looping "security camera" flip-books. There's no hardware video decoder
// on this chip, so each clip is pre-cut (see tools/spycam_export.sh) into baseline
// JPEG frames and played back like an old picture reel: read one frame, decode it
// with TJpgDec (same decoder the aircraft-photo feature uses), draw it, advance.
// Frames live in /spycam_frames/ on the microSD card (see sdcard.cpp for the pins) —
// copy tools/spycam_export.sh's sdcard_stage/spycam_frames/ output onto the card with
// a reader; there's no path from the USB cable to the card. Only runs while this
// screen is on-screen. Knob push cycles between camera feeds (see CAMS[] below).
//
// Reading one frame file off the SD card per draw was the bottleneck (SD's per-file
// open latency, tens to over a hundred ms, on top of decode). Instead, a whole clip
// (well under the ~8MB of free PSRAM) is bulk-read into RAM once, whenever the camera
// changes, and every frame after that decodes straight out of RAM — no filesystem
// access at all during steady-state playback. The "SWITCHBOARD" screen covers that
// one-time bulk read: an aged-parchment map of Europe (reprojecting the same Natural
// Earth coastline data the Radar app's scope uses, just re-centered and re-colored)
// with a pin and a gold city-name label over wherever that camera is themed as. The
// look was concept-art'd with Higgsfield first, but the pin position is real math, not
// AI guesswork — city_marker_pos() projects the camera's actual lat/lon.
//
// This map's own projected coastline (coastline_project_flat(), PSRAM buffers below)
// is deliberately NOT the shared std::vector cache in coastline.cpp/coastline_project()
// that the Radar scope uses, for two reasons: that cache is keyed to Radar's current
// home/range and reprojected only when those change, so overwriting it here would leave
// Radar's own map wrong until its next zoom/pan; and more importantly, a continent-wide
// view like this one projects ~45,000 points (~115KB) — std::vector's default allocator
// pulls from the ~300KB *internal* heap, not PSRAM, and that one call was enough to
// starve the whole device's heap for its entire session (measured, not theoretical).
// Going through coastline_project_flat() into PSRAM buffers fixes that at the root.
// Also lets this file skip #include-ing coastline_data.h directly, which would
// duplicate that ~600KB dataset into flash (its arrays are file-scope `static const`,
// one copy per translation unit).
#include "spycam_view.h"
#include "app_theme.h"
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "diag_log.h"
#include "sdcard.h"
#include "theme_style.h"   // theme_style::apps() — is this app even in the active theme?
#include "app_shell.h"
#include "geo.h"
#include "coastline.h"
#include "knob.h"

namespace {
    struct Cam {
        const char *label; const char *prefix; int frameCount;
        const char *city; double lat; double lon;   // SWITCHBOARD map marker
    };
    // Must match tools/spycam_export.sh runs (cam index -> prefix, and frame count
    // from that script's "wrote N frames" output). All feeds share one fixed fps.
    // City assignments are placeholders pending confirmation of which feed is the camp.
    const Cam CAMS[] = {
        { "CAM 1", "cam1", 90, "VIENNA", 48.2082, 16.3738 },
        { "CAM 2", "cam0", 90, "PARIS",  48.8566,  2.3522 },   // Paris alley, Russian officer
        { "CAM 3", "cam2", 90, "BERLIN", 52.5200, 13.4050 },   // man writing at the desk
        { "CAM 4", "cam3", 90, "LONDON", 51.5074, -0.1278 },   // bomber flying overhead
        { "CAM 5", "cam4", 90, "WARSAW", 52.2297, 21.0122 },
    };
    constexpr int CAM_COUNT = (int)(sizeof(CAMS) / sizeof(CAMS[0]));
    constexpr int SPYCAM_FPS = 6;
    constexpr int MAX_FRAMES = 128;                        // >= every CAMS[] frameCount today
    constexpr size_t CLIP_BUF_BYTES = 5 * 512 * 1024;      // 2.5MB: ~0.4MB over the largest clip (~2.1MB).
                                                           // Trimmed from 3MB to free PSRAM for the Weather
                                                           // Radar animation frames, which share the pool.

    // SWITCHBOARD map: centered on continental Europe, wide enough range to frame
    // Britain/Iberia to western Russia, Scandinavia to the Mediterranean.
    constexpr double MAP_CENTER_LAT = 50.0;
    constexpr double MAP_CENTER_LON = 15.0;
    constexpr double MAP_RANGE_KM   = 2600.0;
    // Measured this view at 44,803 points / 527 polylines — these give real headroom
    // without wasting much PSRAM (a few hundred KB either way is nothing against ~8MB free).
    constexpr size_t MAP_MAX_PTS   = 60000;
    constexpr size_t MAP_MAX_POLYS = 700;

    lv_obj_t   *s_screen    = nullptr;
    lv_obj_t   *s_canvas    = nullptr;
    lv_color_t *s_buf       = nullptr;
    lv_obj_t   *s_msg       = nullptr;   // "no signal" shown if frames aren't flashed yet
    lv_obj_t   *s_camLabel  = nullptr;   // small "CAM n" indicator, bottom corner
    lv_obj_t   *s_mapLayer  = nullptr;   // SWITCHBOARD coastline map, drawn behind s_connLabel
    lv_obj_t   *s_cityLabel = nullptr;   // black city name, positioned next to the reticle each load
    lv_obj_t   *s_connLabel = nullptr;   // "SWITCHBOARD" shown while a clip bulk-loads into RAM
    bool        s_fsOk      = false;
    int         s_camIdx    = 0;
    int         s_frame     = 0;

    // PSRAM-backed, independent of coastline.cpp's shared cache — see header comment.
    // NOT a std::vector: this view projects ~45,000 points, and std::vector's default
    // allocator pulls from the ~300KB internal heap, not PSRAM — that alone starved the
    // whole device's heap for its entire session (measured: -117KB from one call).
    lv_point_t *s_mapPts      = nullptr;
    uint16_t   *s_mapPolyLen  = nullptr;
    size_t      s_mapPolyCount = 0;
    lv_point_t  s_cityMarker  = { 0, 0 };

    // Projects the fixed Europe view once (map center/range never change, unlike the
    // Radar app's zoomable scope) — see coastline_project_flat()'s comment for why this
    // goes through that function, into PSRAM buffers, instead of coastline_project_into().
    void project_map() {
        if (!s_mapPts) s_mapPts = (lv_point_t *)heap_caps_malloc(MAP_MAX_PTS * sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
        if (!s_mapPolyLen) s_mapPolyLen = (uint16_t *)heap_caps_malloc(MAP_MAX_POLYS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!s_mapPts || !s_mapPolyLen) {
            Serial.println("[spycam] PSRAM alloc for map buffers failed");
            return;
        }
        const float cx = SCREEN_W / 2.0f, cy = SCREEN_H / 2.0f, R = RADAR_R_OUTER_PX;
        s_mapPolyCount = coastline_project_flat(MAP_CENTER_LAT, MAP_CENTER_LON, MAP_RANGE_KM, cx, cy, R,
                                                s_mapPts, MAP_MAX_PTS, s_mapPolyLen, MAP_MAX_POLYS);
    }

    // Where on the map a given camera's city marker belongs.
    lv_point_t city_marker_pos(const Cam &cam) {
        const float cx = SCREEN_W / 2.0f, cy = SCREEN_H / 2.0f, R = RADAR_R_OUTER_PX;
        const double dist = geo::haversineKm(MAP_CENTER_LAT, MAP_CENTER_LON, cam.lat, cam.lon);
        const double brg  = geo::bearingDeg(MAP_CENTER_LAT, MAP_CENTER_LON, cam.lat, cam.lon);
        const geo::Point sp = geo::projectToScreen(dist, brg, MAP_RANGE_KM, cx, cy, R);
        return { (lv_coord_t)lroundf(sp.x), (lv_coord_t)lroundf(sp.y) };
    }

    // Aged-parchment art direction (pin + brass lettering) came from Higgsfield concept
    // art the user liked, but that art can't drive real pin placement — an AI-generated
    // map isn't drawn under any real projection, so there's no math tying its pixels to
    // lat/lon. This draws the same look on top of the real projected coastline instead:
    // guaranteed-correct placement, and real rendered text instead of AI-hallucinated
    // map labels.
    void map_draw_cb(lv_event_t *e) {
        lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);

        lv_draw_rect_dsc_t bg;
        lv_draw_rect_dsc_init(&bg);
        bg.bg_color = lv_color_hex(0xD9C9A3);   // aged parchment
        bg.bg_opa = LV_OPA_COVER;
        lv_area_t full = { 0, 0, SCREEN_W - 1, SCREEN_H - 1 };
        lv_draw_rect(d, &bg, &full);

        lv_draw_line_dsc_t ld;
        lv_draw_line_dsc_init(&ld);
        ld.color = lv_color_hex(0x4A3B28);   // sepia ink
        ld.width = 1;
        ld.opa = 170;
        ld.round_start = ld.round_end = 1;
        if (s_mapPts && s_mapPolyLen) {
            size_t idx = 0;
            for (size_t poly = 0; poly < s_mapPolyCount; ++poly) {
                const uint16_t n = s_mapPolyLen[poly];
                for (uint16_t i = 1; i < n; ++i) {
                    lv_point_t a = s_mapPts[idx + i - 1], b = s_mapPts[idx + i];
                    lv_draw_line(d, &ld, &a, &b);
                }
                idx += n;
            }
        }

        // Darkening overlay, on top of the paper + coastline so the pin stays the
        // brightest thing on screen without needing its own color to stand out.
        lv_draw_rect_dsc_t dark;
        lv_draw_rect_dsc_init(&dark);
        dark.bg_color = lv_color_black();
        dark.bg_opa = 110;
        lv_draw_rect(d, &dark, &full);

        // Target reticle: a ring with four crosshair ticks poking through it, plus a
        // center dot, all in one red — a target marking, not a physical pin, so no
        // drop shadow.
        const lv_color_t reticleColor = lv_color_hex(0xB33A2E);
        const lv_coord_t ringR = 8;
        const lv_coord_t tickIn = 4, tickOut = 5;   // tick spans ringR-tickIn .. ringR+tickOut

        lv_draw_arc_dsc_t ring;
        lv_draw_arc_dsc_init(&ring);
        ring.color = reticleColor;
        ring.width = 2;
        ring.opa = LV_OPA_COVER;
        lv_draw_arc(d, &ring, &s_cityMarker, ringR, 0, 360);

        lv_draw_line_dsc_t tick;
        lv_draw_line_dsc_init(&tick);
        tick.color = reticleColor;
        tick.width = 2;
        tick.opa = LV_OPA_COVER;
        lv_point_t t1, t2;
        t1 = { s_cityMarker.x, (lv_coord_t)(s_cityMarker.y - ringR - tickOut) };
        t2 = { s_cityMarker.x, (lv_coord_t)(s_cityMarker.y - ringR + tickIn) };
        lv_draw_line(d, &tick, &t1, &t2);
        t1 = { s_cityMarker.x, (lv_coord_t)(s_cityMarker.y + ringR - tickIn) };
        t2 = { s_cityMarker.x, (lv_coord_t)(s_cityMarker.y + ringR + tickOut) };
        lv_draw_line(d, &tick, &t1, &t2);
        t1 = { (lv_coord_t)(s_cityMarker.x - ringR - tickOut), s_cityMarker.y };
        t2 = { (lv_coord_t)(s_cityMarker.x - ringR + tickIn), s_cityMarker.y };
        lv_draw_line(d, &tick, &t1, &t2);
        t1 = { (lv_coord_t)(s_cityMarker.x + ringR - tickIn), s_cityMarker.y };
        t2 = { (lv_coord_t)(s_cityMarker.x + ringR + tickOut), s_cityMarker.y };
        lv_draw_line(d, &tick, &t1, &t2);

        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.bg_color = reticleColor;
        dot.bg_opa = LV_OPA_COVER;
        dot.radius = LV_RADIUS_CIRCLE;
        lv_area_t dotArea = { (lv_coord_t)(s_cityMarker.x - 2), (lv_coord_t)(s_cityMarker.y - 2),
                              (lv_coord_t)(s_cityMarker.x + 2), (lv_coord_t)(s_cityMarker.y + 2) };
        lv_draw_rect(d, &dot, &dotArea);
    }

    // Whichever camera's frames are currently sitting in s_clipBuf, all of them, decoded
    // straight from RAM during playback — see the header comment for why.
    uint8_t  *s_clipBuf    = nullptr;
    uint32_t  s_frameOff[MAX_FRAMES];
    uint32_t  s_frameLen[MAX_FRAMES];
    int       s_clipCamIdx = -1;   // -1 = nothing loaded yet

    // JPEG decode target for this call (set just before drawJpg, like photo_client.cpp).
    lv_color_t *s_dst  = nullptr;
    int         s_dstW = 0, s_dstH = 0;

    bool jpg_out(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
        for (int j = 0; j < h; ++j) {
            const int yy = y + j;
            if (yy < 0 || yy >= s_dstH) continue;
            for (int i = 0; i < w; ++i) {
                const int xx = x + i;
                if (xx < 0 || xx >= s_dstW) continue;
                s_dst[yy * s_dstW + xx].full = bmp[j * w + i];
            }
        }
        return true;
    }

    // Pure SD->RAM read of one camera's clip: no UI, no dot animation, no knob-cancel.
    // Used by the boot-time preload (below). load_clip() keeps its own inline copy of this
    // read on purpose — it interleaves the SWITCHBOARD dot animation and knob-cancel per
    // frame, which this silent version deliberately omits. Fills s_frameOff/s_frameLen/
    // s_clipCamIdx and returns whether the whole clip made it in.
    bool read_clip_into_ram(int camIdx) {
        if (!s_clipBuf) return false;
        const Cam &cam = CAMS[camIdx];
        const int n = cam.frameCount > MAX_FRAMES ? MAX_FRAMES : cam.frameCount;
        uint32_t offset = 0;
        for (int i = 0; i < n; i++) {
            char path[48];
            snprintf(path, sizeof(path), "/spycam_frames/%s_%03d.jpg", cam.prefix, i);
            File f = SD.open(path, "r");
            if (!f || f.isDirectory()) {
                if (f) f.close();
                diag::log("spycam %s read open fail %d", cam.prefix, i);
                return false;
            }
            const size_t sz = f.size();
            if (offset + sz > CLIP_BUF_BYTES) {
                f.close();
                diag::log("spycam %s read overflow at frame %d", cam.prefix, i);
                return false;
            }
            const size_t got = f.read(s_clipBuf + offset, sz);
            f.close();
            if (got != sz) {
                diag::log("spycam %s read short %d", cam.prefix, i);
                return false;
            }
            s_frameOff[i] = offset;
            s_frameLen[i] = (uint32_t)sz;
            offset += (uint32_t)sz;
            // Yield each frame so the core-0 idle task runs — this loop takes ~9s total,
            // and without a yield it starves IDLE0 and trips the task watchdog (this runs
            // as a background task, never on the LVGL thread, so blocking briefly is fine).
            vTaskDelay(1);
        }
        s_clipCamIdx = camIdx;
        return true;
    }

    // Background task (core 0) that warms Surveillance off the boot critical path, so the
    // Clock (app index 0) can appear right after the splash instead of waiting on this.
    // Two jobs, in order:
    //   1. project_map() — the continental-Europe coastline reprojection (~45k points of
    //      double-precision trig). Run FIRST and unconditionally (even with no SD card), so
    //      the SWITCHBOARD map is ready whenever the user reaches Surveillance. Moving this
    //      off the setup() thread is what removes the old ~11s boot stall (see the file
    //      header + coastline.cpp — this exact call was a documented multi-second blocker).
    //   2. read_clip_into_ram() — CAM 1's ~9s SD bulk read, only if an SD card is present.
    // While s_bgPreloadRunning is true the LVGL thread must NOT touch SD (tick_cb honors
    // this), so there's exactly one reader of the SD/SPI bus at a time — no mutex needed.
    // read_clip_into_ram() sets s_clipCamIdx only on success, before this clears the flag,
    // so the LVGL side never reads a half-filled clip. project_map() only writes spycam's
    // own PSRAM buffers (s_mapPts/s_mapPolyLen/s_mapPolyCount); map_draw_cb() already guards
    // on those, so a Surveillance entry mid-projection just draws the map without coastline
    // for a beat — no crash, no torn read.
    volatile bool s_bgPreloadRunning = false;

    void bg_init_task(void * /*arg*/) {
        const uint32_t tMap0 = millis();
        project_map();
        Serial.printf("[spycam] bg map projected: %ums (%u polylines)\n",
                      (unsigned)(millis() - tMap0), (unsigned)s_mapPolyCount);

        if (s_fsOk && s_clipBuf) {
            const uint32_t t0 = millis();
            if (read_clip_into_ram(0))
                Serial.printf("[spycam] bg preload %s: %ums\n", CAMS[0].prefix, (unsigned)(millis() - t0));
            else
                diag::log("spycam bg preload failed (frames not on SD yet?)");
        } else {
            diag::log("spycam bg preload skipped (no SD card)");
        }
        s_bgPreloadRunning = false;
        vTaskDelete(NULL);
    }

    // The Vienna "SWITCHBOARD" splash: the parchment Europe map + target reticle + city
    // name + "connecting..." banner. Shown both while a clip actually loads (load_clip)
    // and, for a fixed couple of seconds, on entry even when the clip is already preloaded
    // (tick_cb) — so arriving at Surveillance still opens on the splash, not a hard cut.
    void show_switchboard(const Cam &cam) {
        lv_label_set_text(s_connLabel, "S W I T C H B O A R D\nconnecting...");
        s_cityMarker = city_marker_pos(cam);
        lv_label_set_text(s_cityLabel, cam.city);
        // Tuned against London, the worst case (dense coastline right at the pin) —
        // this offset is shared by every city, so fixing London's overlap fixes them all.
        lv_obj_set_pos(s_cityLabel, s_cityMarker.x + 14, s_cityMarker.y + 8);
        lv_obj_clear_flag(s_mapLayer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_cityLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_connLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_mapLayer);
    }

    void hide_switchboard() {
        lv_obj_add_flag(s_connLabel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_mapLayer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_cityLabel, LV_OBJ_FLAG_HIDDEN);
    }

    // Bulk-reads one camera's whole clip into s_clipBuf. Blocking — this is the one
    // moment SD I/O still happens, so the SWITCHBOARD screen covers it. No-op if that
    // camera is already the one resident in RAM (re-entering Spy Cam on the same feed).
    bool load_clip(int camIdx) {
        if (camIdx == s_clipCamIdx) return true;
        if (!s_clipBuf) return false;
        const Cam &cam = CAMS[camIdx];

        show_switchboard(cam);
        lv_refr_now(NULL);   // paint the connecting screen now, before this function blocks
        const uint32_t tLoad0 = millis();

        const int32_t knobStart = knob::rawPosition();   // detect a turn during this blocking load — see knob.h
        const int n = cam.frameCount > MAX_FRAMES ? MAX_FRAMES : cam.frameCount;
        uint32_t offset = 0;
        bool ok = true;
        bool cancelled = false;
        int lastDots = 1;    // "connecting..." above already reads as 1 dot's worth
        for (int i = 0; i < n; i++) {
            // Cycles 1/2/3 dots every 350ms so the load doesn't look frozen. This can't
            // be a normal LVGL animation — lv_timer_handler() never runs while this loop
            // blocks — so it's updated by hand here and force-painted immediately.
            const int dots = (int)(((millis() - tLoad0) / 350) % 3) + 1;
            if (dots != lastDots) {
                lastDots = dots;
                char dotMsg[40];
                snprintf(dotMsg, sizeof(dotMsg), "S W I T C H B O A R D\nconnecting%.*s", dots, "...");
                lv_label_set_text(s_connLabel, dotMsg);
                lv_refr_now(NULL);
            }
            if (knob::rawPosition() != knobStart) {
                // Don't consume the turn here — leave it queued so the normal main-loop
                // dispatch (main.cpp) picks it up right after this returns and opens the
                // app switcher exactly like any other turn, instead of duplicating that
                // routing logic here.
                cancelled = true;
                ok = false;
                break;
            }
            char path[48];
            snprintf(path, sizeof(path), "/spycam_frames/%s_%03d.jpg", cam.prefix, i);
            File f = SD.open(path, "r");
            if (!f || f.isDirectory()) {
                if (f) f.close();
                diag::log("spycam %s preload open fail %d", cam.prefix, i);
                ok = false; break;
            }
            const size_t sz = f.size();
            if (offset + sz > CLIP_BUF_BYTES) {
                f.close();
                diag::log("spycam %s preload overflow at frame %d", cam.prefix, i);
                ok = false; break;
            }
            const size_t got = f.read(s_clipBuf + offset, sz);
            f.close();
            if (got != sz) {
                diag::log("spycam %s preload short read %d", cam.prefix, i);
                ok = false; break;
            }
            s_frameOff[i] = offset;
            s_frameLen[i] = (uint32_t)sz;
            offset += (uint32_t)sz;
        }

        hide_switchboard();
        s_clipCamIdx = ok ? camIdx : -1;
        if (cancelled) {
            diag::log("spycam %s preload cancelled by knob input", cam.prefix);
        } else {
            Serial.printf("[spycam] preload %s: %ums for %d frames (%u bytes)\n",
                          cam.prefix, (unsigned)(millis() - tLoad0), n, (unsigned)offset);
            if (!ok) diag::log("spycam %s preload failed, clip unavailable", cam.prefix);
        }
        return ok;
    }

    bool draw_frame(const Cam &cam, int idx) {
        if (s_clipCamIdx != s_camIdx) return false;   // clip not resident (load_clip hasn't run/failed)
        const uint8_t *buf = s_clipBuf + s_frameOff[idx];
        const size_t   sz  = s_frameLen[idx];

        s_dst = s_buf; s_dstW = SCREEN_W; s_dstH = SCREEN_H;
        TJpgDec.setJpgScale(1);
        TJpgDec.setSwapBytes(false);
        TJpgDec.setCallback(jpg_out);
        const JRESULT jr = TJpgDec.drawJpg(0, 0, (uint8_t *)buf, sz);
        if (jr != JDR_OK) diag::log("spycam %s decode fail frame %d (jr=%d)", cam.prefix, idx, (int)jr);
        return jr == JDR_OK;
    }

    constexpr uint32_t SPLASH_MS = 2000;   // how long the entry switchboard holds when warm
    bool     s_wasActive  = false;         // were we committed on-screen last tick? (entry edge-detect)
    uint32_t s_splashUntil = 0;            // millis() deadline for the warm-path entry splash

    void tick_cb(lv_timer_t * /*t*/) {
        // Leaving the app (or still previewing it in the switcher) disarms the entry edge —
        // the app-switcher's "scroll through names" loads the underlying screen on every
        // turn, not just on commit, so real playback/SD work waits until browsing settles.
        if (lv_scr_act() != s_screen || !s_fsOk) { s_wasActive = false; return; }
        if (app_shell::browsing())               { s_wasActive = false; return; }

        const Cam &cam = CAMS[s_camIdx];
        const bool freshEntry = !s_wasActive;   // first committed tick since we last left
        s_wasActive = true;

        // Background preload of CAM 1 still finishing: hold the switchboard and don't touch
        // SD from this (LVGL) thread — the bg task owns the bus until s_bgPreloadRunning clears.
        if (s_bgPreloadRunning && s_camIdx == 0) {
            if (freshEntry) show_switchboard(cam);
            return;
        }

        if (s_clipCamIdx != s_camIdx) {
            // Cold: clip not resident. load_clip() shows its own switchboard for the whole
            // SD read, so that read *is* the wait — no separate hold afterward.
            if (!load_clip(s_camIdx)) return;
            s_splashUntil = 0;
        } else if (freshEntry) {
            // Warm (preloaded): a hard cut into playback feels wrong, so hold the Vienna
            // switchboard a deliberate SPLASH_MS before the footage starts.
            show_switchboard(cam);
            s_splashUntil = millis() + SPLASH_MS;
        }

        if (millis() < s_splashUntil) return;   // still holding the warm-path entry splash
        hide_switchboard();

        if (draw_frame(cam, s_frame)) {
            lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_canvas);
        }
        s_frame = (s_frame + 1) % cam.frameCount;
    }
}

// Knob push while viewing Spy Cam: switch to the next camera feed.
// Cameras move with the knob now rather than with the button. A turn is the obvious way to
// go through a row of feeds, and it goes both ways, which a press never could.
void spycamview::onTurn(int delta) {
    if (CAM_COUNT <= 1 || delta == 0) return;
    const int dir = delta > 0 ? 1 : -1;
    s_camIdx = ((s_camIdx + dir) % CAM_COUNT + CAM_COUNT) % CAM_COUNT;
    s_frame  = 0;
    lv_label_set_text(s_camLabel, CAMS[s_camIdx].label);
    if (s_fsOk) load_clip(s_camIdx);
    Serial.printf("[spycam] switched to %s\n", CAMS[s_camIdx].label);
}

// Nothing. See the header: the turn does this screen's only job.
void spycamview::onPress() {}

void spycamview::init() {
    s_fsOk = sdcard::mounted();   // sdcard::begin() already ran earlier in setup()
    if (!s_fsOk) Serial.println("[spycam] no SD card — insert one with /spycam_frames/ on it");

    // 2.5 MB is a quarter of all PSRAM on this board, and it was being reserved whether
    // or not the active theme actually includes Surveillance. A theme with the app turned
    // off got no benefit from it and every other screen paid for it: measured free PSRAM
    // with an app loaded was 592 KB, which is why the menu could not allocate its own
    // artwork. Turning an app off in Launch Kit now genuinely frees its memory instead of
    // only hiding it from the knob. theme_select always reboots on a theme change, so
    // this is re-evaluated whenever the roster could have changed.
    if (theme_style::apps().surveillance) {
        s_clipBuf = (uint8_t *)heap_caps_malloc(CLIP_BUF_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_clipBuf) Serial.println("[spycam] PSRAM alloc for clip buffer failed");
    } else {
        Serial.printf("[spycam] app disabled by theme — skipping %u KB clip buffer\n",
                      (unsigned)(CLIP_BUF_BYTES / 1024));
    }

    const bool office = app_theme::get() == APP_THEME_OFFICE;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, office ? app_theme::palette().bg : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    const size_t bufBytes = (size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t);
    s_buf = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf) {
        s_canvas = lv_canvas_create(s_screen);
        lv_canvas_set_buffer(s_canvas, s_buf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_center(s_canvas);
        lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_COVER);
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);   // hidden until the first frame decodes
    } else {
        Serial.println("[spycam] PSRAM alloc for canvas failed");
    }

    s_msg = lv_label_create(s_screen);
    lv_label_set_text(s_msg, "NO SIGNAL\n(no /spycam_frames/ on SD card)");
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_msg, office ? app_theme::palette().soft : lv_color_hex(0x6A7078), 0);
    lv_obj_set_style_text_font(s_msg, &lv_font_montserrat_16, 0);
    lv_obj_center(s_msg);

    // project_map() (the heavy ~45k-point Europe coastline reprojection) is NOT run here
    // anymore — it moved to bg_init_task (core 0) so it can't stall boot before the Clock
    // appears. s_mapLayer below is created now (cheap), but its draw callback guards on the
    // not-yet-projected buffers, so it just draws nothing until the background task fills them.

    s_mapLayer = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_mapLayer);
    lv_obj_set_size(s_mapLayer, SCREEN_W, SCREEN_H);
    lv_obj_center(s_mapLayer);
    lv_obj_clear_flag(s_mapLayer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_mapLayer, map_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_flag(s_mapLayer, LV_OBJ_FLAG_HIDDEN);

    // Squared-off black backdrop + light text, shown only for the brief blocking
    // bulk-load of a clip into RAM. Sits low so it doesn't collide with wherever the
    // pin/city label lands. A plain label directly on the map wasn't legible enough
    // against the parchment/coastline underneath it.
    s_connLabel = lv_label_create(s_screen);
    lv_label_set_text(s_connLabel, "");
    lv_obj_set_style_text_align(s_connLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_connLabel, lv_color_hex(0xD9C9A3), 0);
    lv_obj_set_style_text_font(s_connLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_bg_color(s_connLabel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_connLabel, 220, 0);
    lv_obj_set_style_radius(s_connLabel, 0, 0);
    lv_obj_set_style_pad_hor(s_connLabel, 10, 0);
    lv_obj_set_style_pad_ver(s_connLabel, 6, 0);
    lv_obj_align(s_connLabel, LV_ALIGN_BOTTOM_MID, 0, -46);
    lv_obj_add_flag(s_connLabel, LV_OBJ_FLAG_HIDDEN);

    // Black stenciled city name, positioned next to the target reticle each load.
    s_cityLabel = lv_label_create(s_screen);
    lv_label_set_text(s_cityLabel, "");
    lv_obj_set_style_text_color(s_cityLabel, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_cityLabel, &lv_font_montserrat_16, 0);
    lv_obj_add_flag(s_cityLabel, LV_OBJ_FLAG_HIDDEN);

    s_camLabel = lv_label_create(s_screen);
    lv_label_set_text(s_camLabel, CAMS[0].label);
    lv_obj_set_style_text_color(s_camLabel, office ? app_theme::palette().soft : lv_color_hex(0x6A7078), 0);
    lv_obj_set_style_text_font(s_camLabel, &lv_font_montserrat_14, 0);
    lv_obj_align(s_camLabel, LV_ALIGN_BOTTOM_MID, 0, -18);

    lv_timer_create(tick_cb, 1000 / SPYCAM_FPS, nullptr);

    // Warm Surveillance in the background (core 0): project the SWITCHBOARD map and preload
    // CAM 1's clip, so neither stalls boot or freezes the UI and both are ready by the time
    // the user reaches Surveillance. Launched unconditionally (even with no SD card) because
    // the map projection must still run — see bg_init_task. The flag is set here, before the
    // task exists, so there's no window where the LVGL side could see "not running" and race
    // it (see bg_init_task / tick_cb).
    s_bgPreloadRunning = true;
    xTaskCreatePinnedToCore(bg_init_task, "spycam_bg", 4096, nullptr, 1, nullptr, 0);
}

lv_obj_t *spycamview::screen() { return s_screen; }
