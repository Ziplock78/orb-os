#include "coastline.h"
#include "coastline_data.h"
#include "geo.h"
#include <vector>
#include <math.h>
#ifdef ARDUINO
#include <Arduino.h>
#endif

// Cached screen-space polylines for the current scope. Rebuilt only when the
// home position or range changes (a handful of times per session), so the per-
// frame render cost is zero — the chrome layer just repaints what's here.
static std::vector<std::vector<lv_point_t>> s_lines;

// Shared by coastline_project() (writes the Radar scope's cache below) and
// coastline_project_into() (lets another translation unit, e.g. spycam_view.cpp's
// SWITCHBOARD map, reproject this same dataset into its own buffer — without
// #include-ing coastline_data.h itself, which would duplicate the ~600KB dataset
// into flash per translation unit, since its arrays are file-scope `static const`).
static void project_coastline(double centerLat, double centerLon, double rangeKm,
                              float cx, float cy, float rOuterPx,
                              std::vector<std::vector<lv_point_t>> &out) {
    out.clear();
    if (rangeKm <= 0) return;

    const double EDGE = 1.08;                       // include a touch past the rim, then clip
    const double rangeDeg  = rangeKm / 111.0;
    const double latMargin = rangeDeg * 1.20;
    const double cosLat    = cos(centerLat * M_PI / 180.0);
    const double lonMargin = latMargin / (cosLat < 0.15 ? 0.15 : cosLat);

    const int16_t *p = COAST_PTS;
    for (int poly = 0; poly < COAST_NUM_POLYS; ++poly) {
        const int n = COAST_POLY_LEN[poly];
        std::vector<lv_point_t> run;
        for (int i = 0; i < n; ++i) {
            const double lat = p[i * 2]     / (double)COAST_SCALE;
            const double lon = p[i * 2 + 1] / (double)COAST_SCALE;
            const double dlon = lon - centerLon;
            // cheap bounding-box reject (no trig) discards ~99% of the planet instantly;
            // the second dlon test wraps the antimeridian so e.g. home near 179E still works.
            const bool out_ = (fabs(lat - centerLat) > latMargin) ||
                              (fabs(dlon) > lonMargin && fabs(fabs(dlon) - 360.0) > lonMargin);
            if (!out_) {
                const double dist = geo::haversineKm(centerLat, centerLon, lat, lon);
                if (dist <= rangeKm * EDGE) {
                    const double brg = geo::bearingDeg(centerLat, centerLon, lat, lon);
                    const double rPx = (dist / rangeKm) * rOuterPx;
                    const double a   = brg * M_PI / 180.0;
                    lv_point_t sp;
                    sp.x = (lv_coord_t)lroundf((float)(cx + rPx * sin(a)));
                    sp.y = (lv_coord_t)lroundf((float)(cy - rPx * cos(a)));
                    run.push_back(sp);
                    continue;
                }
            }
            if (run.size() >= 2) out.push_back(std::move(run));   // flush the in-range run
            run.clear();
        }
        if (run.size() >= 2) out.push_back(std::move(run));
        p += n * 2;
    }
}

void coastline_project(double homeLat, double homeLon, double rangeKm,
                       float cx, float cy, float rOuterPx) {
    project_coastline(homeLat, homeLon, rangeKm, cx, cy, rOuterPx, s_lines);
}

void coastline_project_into(double centerLat, double centerLon, double rangeKm,
                            float cx, float cy, float rOuterPx,
                            std::vector<std::vector<lv_point_t>> &out) {
    project_coastline(centerLat, centerLon, rangeKm, cx, cy, rOuterPx, out);
}

// Heap-safe, and dataset-agnostic: writes points directly into caller-owned buffers
// (allocate them in PSRAM) instead of via std::vector, whose default allocator comes
// from the ~300KB internal heap — fine at the Radar scope's small ranges (a few hundred
// points) but a continent-wide view like the SWITCHBOARD map projects tens of thousands
// of points, ~115KB, which starved the whole device's internal heap for its entire
// session. Single-pass: each accepted point is written straight to outPts[ptCount++]; a
// run that turns out to be only 1 point (never reaches the >=2 threshold) gets rolled
// back by resetting ptCount to where that run started, so it's never actually a
// wasted/dangling write. Takes the source dataset as parameters (rather than hardcoding
// COAST_PTS etc.) so a second dataset — roads.cpp's local highway extract — can reuse
// this exact math instead of a copy-pasted second implementation.
// Clips the segment (x0,y0)-(x1,y1) against a circle of radius r centered at the origin
// (both points already relative to center). Standard quadratic line-circle intersection,
// solved for the line and then clamped to the segment's parameter range [0,1] — that
// clamp is what makes this handle all three cases in one formula: both endpoints inside
// (roots land outside [0,1] on either side, clamping reproduces the original endpoints),
// one endpoint inside/one outside (one root lands in (0,1): the true crossing; clamping
// the other pins it back to the inside endpoint), and both outside with the segment
// passing through as a chord (both roots land inside (0,1)). Returns 2 (two points
// written to out) if any part of the segment is within the circle, 0 otherwise.
static int clip_segment_circle(double x0, double y0, double x1, double y1, double r,
                               double out[2][2]) {
    const double dx = x1 - x0, dy = y1 - y0;
    const double a = dx * dx + dy * dy;
    if (a < 1e-9) return 0;
    const double b = 2.0 * (x0 * dx + y0 * dy);
    const double c = x0 * x0 + y0 * y0 - r * r;
    const double disc = b * b - 4.0 * a * c;
    if (disc < 0.0) return 0;
    const double sq = sqrt(disc);
    double t0 = (-b - sq) / (2.0 * a);
    double t1 = (-b + sq) / (2.0 * a);
    if (t0 > t1) { const double tmp = t0; t0 = t1; t1 = tmp; }
    if (t1 < 0.0 || t0 > 1.0) return 0;
    if (t0 < 0.0) t0 = 0.0;
    if (t1 > 1.0) t1 = 1.0;
    out[0][0] = x0 + t0 * dx; out[0][1] = y0 + t0 * dy;
    out[1][0] = x0 + t1 * dx; out[1][1] = y0 + t1 * dy;
    return 2;
}

// A polyline vertex only survives the OLD version of this function (kept-vertex
// filtering, no clipping) if it happened to fall within range itself — fine at wide
// ranges where any real dataset has plenty of vertices inside, but at a tight zoom (the
// Weather map's 10mi tier) a nearly-straight, Douglas-Peucker-simplified highway can have
// both its surviving vertices outside the circle even though the road passes right
// through the middle of it, silently dropping it (0 polylines at 10mi around a Phoenix
// home location was how this got caught). This version clips every segment against the
// display circle properly (see clip_segment_circle()) instead of filtering vertices, so a
// road is drawn wherever it actually crosses the visible area regardless of how far apart
// its simplified vertices are.
size_t geo_project_polylines_flat(const int16_t *srcPts, int srcNumPolys, const uint16_t *srcPolyLen, int srcScale,
                                  double centerLat, double centerLon, double rangeKm,
                                  float cx, float cy, float rOuterPx,
                                  lv_point_t *outPts, size_t maxPts,
                                  uint16_t *outPolyLen, size_t maxPolys) {
    size_t polyCount = 0, ptCount = 0;
    if (rangeKm <= 0) return 0;
    const double R = rOuterPx, R2 = R * R;

    // Cheap bounding-box reject (no trig) before the real haversine/bearing/clip work
    // below — the world coastline dataset (coastline_data.h, used both for the Radar
    // scope and the Surveillance/SWITCHBOARD map) is tens of thousands of points; without
    // this, computing haversine+bearing for the whole planet on every call turned
    // spycamview::init()'s one-time world projection into a many-second stall at boot
    // (found live: boot hung between "[knob] ready" and "[shell] app 1/6" after this
    // filter was first dropped during the segment-clipping rewrite). Local datasets like
    // the road tiles are already small enough that this reject rarely fires, so it's free there.
    const double rangeDeg  = rangeKm / 111.0;
    const double latMargin = rangeDeg * 1.20;
    const double cosLat    = cos(centerLat * M_PI / 180.0);
    const double lonMargin = latMargin / (cosLat < 0.15 ? 0.15 : cosLat);

    auto emit = [&](double x, double y) {
        if (ptCount >= maxPts) return;
        outPts[ptCount].x = (lv_coord_t)lroundf((float)(cx + x));
        outPts[ptCount].y = (lv_coord_t)lroundf((float)(cy + y));
        ptCount++;
    };
    auto closeRun = [&](size_t runStart) {
        if (ptCount - runStart >= 2 && polyCount < maxPolys) {
            outPolyLen[polyCount++] = (uint16_t)(ptCount - runStart);
        } else {
            ptCount = runStart;
        }
    };

    // Yield periodically so a big dataset (the narrow roads extract is ~43k points now
    // that primary roads are included, and a wide-radius tier lets most of them past the
    // bbox reject above) can't starve the core-0 idle task long enough to trip the
    // ESP-IDF task watchdog — found live as a reboot loop entering Weather Radar at the
    // 50mi tier right after primary roads were added. This function runs on adsb_task
    // (background, not the display), so a few 1-tick yields spread across tens of
    // thousands of points cost nothing visible.
    //
    // YIELD_EVERY counts every point EXAMINED, but the expensive part (haversine+bearing+
    // projection, ~17 software double-precision trig ops each) only runs for points that
    // survive the bbox reject. In a dense metro area a long run of consecutive survivors
    // can pack thousands of full-trig points between yields — at the old value of 2000 that
    // worst-case run measured close enough to the 5s watchdog limit that it started tripping
    // and rebooting (backtrace: adsb_task -> wx_radar_fetch_frame -> the road projection ->
    // sin). 256 keeps the gap between yields comfortably sub-second even in the densest
    // region, at the cost of only a fraction of a second more on the one-time projection.
    size_t yieldCounter = 0;
    constexpr size_t YIELD_EVERY = 256;

    const int16_t *p = srcPts;
    for (int poly = 0; poly < srcNumPolys; ++poly) {
        const int n = srcPolyLen[poly];
        size_t runStart = ptCount;
        bool haveRun = false, havePrev = false, prevInside = false;
        double prevX = 0, prevY = 0;

        for (int i = 0; i < n; ++i) {
            if (++yieldCounter >= YIELD_EVERY) {
                yieldCounter = 0;
#ifdef ARDUINO
                vTaskDelay(1);
#endif
            }
            const double lat = p[i * 2]     / (double)srcScale;
            const double lon = p[i * 2 + 1] / (double)srcScale;
            const double dlon = lon - centerLon;
            const bool farOut = (fabs(lat - centerLat) > latMargin) ||
                                (fabs(dlon) > lonMargin && fabs(fabs(dlon) - 360.0) > lonMargin);
            if (farOut) {
                // No precise coords computed for this point, so it can't anchor a clip on
                // either side — same as the old vertex-filtering code, it just breaks
                // whatever run was open. Real datasets don't have a chord that both enters
                // and exits the local circle within a single farMargin-sized gap, so this
                // doesn't reintroduce the vertex-drop bug the rewrite above fixed.
                if (haveRun) closeRun(runStart);
                haveRun = false; havePrev = false;
                continue;
            }

            const double dist = geo::haversineKm(centerLat, centerLon, lat, lon);
            const double brg  = geo::bearingDeg(centerLat, centerLon, lat, lon);
            const double rPx  = (dist / rangeKm) * R;
            const double a    = brg * M_PI / 180.0;
            const double x = rPx * sin(a), y = -rPx * cos(a);
            const bool inside = (x * x + y * y) <= R2;
            double clip[2][2];

            if (!havePrev) {
                if (inside) { runStart = ptCount; emit(x, y); haveRun = true; }
            } else if (prevInside && inside) {
                emit(x, y);
            } else if (prevInside && !inside) {
                if (clip_segment_circle(prevX, prevY, x, y, R, clip) == 2) emit(clip[1][0], clip[1][1]);
                closeRun(runStart); haveRun = false;
            } else if (!prevInside && inside) {
                runStart = ptCount;
                if (clip_segment_circle(prevX, prevY, x, y, R, clip) == 2) emit(clip[0][0], clip[0][1]);
                emit(x, y);
                haveRun = true;
            } else if (clip_segment_circle(prevX, prevY, x, y, R, clip) == 2) {   // both outside, chord passes through
                runStart = ptCount;
                emit(clip[0][0], clip[0][1]);
                emit(clip[1][0], clip[1][1]);
                closeRun(runStart);
                haveRun = false;
            }
            prevX = x; prevY = y; prevInside = inside; havePrev = true;
        }
        if (haveRun) closeRun(runStart);
        p += n * 2;
    }
    return polyCount;
}

size_t coastline_project_flat(double centerLat, double centerLon, double rangeKm,
                              float cx, float cy, float rOuterPx,
                              lv_point_t *outPts, size_t maxPts,
                              uint16_t *outPolyLen, size_t maxPolys) {
    return geo_project_polylines_flat(COAST_PTS, COAST_NUM_POLYS, COAST_POLY_LEN, COAST_SCALE,
                                      centerLat, centerLon, rangeKm, cx, cy, rOuterPx,
                                      outPts, maxPts, outPolyLen, maxPolys);
}

void coastline_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa, lv_coord_t width) {
    if (s_lines.empty()) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.width = width;
    d.opa   = opa;
    d.round_start = d.round_end = 1;   // smooth the joints on the thicker line
    for (const auto &line : s_lines) {
        for (size_t i = 1; i < line.size(); ++i) {
            lv_point_t a = line[i - 1];
            lv_point_t b = line[i];
            lv_draw_line(ctx, &d, &a, &b);
        }
    }
}
