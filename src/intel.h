#pragma once

#include <stdint.h>
#include <stddef.h>

// Headlines for the Intel screen, shared by the network task and the LVGL UI.
//
// Sized by what the dial can actually show. The worker cuts every headline to 70 characters
// on a word boundary before it is sent, so the buffer only has to hold that plus a NUL and
// the room multi-byte characters take: an ellipsis is three bytes in UTF-8 and a headline
// may end in one, and quotes come back as typographic quotes from some feeds.
#define INTEL_MAX_ITEMS   5
#define INTEL_TEXT_BYTES  96
#define INTEL_SOURCE_BYTES 12

struct IntelItem {
    char text[INTEL_TEXT_BYTES];      // the headline, already cut to fit by the worker
    char source[INTEL_SOURCE_BYTES];  // "BBC", "BBC Sport", "NASA" — shown as a credit
};

struct IntelSnapshot {
    bool valid;
    IntelItem items[INTEL_MAX_ITEMS];
    int count;
    // millis() when this arrived, so the screen can say how old it is. Not wall-clock: the
    // Orb may have no time source yet, and an age is what a reader wants anyway.
    uint32_t fetchedMs;
};

void intel_store(const IntelSnapshot &snapshot);
bool intel_get(IntelSnapshot &snapshot);
