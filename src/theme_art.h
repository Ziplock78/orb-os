#pragma once
#include <stdint.h>
#include <stddef.h>

// Pre-baked theme art, resident in flash and drawn without any runtime work.
//
// The problem this solves, measured on a 466x466 plate (see docs/ARCHITECTURE.md):
//     read 342 KB PNG off the SD card   226 ms   (card is 20 MHz SPI, ~1.5 MB/s)
//     unpack the PNG to RGB565          205 ms
//                                       -------
//                                       431 ms, every single time the screen is shown,
//                                       plus 424 KB of PSRAM held while it is up.
//
// Both halves are avoidable. A PNG is a compressed picture; the panel wants raw RGB565.
// Nothing in the pipeline ever did that conversion ahead of time, so the device did it
// over and over. Launch Kit now bakes the exact bytes the panel wants at push time (on a
// Mac, where the work is free) and the device stores them in the `themeart` flash
// partition. The ESP32-S3 memory-maps flash through its cache, so an asset in there is
// just a pointer: no SD read, no decode, no PSRAM copy, no per-show cost at all.
//
// Deliberately NOT a replacement for the SD/PNG path. An asset that was never baked, or
// a theme installed before this existed, falls through to decode_sd_first() unchanged.
// Flash is a cache in front of that path, never a precondition for it.
namespace theme_art {

// Pixel layouts, matching custom_sprite.cpp's line_cb output byte for byte.
enum Format : uint8_t {
    FMT_RGB565       = 0,   // 2 bytes/px, opaque (plates, splash)
    FMT_RGB565_ALPHA = 1,   // 3 bytes/px, RGB565 + 8-bit alpha (overlays, hands)
    // Stored verbatim, not pixels. Fonts arrive as lv_font_conv binaries and are read
    // back through an lv_fs driver (theme_font.cpp), which is what lets a theme carry its
    // own typography instead of having it compiled into the firmware.
    FMT_RAW          = 2,
};

// Map the partition once at boot. Safe to call more than once; later calls are no-ops.
// Returns false when the partition is missing (older layout) or holds no valid index,
// which is not an error: every caller then just uses the SD path.
bool begin();

// Look up one baked asset for the active theme, e.g. "menu_plate.png".
// On success `out` points straight into memory-mapped flash and must NOT be freed.
// The pointer stays valid for the life of the process.
bool lookup(const char *slug, const char *assetName,
            const uint8_t *&out, int &w, int &h, Format &fmt);

// Convenience wrapper for the common "did this asset get baked?" question.
bool has(const char *slug, const char *assetName);

// The form every sprite file actually wants: look the asset up for the currently-active
// theme and confirm it is in the layout the caller is about to hand to LVGL. Returns
// nullptr on any miss, so the caller falls through to its existing SD/PNG path. The
// returned pointer is memory-mapped flash: never free it.
const uint8_t *find_active(const char *assetName, Format wantFmt, int &w, int &h);

// ---- install side (used by the /artput upload route) ----

// Start an install run for one theme. Entries belonging to OTHER themes are kept, so a
// second theme can be baked alongside the first and switching between them is free. Falls
// back to wiping everything when too little contiguous room is left.
bool install_begin(const char *slug, uint32_t manifestFingerprint);
// Append one baked asset. `data` is raw pixels, already in `fmt` layout.
bool install_asset(const char *slug, const char *assetName,
                   int w, int h, Format fmt, const uint8_t *data, size_t len);
// Write the index and re-map. Nothing is visible to lookup() until this succeeds.
bool install_commit();

// Bytes free for further install_asset() calls.
size_t space_free();
// Total usable bytes in the partition (0 when the partition is absent).
size_t space_total();

// Raw bytes of a baked asset, whatever its format. Used for FMT_RAW blobs (fonts),
// where there is nothing to interpret and the caller just wants a pointer and a length.
bool find_blob(const char *slug, const char *assetName, const uint8_t *&data, size_t &len);

// Does this pointer aim into the memory-mapped partition? Release paths use this instead
// of tracking a per-asset "came from flash" flag: a buffer that theme_art owns was never
// allocated, so freeing it would be a wild pointer into flash.
bool owns(const void *p);

// The asset-list fingerprint stored with this theme's bake, or 0 if it is not baked.
// Compared against theme_style::assetsFingerprint() to decide whether a re-bake is due.
// Per theme, not per partition: several themes stay baked at once, and each has to be
// able to go stale on its own.
uint32_t baked_manifest(const char *slug);

// Does the cache already hold anything for this theme?
bool slug_baked(const char *slug);

// One-time conversion for the active theme: read each PNG off the card, unpack it once,
// and store the raw pixels in flash. Runs at boot only when the active theme is not
// already baked, i.e. right after a theme push, inside the reboot the user is already
// waiting through. No-op once done, and a failure anywhere just leaves that asset on the
// SD path. Returns true if anything was baked.
bool bake_active_theme();

} // namespace theme_art
