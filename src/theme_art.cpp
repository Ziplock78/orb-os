#include "theme_art.h"
#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>
#include "theme_select.h"   // activeSlug() — find_active()'s implicit first argument

namespace theme_art {
namespace {

// On-flash layout:
//   [0 .. INDEX_BYTES)          header + fixed-size index (one erase sector, rewritten
//                               whole on every commit so a half-written index is never
//                               readable: the magic goes down last, in its own write)
//   [INDEX_BYTES .. )           asset blobs, 4-byte aligned, in install order
constexpr uint32_t MAGIC       = 0x4F524254;   // 'ORBT'
// Bump whenever the baked layout changes. load_index() rejects any other version, which
// makes the cache look empty, which makes the next boot re-bake from the card. That is
// the only safe way to retire bad data: v1 packed blobs tightly and let each erase clip
// the previous blob's tail, leaving white bands baked into the artwork.
constexpr uint32_t VERSION     = 4;
constexpr size_t   INDEX_BYTES = 8192;         // two 4 KB sectors
constexpr size_t   SECTOR      = 4096;

struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t used;      // bytes consumed by blobs, from INDEX_BYTES
    uint32_t manifest;  // theme_style::assetsFingerprint() at bake time; re-bake if it moves
};

struct Entry {              // exactly 64 bytes, so the index size is trivially checkable
    char     slug[20];
    char     name[24];
    uint32_t offset;        // from partition start
    uint32_t len;
    uint16_t w;
    uint16_t h;
    uint8_t  fmt;
    uint8_t  pad[7];
};
static_assert(sizeof(Entry) == 64, "Entry must stay 64 bytes: the index size depends on it");
constexpr size_t MAX_ENTRIES = (INDEX_BYTES - sizeof(Header)) / sizeof(Entry);

const esp_partition_t *s_part   = nullptr;
const void            *s_mapped = nullptr;      // whole partition, memory-mapped
esp_partition_mmap_handle_t s_map = 0;
Header                 s_hdr    = {};
Entry                  s_index[MAX_ENTRIES];
bool                   s_ready  = false;

// Install-run state (RAM only until commit).
bool     s_installing = false;
uint32_t s_insCount   = 0;
uint32_t s_insUsed    = 0;

bool map_partition() {
    if (s_mapped) { esp_partition_munmap(s_map); s_mapped = nullptr; s_map = 0; }
    const esp_err_t e = esp_partition_mmap(s_part, 0, s_part->size,
                                           ESP_PARTITION_MMAP_DATA, &s_mapped, &s_map);
    if (e != ESP_OK) {
        Serial.printf("[theme_art] mmap failed (%d) — falling back to the SD/PNG path\n", (int)e);
        s_mapped = nullptr;
        return false;
    }
    return true;
}

// Read the header + index out of the mapped region. A bad magic just means "nothing
// baked yet", which is a normal first-boot state, not a failure.
bool load_index() {
    if (!s_mapped) return false;
    memcpy(&s_hdr, s_mapped, sizeof(Header));
    if (s_hdr.magic != MAGIC || s_hdr.version != VERSION || s_hdr.count > MAX_ENTRIES) {
        s_hdr.count = 0;
        s_hdr.used  = 0;
        return false;
    }
    memcpy(s_index, (const uint8_t *)s_mapped + sizeof(Header), s_hdr.count * sizeof(Entry));
    return true;
}

} // namespace

bool begin() {
    if (s_ready) return s_hdr.count > 0;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      (esp_partition_subtype_t)0x40, "themeart");
    if (!s_part) {
        Serial.println("[theme_art] no themeart partition (old layout) — SD/PNG path only");
        s_ready = true;
        return false;
    }
    s_ready = true;
    if (!map_partition()) return false;
    const bool ok = load_index();
    Serial.printf("[theme_art] partition %u KB, %u baked asset(s), %u KB free\n",
                  (unsigned)(s_part->size / 1024), (unsigned)s_hdr.count,
                  (unsigned)(space_free() / 1024));
    return ok && s_hdr.count > 0;
}

bool lookup(const char *slug, const char *assetName,
            const uint8_t *&out, int &w, int &h, Format &fmt) {
    if (!s_mapped || !s_hdr.count || !slug || !slug[0] || !assetName) return false;
    for (uint32_t i = 0; i < s_hdr.count; ++i) {
        const Entry &e = s_index[i];
        if (strncmp(e.slug, slug, sizeof(e.slug)) != 0) continue;
        if (strncmp(e.name, assetName, sizeof(e.name)) != 0) continue;
        out = (const uint8_t *)s_mapped + e.offset;   // straight into flash, never freed
        w   = e.w;
        h   = e.h;
        fmt = (Format)e.fmt;
        return true;
    }
    return false;
}

bool has(const char *slug, const char *assetName) {
    const uint8_t *p = nullptr; int w = 0, h = 0; Format f = FMT_RGB565;
    return lookup(slug, assetName, p, w, h, f);
}

bool owns(const void *p) {
    if (!s_mapped || !p) return false;
    const uint8_t *base = (const uint8_t *)s_mapped;
    const uint8_t *q    = (const uint8_t *)p;
    return q >= base && q < base + s_part->size;
}

const uint8_t *find_active(const char *assetName, Format wantFmt, int &w, int &h) {
    const uint8_t *p = nullptr;
    Format got = FMT_RGB565;
    if (!lookup(theme_select::activeSlug(), assetName, p, w, h, got)) return nullptr;
    if (got != wantFmt) return nullptr;   // baked in a layout this caller cannot draw
    return p;
}

bool slug_baked(const char *slug) {
    if (!s_mapped || !s_hdr.count || !slug || !slug[0]) return false;
    for (uint32_t i = 0; i < s_hdr.count; ++i)
        if (strncmp(s_index[i].slug, slug, sizeof(s_index[i].slug)) == 0) return true;
    return false;
}

uint32_t baked_manifest() { return s_hdr.count ? s_hdr.manifest : 0; }

size_t space_total() { return s_part ? (s_part->size - INDEX_BYTES) : 0; }

size_t space_free() {
    if (!s_part) return 0;
    const uint32_t used = s_installing ? s_insUsed : s_hdr.used;
    return space_total() - used;
}

bool install_begin() {
    if (!s_part) return false;
    // Erase the index first. Until commit writes a valid magic back, lookup() sees
    // "nothing baked" and every caller uses the SD path — so an install interrupted by a
    // power cut degrades to slow-but-correct, never to a half-read blob.
    if (esp_partition_erase_range(s_part, 0, INDEX_BYTES) != ESP_OK) return false;
    s_hdr.count = 0;
    s_hdr.used  = 0;
    s_insCount  = 0;
    s_insUsed   = 0;
    s_installing = true;
    return true;
}

bool install_asset(const char *slug, const char *assetName,
                   int w, int h, Format fmt, const uint8_t *data, size_t len) {
    if (!s_installing || !s_part || !data || !len) return false;
    if (s_insCount >= MAX_ENTRIES) {
        Serial.println("[theme_art] index full — remaining assets stay on the SD path");
        return false;
    }
    const uint32_t offset = INDEX_BYTES + s_insUsed;
    // Each blob starts on its own 4 KB erase sector. Flash erases a whole sector at a
    // time, so packing blobs tightly meant this asset's erase wiped the tail of the one
    // before it — and erased flash reads back as 0xFF, which is opaque white. That showed
    // up as a white band along the bottom of most images and as entirely white small
    // assets (a 2 KB hand shares one sector with its neighbour, so it vanished whole).
    // Sector alignment wastes under 4 KB per asset, which is nothing against 3.4 MB.
    const size_t   padded = (len + SECTOR - 1) & ~(SECTOR - 1);
    if (s_insUsed + padded > space_total()) {
        Serial.printf("[theme_art] %s/%s (%u KB) does not fit — staying on the SD path\n",
                      slug, assetName, (unsigned)(len / 1024));
        return false;
    }
    // offset is sector-aligned by construction (INDEX_BYTES and every `padded` are), so
    // this erase covers exactly this blob's sectors and never reaches back into another's.
    if (esp_partition_erase_range(s_part, offset, padded) != ESP_OK) return false;
    if (esp_partition_write(s_part, offset, data, len) != ESP_OK) return false;

    // Read the blob straight back and compare. Silent corruption here is invisible in
    // code and only shows up as wrong pixels on a screen nobody may look at for days,
    // which is exactly how the sector-overlap bug survived its first deploy. A mismatch
    // rejects the asset so it falls back to the SD path rather than displaying garbage.
    {
        uint8_t chunk[512];
        for (size_t off = 0; off < len; off += sizeof(chunk)) {
            const size_t n = (len - off < sizeof(chunk)) ? (len - off) : sizeof(chunk);
            if (esp_partition_read(s_part, offset + off, chunk, n) != ESP_OK ||
                memcmp(chunk, data + off, n) != 0) {
                Serial.printf("[theme_art] %s/%s: VERIFY FAILED at byte %u — staying on the SD path\n",
                              slug, assetName, (unsigned)(off));
                return false;
            }
        }
    }

    Entry &e = s_index[s_insCount];
    memset(&e, 0, sizeof(e));
    strncpy(e.slug, slug,      sizeof(e.slug) - 1);
    strncpy(e.name, assetName, sizeof(e.name) - 1);
    e.offset = offset;
    e.len    = (uint32_t)len;
    e.w      = (uint16_t)w;
    e.h      = (uint16_t)h;
    e.fmt    = (uint8_t)fmt;
    ++s_insCount;
    s_insUsed += padded;
    return true;
}

bool install_commit(uint32_t manifestFingerprint) {
    if (!s_installing || !s_part) return false;
    s_installing = false;

    // Entries first, magic last: a power cut between the two leaves the header blank, so
    // load_index() rejects it and everything falls back to SD rather than reading
    // entries that point at blobs which may not have been written.
    if (esp_partition_write(s_part, sizeof(Header), s_index,
                            s_insCount * sizeof(Entry)) != ESP_OK) return false;
    Header h = { MAGIC, VERSION, s_insCount, s_insUsed, manifestFingerprint };
    if (esp_partition_write(s_part, 0, &h, sizeof(h)) != ESP_OK) return false;

    if (!map_partition()) return false;     // re-map so the new blobs are visible
    const bool ok = load_index();
    Serial.printf("[theme_art] committed %u asset(s), %u KB used, %u KB free\n",
                  (unsigned)s_hdr.count, (unsigned)(s_hdr.used / 1024),
                  (unsigned)(space_free() / 1024));
    return ok;
}

} // namespace theme_art

#else   // ---- desktop simulator: no flash partitions, always use the SD/PNG path ----

namespace theme_art {
bool begin() { return false; }
bool lookup(const char *, const char *, const uint8_t *&, int &, int &, Format &) { return false; }
bool has(const char *, const char *) { return false; }
const uint8_t *find_active(const char *, Format, int &, int &) { return nullptr; }
bool owns(const void *) { return false; }
bool slug_baked(const char *) { return false; }
bool install_begin() { return false; }
bool install_asset(const char *, const char *, int, int, Format, const uint8_t *, size_t) { return false; }
bool install_commit(uint32_t) { return false; }
uint32_t baked_manifest() { return 0; }
size_t space_free()  { return 0; }
size_t space_total() { return 0; }
} // namespace theme_art

#endif
