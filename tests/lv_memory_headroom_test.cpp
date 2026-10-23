#define ESP_PLATFORM
#define ORB_LV_STATS 0
#include "lv_psram_alloc.h"
#include <assert.h>
#include <stdio.h>

static size_t internalFree=65536;
static unsigned lastCaps=0;
static bool failExternal=false;
size_t heap_caps_get_free_size(unsigned) {return internalFree;}
void *heap_caps_malloc(size_t n,unsigned caps) {
    lastCaps=caps;
    if(failExternal&&(caps&MALLOC_CAP_SPIRAM))return nullptr;
    return malloc(n);
}
void *heap_caps_realloc(void *p,size_t n,unsigned caps) {
    lastCaps=caps;
    if(failExternal&&(caps&MALLOC_CAP_SPIRAM))return nullptr;
    return realloc(p,n);
}
void heap_caps_free(void *p) {free(p);}
int main() {
    void *p=orb_lv_malloc(128);
    assert(p&&(lastCaps&MALLOC_CAP_INTERNAL));
    internalFree=ORB_LV_INTERNAL_RESERVE+64;
    p=orb_lv_realloc(p,128);
    assert(p&&(lastCaps&MALLOC_CAP_SPIRAM));
    orb_lv_free(p);
    internalFree=0; // no unsigned subtraction underflow
    p=orb_lv_malloc(64);
    assert(p&&(lastCaps&MALLOC_CAP_SPIRAM));orb_lv_free(p);
    internalFree=65536;
    p=orb_lv_malloc(8192);
    assert(p&&(lastCaps&MALLOC_CAP_SPIRAM));orb_lv_free(p);
    p=orb_lv_malloc(64);
    assert(p&&(lastCaps&MALLOC_CAP_INTERNAL));orb_lv_free(p);
    failExternal=true;internalFree=ORB_LV_INTERNAL_RESERVE;
    p=orb_lv_malloc(64);
    assert(p&&(lastCaps&MALLOC_CAP_INTERNAL));orb_lv_free(p);
    puts("LVGL internal headroom, migration and fallback tests passed");
}
