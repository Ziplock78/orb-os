// Aircraft photo via planespotters.net (free, non-commercial, attribution).
//   GET /pub/photos/hex/{icao}  -> JSON with a thumbnail URL + photographer
//   download the JPEG -> decode (TJpgDec) into photo_buffer() as RGB565.
// Device-only. All network is on core 0 (adsb_task); the UI just displays the buffer.
#include "photo_client.h"
#include "photo.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>

// The same identity every other client sends. It used to be a second, separately written
// string, which is how one of them ends up updated and the other does not.
#define PS_UA ORB_USER_AGENT

// JPEG decode target (set just before drawJpg)
static lv_color_t *s_dst = nullptr;
static int s_dstW = 0, s_dstH = 0;

static bool jpg_out(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bmp) {
    for (int j = 0; j < h; ++j) {
        const int yy = y + j;
        if (yy < 0 || yy >= s_dstH) continue;
        for (int i = 0; i < w; ++i) {
            const int xx = x + i;
            if (xx < 0 || xx >= s_dstW) continue;
            s_dst[yy * s_dstW + xx].full = bmp[j * w + i];   // RGB565 -> lv_color_t
        }
    }
    return true;
}

// GET a URL into a freshly heap_caps(PSRAM) buffer. Caller frees with heap_caps_free.
static bool http_get(const char *url, uint8_t **out, size_t *outLen, size_t maxLen) {
    *out = nullptr; *outLen = 0;
    WiFiClientSecure cli;
    cli.setInsecure();
    cli.setHandshakeTimeout(5);
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3000);    // keep short: this runs on the feed task, a slow photo
    http.setTimeout(6000);           // server must not freeze the live aircraft poll for long
    if (!http.begin(cli, url)) { Serial.println("[photo]   http.begin failed"); return false; }
    http.setUserAgent(PS_UA);   // planespotters rejects the default UA; set the canonical one
    const int code = http.GET();
    if (code != 200) { Serial.printf("[photo]   HTTP %d\n", code); http.end(); return false; }

    // HTTPClient decodes chunked responses into a bounded PSRAM sink. Never build
    // an unbounded String or pass a truncated JPEG to the decoder.
    const int len = http.getSize();
    if (len > (int)maxLen) { http.end(); return false; }
    const size_t cap = len > 0 ? (size_t)len : maxLen;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) { http.end(); return false; }
    struct Sink : Stream {
        uint8_t *data; size_t cap, used = 0;
        Sink(uint8_t *p, size_t n) : data(p), cap(n) {}
        size_t write(uint8_t b) override { return write(&b, 1); }
        size_t write(const uint8_t *p, size_t n) override {
            if (n > cap - used) return 0;
            memcpy(data + used, p, n); used += n; return n;
        }
        int available() override { return 0; }
        int read() override { return -1; }
        int peek() override { return -1; }
        void flush() override {}
    } sink(buf, cap);
    const int received = http.writeToStream(&sink);
    const size_t got = sink.used;
    http.end();
    if (received < 0 || !got || (len > 0 && got != (size_t)len)) {
        heap_caps_free(buf); return false;
    }
    *out = buf; *outLen = got;
    return true;
}

// Parse JSON in PSRAM (keep internal RAM free for TLS).
struct PsramAlloc : ArduinoJson::Allocator {
    void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramAlloc s_jsonPsram;

static void strip_html(const char *in, char *out, size_t n) {
    size_t o = 0; bool tag = false;
    for (size_t i = 0; in && in[i] && o + 1 < n; ++i) {
        if (in[i] == '<') { tag = true; continue; }
        if (in[i] == '>') { tag = false; continue; }
        if (tag) continue;
        if (strncmp(in + i, "&amp;", 5) == 0) { out[o++] = '&'; i += 4; continue; }
        if (strncmp(in + i, "&quot;", 6) == 0) { out[o++] = '"'; i += 5; continue; }
        if ((unsigned char)in[i] < 0x80) out[o++] = in[i];
    }
    out[o] = 0;
}

static bool generic_photo_lookup(const char *type, char *imgUrl, size_t un,
                                 char *credit, size_t cn) {
    if (!type || !type[0]) return false;
    char designator[12]; size_t dn = 0;
    for (const char *p = type; *p && dn + 1 < sizeof(designator); ++p) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) designator[dn++] = *p;
        else if (*p >= 'a' && *p <= 'z') designator[dn++] = (char)(*p - 'a' + 'A');
    }
    designator[dn] = 0;
    if (!designator[0]) return false;
    char url[512];
    snprintf(url, sizeof(url),
             "https://commons.wikimedia.org/w/api.php?action=query&generator=search&gsrsearch=%s%%20aircraft%%20filetype%%3Abitmap"
             "&gsrnamespace=6&gsrlimit=3&prop=imageinfo&iiprop=url%%7Cextmetadata%%7Cmime&iiextmetadatafilter=Artist%%7CLicenseShortName&iiurlwidth=400&format=json",
             designator);
    uint8_t *buf = nullptr; size_t len = 0;
    if (!http_get(url, &buf, &len, 16384)) {
        delay(150);
        if (!http_get(url, &buf, &len, 16384)) return false;
    }

    JsonDocument doc(&s_jsonPsram);
    const DeserializationError err = deserializeJson(doc, buf, len);
    heap_caps_free(buf);
    if (err) return false;

    JsonObjectConst pages = doc["query"]["pages"].as<JsonObjectConst>();
    JsonObjectConst best;
    int bestIndex = 1000000;
    for (JsonPairConst kv : pages) {
        JsonObjectConst page = kv.value().as<JsonObjectConst>();
        JsonObjectConst info = page["imageinfo"][0].as<JsonObjectConst>();
        const char *mime = info["mime"] | "";
        const char *thumb = info["thumburl"] | "";
        const int index = page["index"] | bestIndex;
        if (strncmp(mime, "image/", 6) == 0 && thumb[0] && index < bestIndex) {
            best = info; bestIndex = index;
        }
    }
    if (best.isNull()) return false;
    snprintf(imgUrl, un, "%s", (const char *)(best["thumburl"] | ""));
    char artist[80];
    strip_html(best["extmetadata"]["Artist"]["value"] | "Wikimedia Commons",
               artist, sizeof(artist));
    const char *license = best["extmetadata"]["LicenseShortName"]["value"] | "Commons license";
    snprintf(credit, cn, "Generic %s / Wikimedia Commons / %s / %s", designator, artist, license);
    Serial.printf("[photo] %s: using generic image from Wikimedia Commons\n", designator);
    return true;
}

static bool download_photo(const char *imgUrl) {
    // 2) download the JPEG thumbnail.
    // planespotters serves *progressive* JPEGs, which TJpgDec cannot decode. Route the
    // image through the weserv.nl image proxy, which re-encodes to baseline JPEG and
    // resizes to our canvas width — the result is small (~5 KB) and decodable.
    const char *bare = imgUrl;
    if      (strncmp(bare, "https://", 8) == 0) bare += 8;
    else if (strncmp(bare, "http://",  7) == 0) bare += 7;
    int canvasW = 232, canvasH = 156;
    if (!photo_buffer(&canvasW, &canvasH)) { return false; }
    // Encode the entire nested URL, including Commons paths containing spaces or '&'.
    String proxUrl = "https://images.weserv.nl/?url=";
    static const char digits[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)bare; *p; ++p) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~') proxUrl += (char)*p;
        else { proxUrl += '%'; proxUrl += digits[*p >> 4]; proxUrl += digits[*p & 15]; }
    }
    proxUrl += "&w=" + String(canvasW) + "&h=" + String(canvasH) + "&fit=inside&output=jpg";

    uint8_t *img = nullptr; size_t ilen = 0;
    if (!http_get(proxUrl.c_str(), &img, &ilen, 65536)) { return false; }

    // 3) decode into the shared PSRAM buffer, scaled to fit
    int maxW = 0, maxH = 0;
    lv_color_t *dst = photo_buffer(&maxW, &maxH);
    if (!dst) { heap_caps_free(img); return false; }
    uint16_t jw = 0, jh = 0;
    if (TJpgDec.getJpgSize(&jw, &jh, img, ilen) != JDR_OK || jw == 0 || jh == 0) {
        Serial.println("[photo] JPEG header decode failed");
        heap_caps_free(img); return false;
    }
    uint8_t scale = 1;
    while ((jw / scale) > (uint16_t)maxW || (jh / scale) > (uint16_t)maxH) { scale <<= 1; if (scale >= 8) break; }
    s_dstW = (int)(jw / scale); if (s_dstW > maxW) s_dstW = maxW;
    s_dstH = (int)(jh / scale); if (s_dstH > maxH) s_dstH = maxH;
    s_dst = dst;
    for (int i = 0; i < s_dstW * s_dstH; ++i) s_dst[i].full = 0;   // clear

    TJpgDec.setJpgScale(scale);
    TJpgDec.setSwapBytes(false);
    TJpgDec.setCallback(jpg_out);
    const JRESULT jr = TJpgDec.drawJpg(0, 0, img, ilen);
    heap_caps_free(img);

    if (jr != JDR_OK) { return false; }
    return true;
}

bool photo_fetch(const char *hex, const char *type) {
    if (!hex || !hex[0] || WiFi.status() != WL_CONNECTED) { photo_commit(0, 0, hex, ""); return false; }

    // Memory guard: a photo fetch needs a TLS handshake + JPEG decode. If the largest
    // internal block or PSRAM is tight, skip it. TLS now uses the PSRAM allocator.
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 8000 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 100000) {
        Serial.println("[photo] low memory, skipping");
        photo_commit(0, 0, hex, "");
        return false;
    }


    char url[128], imgUrl[1024] = "", credit[192] = "";
    snprintf(url, sizeof(url), "https://api.planespotters.net/pub/photos/hex/%s", hex);
    uint8_t *jbuf = nullptr; size_t jlen = 0;
    if (http_get(url, &jbuf, &jlen, 8192)) {
        JsonDocument filter(&s_jsonPsram);
        filter["photos"][0]["thumbnail_large"]["src"] = true;
        filter["photos"][0]["photographer"] = true;
        JsonDocument doc(&s_jsonPsram);
        const auto err = deserializeJson(doc, jbuf, jlen, DeserializationOption::Filter(filter));
        heap_caps_free(jbuf);
        if (!err) {
            snprintf(imgUrl, sizeof(imgUrl), "%s", (const char *)(doc["photos"][0]["thumbnail_large"]["src"] | ""));
            snprintf(credit, sizeof(credit), "%s", (const char *)(doc["photos"][0]["photographer"] | ""));
        }
    }
    bool ready = imgUrl[0] && download_photo(imgUrl);
    if (!ready) {
        Serial.printf("[photo] %s: trying generic type %s\n", hex, type ? type : "");
        imgUrl[0] = 0;
        ready = generic_photo_lookup(type, imgUrl, sizeof(imgUrl), credit, sizeof(credit)) && download_photo(imgUrl);
    }
    if (!ready) { photo_commit(0, 0, hex, ""); return false; }
    photo_commit(s_dstW, s_dstH, hex, credit);
    Serial.printf("[photo] %s: %dx%d by %s\n", hex, s_dstW, s_dstH, credit);
    return true;
}
