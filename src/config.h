#pragma once
// Capsule Radar — build & user configuration.

#define FW_VERSION "1.4.2"   // shown on the web config page + Stats screen; bump on release
// Edit pins below: replace every -1 with the value from the Waveshare factory demo
// (see docs/HARDWARE.md and docs/SETUP.md). Do NOT guess them.

// ---------- Home location (default: Phoenix, AZ) ----------
// Fallback only. Launch Kit is the source of truth for location: a pushed
// design's Latitude/Longitude (CUSTOM_RADAR_HOME_*) overrides this on both the
// device and the simulator, and the device also stores a runtime override in
// NVS via the captive portal. This default is what a fresh device / an
// un-pushed simulator centers on, kept in sync with Launch Kit's own editor
// default so all three line up out of the box.
#define HOME_LAT_DEFAULT   33.4484
#define HOME_LON_DEFAULT  -112.0740

// ---------- Radar ----------
#define RANGE_KM_DEFAULT    30.0f          // display range (outer ring). Query is wider, see below.
// Feed query radius = display range × MULT, clamped to [MIN, MAX]. Querying a bit wider than
// the display shows off-range traffic as edge arrows. The floor MUST stay small: the old 50 km
// floor made small display ranges still pull a huge aircraft list in busy airspace, which timed
// out the poll (feed permanently amber near big hubs). Fix contributed by @alexzogh (STLWarehouse).
#define ADSB_QUERY_MULT     1.4f
#define ADSB_QUERY_MIN_KM   12.0f
#define ADSB_QUERY_MAX_KM   150.0f
static const float RANGE_STEPS_KM[] = {10.0f, 20.0f, 30.0f, 50.0f, 100.0f};
#define POLL_INTERVAL_MS    2000           // be gentle with the free API (>=1000)
#define POLL_INTERVAL_BATTERY_MS 5000      // slower polling when running on battery
#define MOTION_INTERP       1              // 1 = glyphs glide between polls; 0 = snap to new pos
#define AC_STALE_MS         15000          // keep the last contacts through brief empty feed responses

// ---------- Weather forecast (Open-Meteo, no API key) ----------
#define WEATHER_REFRESH_MS  1800000UL      // 30 minutes; forecast data changes slowly
#define WX_RADAR_REFRESH_MS 300000UL       // RainViewer frames update about every 5 minutes
#define CLOUD_IMAGE_REFRESH_MS 600000UL    // EUMETSAT MTG cloud imagery; cache for 10 minutes

// ---------- Screen (CO5300 AMOLED) ----------
#define SCREEN_W            466
#define SCREEN_H            466
#define SCREEN_CX           233
#define SCREEN_CY           233
#define RADAR_R_OUTER_PX    218            // outer ring radius in pixels
#define LV_COLOR_DEPTH_BITS 16
#define LCD_COL_OFFSET      6              // CO5300 column (x) gap on this panel (esp_lcd set_gap 0x06)
#define LCD_ROW_OFFSET      0              // no row (y) gap
#define LCD_QSPI_HZ         80000000       // CO5300 QSPI clock (vendor uses 40 MHz; 80 = faster, verify no artifacts)
#define BRIGHTNESS_DEFAULT  200            // 0..255, panel brightness via cmd 0x51
#define TZ_STR              "MST7"                        // POSIX TZ default: Arizona (UTC-7, no DST).
                                                          // Overridden at runtime by IP geolocation on
                                                          // auto-locate (host_locate_current) and by the
                                                          // web config; this is just the out-of-box value.
#define BRIGHTNESS_IDLE     25             // dimmed after no touch for IDLE_DIM_MS
#define IDLE_DIM_MS         3600000UL      // default: dim the screen after 1 hour idle (Settings > Display)

// ---------- ADS-B API (free, non-commercial) ----------
// Plain HTTP on purpose, and it is what makes the radar work at all.
//
// A TLS handshake needs two contiguous ~16 KB buffers from internal RAM, and this board
// cannot offer them: the largest free internal block is ~17 KB no matter what else is
// freed. That was measured, not assumed. Routing every LVGL allocation to PSRAM changed
// the number by exactly zero bytes, because it is a property of the memory map rather than
// fragmentation anyone can tidy up. So every HTTPS fetch died with '-32512 SSL memory
// allocation failed', which left the radar with no aircraft, the weather blank, and the
// device rebooting itself every ~27 minutes on the stuck-feed watchdog.
//
// Dropping to HTTP costs almost nothing here, because ADSB_HTTPS_INSECURE was already 1:
// certificates were never verified, so TLS was only hiding public flight data from
// eavesdroppers, never authenticating the source. No credentials are sent. adsb.lol serves
// this endpoint over HTTP; airplanes.live answers 403 to it, so that one stays on HTTPS as
// a fallback for if the memory picture ever changes.
#define ADSB_PRIMARY_HOST   "api.adsb.lol"          // GET /v2/point/{lat}/{lon}/{radius_nm}
#define ADSB_PRIMARY_TLS    0
#define ADSB_FALLBACK_HOST  "api.airplanes.live"    // same readsb format
#define ADSB_FALLBACK_TLS   1
#define ADSB_USER_AGENT     "CapsuleRadar/1.0 (ESP32-S3 hobby; +https://github.com/socquique/capsule-radar)"
#define ADSB_HTTPS_INSECURE 1               // 1 = setInsecure() (hobby). 0 = use pinned root CA.
#define ADSB_MAX_AIRCRAFT   60              // hard cap parsed per poll (protect RAM in busy areas)

// ---------- Debug ----------
#define DEBUG_MEM           0               // 1 = print a [mem] heap/fps line every 5s on serial

// ---------- Pin map ----------
// VERIFIED (ESPHome def, cross-checked against the Waveshare board definition in
// xiaozhi-esp32 and a working Arduino_GFX port for this exact panel):
#define PIN_LCD_CS          12
#define PIN_LCD_RST         39
#define PIN_TP_INT          11
#define PIN_TP_RST          40
#define TP_MIRROR_X         true
#define TP_MIRROR_Y         true

// CONFIRMED — CO5300 QSPI databus (LCD_CS=12, LCD_RST=39 above match too):
#define PIN_LCD_SCLK        38             // QSPI PCLK
#define PIN_LCD_D0          4
#define PIN_LCD_D1          5
#define PIN_LCD_D2          6
#define PIN_LCD_D3          7

// CONFIRMED — shared I2C bus (touch + IMU + RTC + PMIC + audio codec):
#define PIN_I2C_SDA         15
#define PIN_I2C_SCL         14

// CONFIRMED — ES8311 codec over I2S (M4 alert ping). MCLK/DIN/PA included for completeness:
#define PIN_I2S_MCLK        42
#define PIN_I2S_BCLK        9
#define PIN_I2S_LRCLK       45             // a.k.a. WS
#define PIN_I2S_DOUT        8              // ESP32 -> codec (speaker)
#define PIN_I2S_DIN         10             // codec -> ESP32 (mics)
#define PIN_AUDIO_PA        46             // speaker amp enable
#define PIN_BOOT_BUTTON     0              // BOOT button (held on boot = captive portal, later)

// I2C addresses:
#define I2C_ADDR_TOUCH      0x5A           // CST9217 (corrected from vendor driver; was 0x15)
#define I2C_ADDR_IMU        0x6B
#define I2C_ADDR_RTC        0x51
#define I2C_ADDR_PMIC       0x34

// Safety net: should never fire now that pins are filled in. Keeps future edits honest.
#if (PIN_LCD_SCLK < 0) || (PIN_I2C_SDA < 0)
#  error "config.h: QSPI/I2C pins are back to placeholders (-1). Restore the real values."
#endif
