/**
 * Plane Radar 2.0 — LVGL v8.x configuration.
 * Reached via -DLV_CONF_INCLUDE_SIMPLE (LVGL does #include "lv_conf.h").
 * Only the settings we care about are listed; everything else falls back to the
 * library defaults in lv_conf_internal.h. Tuned for the CO5300 466x466 AMOLED.
 */
#if 1 /* Enable content */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16
/* 0: native RGB565 order — paired with gfx->draw16bitRGBBitmap() in the flush_cb.
   If colors look byte-swapped on hardware, set this to 1 and use draw16bitBeRGBBitmap(). */
#define LV_COLOR_16_SWAP 0
#define LV_COLOR_SCREEN_TRANSP 0
#define LV_COLOR_MIX_ROUND_OFS 0
#define LV_COLOR_CHROMA_KEY lv_color_hex(0x00ff00)

/*=========================
   MEMORY SETTINGS
 *=========================*/
/* Object/style pool (internal RAM). Big bitmap draw buffers are allocated
   separately in PSRAM in display.cpp. Bump this if the radar UI grows. */
// LVGL's heap comes from PSRAM, not the 64 KB internal pool it used to own. A theme's
// font is parsed into this heap by lv_font_load(), and one 71 px face is 44 KB, which the
// old pool could not serve — LVGL does not check the failed allocation and panics on the
// null pointer. See include/lv_psram_alloc.h for the full reasoning.
#define LV_MEM_CUSTOM 1

/* Render one object into an image buffer. Used by the radar to etch the map
 * (roads/coastline/airports) ONCE per location instead of re-vectoring 513
 * polylines on every frame — measured at ~25% of the radar's frame budget. */
#define LV_USE_SNAPSHOT 1
#define LV_MEM_CUSTOM_INCLUDE "lv_psram_alloc.h"
#define LV_MEM_CUSTOM_ALLOC   orb_lv_malloc
#define LV_MEM_CUSTOM_FREE    orb_lv_free
#define LV_MEM_CUSTOM_REALLOC orb_lv_realloc
#define LV_MEM_ADR 0
#define LV_MEM_BUF_MAX_NUM 16
#define LV_MEMCPY_MEMSET_STD 0

/*====================
   HAL SETTINGS
 *====================*/
#define LV_DISP_DEF_REFR_PERIOD 16   /* ms; ~60 Hz cap (SPI bandwidth is the real limit) */
#define LV_INDEV_DEF_READ_PERIOD 20  /* ms */

/* On the device, drive LVGL's tick from Arduino's millis() — no separate ticker.
   On the native SDL simulator there is no Arduino.h, so fall back to lv_tick_inc()
   (called from the sim main loop). */
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#  define LV_TICK_CUSTOM 1
#  define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#  define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#else
#  define LV_TICK_CUSTOM 0
#endif

#define LV_DPI_DEF 130

/*=======================
   FEATURE / DRAW CONFIG
 *=======================*/
#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 4
#define LV_DISP_ROT_MAX_BUF (10 * 1024)

/*==================
   LOG (serial)
 *==================*/
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/*==================
   ASSERTS / DEBUG
 *==================*/
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_USE_REFR_DEBUG 0

/*==================
   FONTS
 *==================*/
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1   /* large-text mode: 14 -> 18 */
#define LV_FONT_MONTSERRAT_20 1   /* large-text mode: 16 -> 20 */
/* 22..44: the Headlines screen's size ladder (THEME_CAPS 12). A theme picks a face from
   this set and nothing else — LVGL fonts are compiled glyph bitmaps, so a size that is not
   linked in cannot be drawn at all, and the whole ladder has to exist in the binary for a
   slider to be able to land on it. Roughly 20-60 KB of flash each at these sizes, which is
   what buys headlines readable from across a room. */
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_44 1
#define LV_FONT_MONTSERRAT_48 1   /* big clock face (app shell), and the top of the ladder */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/*==================
   WIDGETS
 *==================*/
/* Core widgets default to enabled in v8. Spinner lives in "extra" — enable it
   explicitly for the M0 hello screen. */
#define LV_USE_ARC 1
#define LV_USE_LABEL 1
#define LV_USE_SPINNER 1
#define LV_USE_LIST 1
#define LV_USE_TILEVIEW 1

#endif /* LV_CONF_H */
#endif /* Enable content */
