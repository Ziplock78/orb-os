#include "photo.h"
#include "detail_cache_policy.h"
#include <cassert>
#include <cstring>
#include <cstdio>
static uint64_t tick = 100;
uint64_t photo_test_now_ms() { return tick; }
static void store(const char *hex, uint16_t color) {
    int w, h; auto *buf = photo_buffer(&w, &h);
    assert(buf && w == 232 && h == 156);
    buf[0].full = color; buf[1].full = color + 1;
    photo_commit(2, 1, hex, "Generic C172 / Commons / test credit");
}
int main() {
    char pending[10], type[12], credit[192]; int w, h;
    lv_color_t display[2];
    photo_request(" abc123 ", "C172");
    assert(photo_pending(pending, sizeof(pending), type, sizeof(type)));
    assert(!strcmp(pending,"ABC123") && !strcmp(type,"C172"));
    store("abc123", 11); store("DEF456", 22);
    photo_request("ABC123", "C172");
    assert(!photo_pending(pending, sizeof(pending)));
    assert(photo_copy("abc123", display, 2, &w, &h, credit, sizeof(credit)));
    assert(w==2 && h==1 && display[0].full==11 && strstr(credit,"Generic C172"));
    // Network writes and late completion cannot mutate a displayed/cached image.
    store("LATE", 33);
    assert(display[0].full==11 && !photo_pending(pending, sizeof(pending)));
    assert(!photo_copy("ABC123", display, 1, &w, &h, credit, sizeof(credit)));
    tick += DETAIL_CACHE_TTL_MS - 1;
    assert(photo_copy("ABC123", display, 2, &w, &h, credit, sizeof(credit)));
    ++tick;
    assert(!photo_done("ABC123") && photo_pending(pending, sizeof(pending)));
    photo_commit(0,0,"MISS", "");
    photo_request("MISS"); assert(!photo_pending(pending,sizeof(pending)));
    assert(photo_done("MISS") && !photo_copy("MISS",display,2,&w,&h,credit,sizeof(credit)));
    tick += DETAIL_CACHE_MISS_TTL_MS;
    assert(photo_pending(pending,sizeof(pending)));
    store("A",1);store("B",2);store("C",3);store("D",4);
    assert(photo_copy("A",display,2,&w,&h,credit,sizeof(credit)));
    store("E",5);
    assert(photo_done("A") && !photo_done("B"));
    // Repeated evictions recycle storage and preserve each committed image.
    for(int i=0;i<20;++i) {
        char hex[10];snprintf(hex,sizeof(hex),"%06X",i+100);
        store(hex,(uint16_t)i);
        assert(photo_copy(hex,display,2,&w,&h,credit,sizeof(credit)) && display[0].full==i);
    }
    puts("photo cache PASS");
}
