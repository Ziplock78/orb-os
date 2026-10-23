#pragma once
// Shared aircraft-photo state (portable). The actual fetch+JPEG decode lives in
// photo_client (device-only); this just holds the decoded RGB565 image + request
// handshake, guarded for cross-thread access (like route.*).
#include <lvgl.h>
#include <stddef.h>

void photo_request(const char *hex, const char *type = nullptr);                 // UI: I want this hex's photo
bool photo_pending(char *hexOut, size_t n, char *typeOut = nullptr, size_t typeN = 0);          // network task: hex to fetch (deduped)
lv_color_t *photo_buffer(int *maxW, int *maxH);      // PSRAM RGB565 buffer to decode into
void photo_commit(int w, int h, const char *hex, const char *credit);  // ready (w=0 => none)
bool photo_get(const char *hex, int *w, int *h, char *credit, size_t cn);  // ready & matches?
bool photo_done(const char *hex);                    // fetch finished for this hex (with or without a photo)

// Copy a completed image under the state lock; UI never displays the decode buffer.
bool photo_copy(const char *hex, lv_color_t *dst, size_t pixels, int *w, int *h, char *credit, size_t cn);
