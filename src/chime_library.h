#pragma once

#include <stddef.h>

// Every chime on the device, wherever it came from.
//
// The hour is the DEVICE's, not the theme's. Zion's: somebody wearing Steam Punk should be
// able to hear Modern's chime on the hour if that is the one they like, without changing what
// their clock looks like. A sound you hear once an hour and a dial you look at all day are not
// the same choice, and tying them together makes you trade one for the other.
//
// So this is a library rather than a property of the worn theme: the chimes baked into the
// firmware, plus one from every theme on the SD card that ships a chime.pcm. Settings already
// drives its picker entirely through host_chime_count/name/index/set/preview, so it needed no
// changes at all to gain them.
//
// THE SELECTION IS AN IDENTITY, NOT AN INDEX. Installing or deleting a theme changes what is
// in the list and where everything sits in it, so a stored index would quietly start meaning a
// different chime. What is persisted is which chime it is; the index is worked out from that.

namespace chime_library {

// Build the list from what is installed, then load whatever was selected. Call once at boot,
// after theme_select::init() and the SD card are up.
void begin();

// Called when the set of installed themes changes, so a chime that arrived with a theme shows
// up without a reboot and one that left stops being offered.
void rescan();

int         count();
const char *name(int idx);      // bounds-checked; "" past the end
int         selected();         // index into the current list
void        select(int idx);    // persists the identity, loads the audio
void        preview(int idx);   // play one now, ignoring mute, for the picker

// The hour. Plays whatever is selected, from flash or from the card.
void playSelected();

}  // namespace chime_library
