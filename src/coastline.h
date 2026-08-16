#pragma once
// World coastline background for the radar scope.
// Project the embedded Natural Earth coastline (coastline_data.h) into screen
// polylines for the current scope, then draw them under the aircraft. Projection
// is done once per home/range change (cheap bbox cull + great-circle), never per
// frame; drawing happens inside the static chrome layer's DRAW_MAIN callback.
#include <lvgl.h>
#include <vector>

void coastline_project(double homeLat, double homeLon, double rangeKm,
                       float cx, float cy, float rOuterPx);

void coastline_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa, lv_coord_t width);

// For any other screen that wants this same coastline dataset reprojected around a
// different center (e.g. spycam_view.cpp's SWITCHBOARD map) without touching the
// Radar scope's own cache above, and without #include-ing coastline_data.h itself —
// its arrays are file-scope `static const`, so a second translation unit including
// it directly would duplicate the ~600KB dataset into flash.
void coastline_project_into(double centerLat, double centerLon, double rangeKm,
                            float cx, float cy, float rOuterPx,
                            std::vector<std::vector<lv_point_t>> &out);

// Same dataset, but for continent-scale views where the point count is too large to
// safely put on the ~300KB internal heap via std::vector (a wide view can be tens of
// thousands of points, over 100KB) — allocate outPts/outPolyLen yourself, ideally in
// PSRAM, and this fills them directly. Returns the polyline count actually written;
// points beyond maxPts or polylines beyond maxPolys are silently dropped, so size those
// generously and sanity-check the return value once against real output.
size_t coastline_project_flat(double centerLat, double centerLon, double rangeKm,
                              float cx, float cy, float rOuterPx,
                              lv_point_t *outPts, size_t maxPts,
                              uint16_t *outPolyLen, size_t maxPolys);

// The actual math behind coastline_project_flat(), generalized to take any similarly-
// shaped baked dataset (int16 lat/lon pairs + a polyline-length index + a scale factor)
// instead of hardcoding COAST_PTS/COAST_NUM_POLYS/COAST_POLY_LEN/COAST_SCALE. roads.cpp
// calls this directly with its own local highway extract so that code isn't duplicated
// a second time; add a new dataset the same way rather than copy-pasting this function.
size_t geo_project_polylines_flat(const int16_t *srcPts, int srcNumPolys, const uint16_t *srcPolyLen, int srcScale,
                                  double centerLat, double centerLon, double rangeKm,
                                  float cx, float cy, float rOuterPx,
                                  lv_point_t *outPts, size_t maxPts,
                                  uint16_t *outPolyLen, size_t maxPolys);
