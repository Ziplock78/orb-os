#pragma once
// Airport markers for the radar scope. Projects the embedded OurAirports list
// (airports_data.h) like the coastline: cull to the scope, great-circle project,
// cache screen markers, draw in the static chrome layer. All airports get a circle
// with a crosshair and an IATA label when available, regardless of airport size.
// Projection is done only on a
// home/range change, never per frame.
#include <lvgl.h>

void airports_project(double homeLat, double homeLon, double rangeKm,
                      float cx, float cy, float rOuterPx);

void airports_draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa);

// Find the nearest recognizable airport (one with an IATA code). Used by the
// weather view for aviation context; entirely offline from the embedded data.
bool airports_nearest_iata(double lat, double lon, float maxKm,
                           char iata[4], float *distKm, float *bearingDeg);
