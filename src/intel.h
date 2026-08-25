#pragma once

#include <stdint.h>
#include <stddef.h>

// Headlines for the Intel screen, shared by the network task and the LVGL UI.
//
// Sized by what the dial can actually show. The worker cuts every headline to 70 characters
// on a word boundary before it is sent, so the buffer only has to hold that plus a NUL and
// the room multi-byte characters take: an ellipsis is three bytes in UTF-8 and a headline
// may end in one, and quotes come back as typographic quotes from some feeds.
// How many headlines the Orb HOLDS. Twenty is well past what any dial can show at once;
// the surplus is what the knob scrolls through.
#define INTEL_MAX_ITEMS   20
// How many row widgets the screen BUILDS. Deliberately far below the item count: even at
// the smallest compiled face only six or seven rows fit on a 466 px circle, so building
// twenty would be eighteen idle LVGL objects on a device whose internal RAM is the scarce
// resource. The window slides over the items; the widgets stay put.
#define INTEL_MAX_ROWS    8
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

// Two narrow reads, so nothing has to put a whole IntelSnapshot on the stack.
//
// At twenty items the struct is over 2 KB, and it used to be copied wholesale by both the
// render path and the once-a-minute age tick. Two kilobytes of stack per call is not
// something to spend on a device whose internal heap is measured in single-digit
// kilobytes, and neither caller ever wanted all of it.
//
// intel_meta: how old the set is and how many it holds. Nothing else.
bool intel_meta(uint32_t &fetchedMs, int &count);
// intel_window: `n` items starting at `from`, clamped to what exists. Returns how many
// were actually written, and reports the full count through `total`.
int  intel_window(int from, int n, IntelItem *out, int &total);
