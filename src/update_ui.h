#pragma once
#include <stdint.h>   // uint32_t in file_progress below

// On-screen update status: the Orb tells the user it is mid-update.
//
// Before this existed the device was silent while Launch Kit worked on it. Files arrived
// over WiFi with nothing on screen, the post-send reboot looked like a crash, and the
// bake after it ran before the display was even initialised, so the panel just froze on
// whatever it last showed. From the desk there was no way to tell "updating" from
// "stale" from "broken", and Zion had to ask which one he was looking at, more than once.
//
// The contract with the user is simple: while anything about the device is being
// updated, the screen says so, and says whether another restart is coming. When the
// update is done the messages go away and the device returns to the clock. Clock showing
// means done; anything else means wait.
//
// All calls must come from the LVGL task (setup() or loop()); every caller here does.
namespace update_ui {

// A theme file just landed over WiFi (/sdput). Shows the "receiving files" overlay, or
// refreshes its counter. If files stop arriving and no reboot follows within ~12 s, the
// overlay says the update was interrupted and then clears itself: a failed send must not
// leave a permanent "updating" screen over a device that is otherwise fine.
void file_received(const char *name, int count);

// A CHUNK of the current file just landed. Files arrive whole before file_received() can be
// called, and a single big one (a 98 KB plate becomes ~130 KB base64 on the wire, over ten
// seconds at this line rate) outlasts the twelve-second interrupted-watchdog on its own. So
// the device announced a dead transfer, cleared its own overlay, dropped back to whatever
// app was showing, and then carried on receiving the very file it had just given up on.
// Feeding progress here keeps the watchdog measuring what it means to measure: silence,
// rather than a large file.
void file_progress(const char *name, int count, uint32_t bytes);

// The device is about to restart as part of an update (called by /reboot only while the
// overlay is up). Swaps the message to "restarting to finish the update" so the reboot
// reads as expected progress, not a crash.
void rebooting();
// Boot is finished. Takes the boot notice down and shows nothing in its place; the splash
// holds, fades, and the clock is underneath. See update_ui.cpp.
void booted();

// The host is about to hand this chip to a firmware flasher over USB.
//
// This is the one update the device cannot narrate. A theme install runs with the firmware
// running, so it can count files and repaint. A firmware flash resets the chip into its ROM
// bootloader, and from that moment this code is not executing at all: no LVGL, no display
// driver, no timer. The panel simply keeps the last frame it was given, for the whole write.
//
// So the frame it is holding has to be chosen on purpose. Left alone it is a clock with a
// stopped second hand, which reads as a crash, and someone watching a crashed clock for two
// minutes pulls the cable, which is the one action that actually does destroy the board.
// This paints the explanation while there is still a processor to paint it with.
//
// Nothing clears this from the device side, because nothing on the device runs again until
// the new firmware boots. The watchdog only covers the case where the flash never starts.
void firmware_incoming();

// Boot, honestly.
//
// The Orb draws a correct, good-looking clock and then blocks for 10 to 20+ seconds inside
// g_wm.autoConnect() trying the saved network. lv_timer_handler() does not run during
// blocking setup() code, so the knob is not polled either: the device looks finished and is
// completely deaf, and the owner reasonably concludes it is broken. Rocking the knob during
// that window does nothing, which is exactly what was reported.
//
// booting() puts an honest notice over the clock for the length of that block. It is not a
// progress bar because there is nothing to count: it says what is happening and that the
// knob will not answer yet.
void booting(const char *what);

// Everything is up and the knob is live.
//
// After a firmware update this WAITS for the knob, because that is the moment the question
// "is it finished?" actually gets asked, and answering it with a clock that may or may not
// respond is what made the update feel broken. On an ordinary power-on it clears itself
// after a moment: a desk clock that demands a button press every time it is plugged in is a
// worse device than one that occasionally starts a second slower than it looks.
void ready(bool needsAck);

// True while a ready notice is waiting to be acknowledged. input_router asks, so whichever
// input dismisses the notice is not also delivered to whatever app is underneath it.
bool awaitingAck();

// Any knob input clears it: a turn in either direction or a press. A notice whose whole
// message is "the knob is yours again" must not be the one screen that ignores most of the
// knob.
void ackReady();

// Boot-time bake progress: converting the received theme into the flash cache. This is
// the "second restart" leg of a full update and takes ~15 s for a rich theme.
void bake_begin(int totalAssets);
void bake_progress(const char *assetName, int done, int totalAssets);
void bake_done();

} // namespace update_ui
