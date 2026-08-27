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
// Eight hex characters and a NUL. The gateway's handle for one story, hashed from the
// headline. See intel_brief_want() for why a story is addressed by hash and not by
// position.
#define INTEL_KEY_BYTES   9
// One story's summary, as the feed wrote it. The gateway caps what it sends at 400
// characters; this is that plus a NUL plus room to spare, and it is ONE buffer for the
// story being read rather than one per headline. Twenty of these would be eight kilobytes
// of internal RAM held permanently for text nobody has asked to see, on a board that
// cannot negotiate TLS because it cannot find two contiguous sixteen-kilobyte blocks.
#define INTEL_BRIEF_BYTES 432

struct IntelItem {
    char text[INTEL_TEXT_BYTES];      // the headline, already cut to fit by the worker
    char source[INTEL_SOURCE_BYTES];  // "BBC", "BBC Sport", "NASA" — shown as a credit
    char key[INTEL_KEY_BYTES];        // ask the gateway for this story's summary with it
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

// ---------------------------------------------------------------------------
// One story's summary, fetched on demand when somebody presses a headline.
//
// Same core-0-writes / core-1-reads handoff as the snapshot above, and the same mutex. The
// UI asks by key and then watches the state; the network task does the waiting. Nothing
// here may block the LVGL task, which is the whole reason this is a state machine rather
// than a function that returns a string.
enum IntelBriefState : uint8_t {
    INTEL_BRIEF_IDLE,      // nobody has asked
    INTEL_BRIEF_WANTED,    // asked; the network task has not picked it up yet
    INTEL_BRIEF_LOADING,   // in flight
    INTEL_BRIEF_READY,     // body is good
    INTEL_BRIEF_EMPTY,     // the feed carries no description for this story. Not a fault.
    INTEL_BRIEF_GONE,      // the story aged out of the feed between the list and the press
    INTEL_BRIEF_FAILED,    // no network, or the gateway did not answer
};

struct IntelBrief {
    IntelBriefState state;
    char key[INTEL_KEY_BYTES];
    char headline[INTEL_TEXT_BYTES];
    char source[INTEL_SOURCE_BYTES];
    char body[INTEL_BRIEF_BYTES];
};

// UI: ask for a story's summary. Cheap and idempotent — asking again for the key already
// held is a no-op, so a second press on the same headline shows what is already there
// instead of refetching it.
void intel_brief_want(const char *key, const char *headline, const char *source);
// UI: stop caring (the briefing was closed). Frees nothing, but stops a late answer from
// arriving into a screen nobody is looking at.
void intel_brief_release();
// UI: what to draw right now.
bool intel_brief_get(IntelBrief &out);
// Network task: is there a key waiting to be fetched? Copies it out and moves to LOADING.
bool intel_brief_take(char *keyOut, size_t keyCap);
// Network task: the answer, whatever it was.
void intel_brief_store(const char *key, IntelBriefState state,
                       const char *headline, const char *source, const char *body);
