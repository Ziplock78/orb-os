#include "roads_sd.h"
#include "map_snapshot.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <iostream>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#endif

// Only rendering is stubbed: exercise the production SD reader and projection.
static unsigned drawn = 0;
extern "C" void lv_draw_line_dsc_init(lv_draw_line_dsc_t *d) { std::memset(d, 0, sizeof(*d)); }
extern "C" void lv_draw_line(lv_draw_ctx_t *, const lv_draw_line_dsc_t *, const lv_point_t *, const lv_point_t *) { ++drawn; }

// Model the observed 651,468-byte allocation failure. Repeated snapshots must
// still work after ALL further allocations are refused, using the same pixels.
static uint32_t snapshotNeeded = 651468;
static unsigned allocations = 0, released = 0, invalidations = 0;
static bool refuseAllocations = false, refuseRender = false;
extern "C" void *lv_mem_alloc(size_t bytes) { ++allocations; return refuseAllocations ? nullptr : std::malloc(bytes); }
extern "C" void lv_mem_free(void *p) { if (p) { ++released; std::free(p); } }
extern "C" void lv_img_cache_invalidate_src(const void *) { ++invalidations; }
extern "C" uint32_t lv_snapshot_buf_size_needed(lv_obj_t *, lv_img_cf_t) { return snapshotNeeded; }
extern "C" lv_res_t lv_snapshot_take_to_buf(lv_obj_t *, lv_img_cf_t cf, lv_img_dsc_t *image, void *pixels, uint32_t capacity) {
    assert(pixels && capacity >= snapshotNeeded);
    if (refuseRender) return LV_RES_INV;
    memset(pixels, 0x5a, snapshotNeeded);
    image->data = static_cast<const uint8_t *>(pixels); image->data_size = snapshotNeeded;
    image->header.w = image->header.h = 466; image->header.cf = cf;
    return LV_RES_OK;
}
static void test_snapshot_reuse() {
    {
        radar::MapSnapshot snapshot;
        assert(snapshot.take(nullptr));
        const void *first = snapshot.image()->data;
        const unsigned before = allocations;
        refuseAllocations = true;
        for (unsigned i = 0; i < 100; ++i) {
            assert(snapshot.take(nullptr) && snapshot.image()->data == first);
            assert(snapshot.image()->data[651467] == 0x5a);
        }
        assert(allocations == before && released == 0);
        snapshotNeeded += 3;
        assert(!snapshot.take(nullptr) && !snapshot.image()); // resize failure returns, never hangs
        snapshotNeeded -= 3;
        assert(snapshot.take(nullptr) && snapshot.image()->data == first);
        refuseAllocations = false; snapshotNeeded += 3;
        assert(snapshot.take(nullptr) && snapshot.image()->data != first && released == 1);
        const unsigned afterResize = allocations;
        refuseRender = true;
        assert(!snapshot.take(nullptr) && !snapshot.image());
        refuseRender = false;
        assert(snapshot.take(nullptr) && allocations == afterResize);
    }
    assert(released == 2 && invalidations >= 100);
    {
        radar::MapSnapshot snapshot;
        refuseAllocations = true;
        assert(!snapshot.take(nullptr) && !snapshot.image()); // initial failure also returns
        refuseAllocations = false;
        assert(snapshot.take(nullptr));
        snapshotNeeded = 0;
        assert(!snapshot.take(nullptr) && !snapshot.image());
    }
    assert(released == 3);
}
static void put16(std::ofstream &f, int v) { f.put((char)(v & 255)); f.put((char)((v >> 8) & 255)); }
static void tile(const std::string &path, int y1, int x1, int y2, int x2, bool empty = false) {
    std::ofstream f(path, std::ios::binary);
    assert(f.good());
    put16(f, empty ? 0 : 1); put16(f, empty ? 0 : 2); put16(f, 0);
    if (!empty) { put16(f, 2); put16(f,y1); put16(f,x1); put16(f,y2); put16(f,x2); }
}
int main(int argc, char **argv) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    test_snapshot_reuse();
    assert(argc == 2);
    const std::string root = argv[1], base = root + "/roads/";
    const char *names[] = {"q160_-447_m.bin", "q160_-447_t.bin", "q161_-447_m.bin", "r40_-112.bin",
                           "d40_-112_m.bin", "d40_-112_t.bin"};
    for (auto name : names) std::remove((base+name).c_str());
    roads_sd::set_root(root.c_str()); roads_sd::init();
    lv_point_t points[200]; uint16_t lengths[100]; bool ok = false;
    auto project = [&]() {
        const size_t count = roads_sd::project_flat(40.125, -111.625, 60, 233, 233, 200,
                                                   points, 200, lengths, 100, ok);
        assert(ok); return count;
    };
    assert(project() == 0);
    tile(base+names[0], 7209,-20093,7236,-20093);
    assert(project() == 1); // first class is visible without the second class
    tile(base+names[1], 7220,-20100,7220,-20080);
    assert(project() == 2);
    tile(base+names[2], 7250,-20093,7260,-20093);
    assert(project() == 3); // adjacent quarter cell joins the same projection
    roads_sd::project(40.125, -111.625, 60, 233, 233, 200);
    roads_sd::draw(nullptr, lv_color_t {}, 150, 2);
    assert(drawn == 3);
    tile(base+names[4], 7209,-20093,7236,-20093);
    assert(project() == 2); // full motorway class supersedes both quarter motorway files
    tile(base+names[5], 7220,-20100,7220,-20080);
    assert(project() == 2); // full trunk class supersedes only the matching class
    tile(base+names[3], 7210,-20090,7230,-20090);
    assert(project() == 1); // old complete degree suppresses overlapping quarters
    tile(base+names[3], 0,0,0,0, true);
    assert(project() == 0); // old valid empty coverage also takes precedence
    { std::ofstream broken(base+names[3], std::ios::binary); put16(broken,1); put16(broken,2); put16(broken,0); }
    assert(project() == 2); // truncated old files must not hide valid class coverage
    std::remove((base+names[4]).c_str()); std::remove((base+names[5]).c_str());
    assert(project() == 3);
    for (auto name : names) std::remove((base+name).c_str());
    std::cout << "Road cache compatibility and snapshot allocation tests passed\n";
}
