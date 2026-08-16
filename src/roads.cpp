#include "roads.h"
#include "roads_data.h"
#include "roads_wide_data.h"
#include "coastline.h"   // geo_project_polylines_flat() — the shared projection math

size_t roads_project_flat(double centerLat, double centerLon, double rangeKm,
                          float cx, float cy, float rOuterPx,
                          lv_point_t *outPts, size_t maxPts,
                          uint16_t *outPolyLen, size_t maxPolys) {
    return geo_project_polylines_flat(ROAD_PTS, ROAD_NUM_POLYS, ROAD_POLY_LEN, ROAD_SCALE,
                                      centerLat, centerLon, rangeKm, cx, cy, rOuterPx,
                                      outPts, maxPts, outPolyLen, maxPolys);
}

size_t roads_wide_project_flat(double centerLat, double centerLon, double rangeKm,
                               float cx, float cy, float rOuterPx,
                               lv_point_t *outPts, size_t maxPts,
                               uint16_t *outPolyLen, size_t maxPolys) {
    return geo_project_polylines_flat(ROAD_WIDE_PTS, ROAD_WIDE_NUM_POLYS, ROAD_WIDE_POLY_LEN, ROAD_WIDE_SCALE,
                                      centerLat, centerLon, rangeKm, cx, cy, rOuterPx,
                                      outPts, maxPts, outPolyLen, maxPolys);
}
