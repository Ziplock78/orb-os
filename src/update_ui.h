#pragma once

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

// The device is about to restart as part of an update (called by /reboot only while the
// overlay is up). Swaps the message to "restarting to finish the update" so the reboot
// reads as expected progress, not a crash.
void rebooting();

// Boot-time bake progress: converting the received theme into the flash cache. This is
// the "second restart" leg of a full update and takes ~15 s for a rich theme.
void bake_begin(int totalAssets);
void bake_progress(const char *assetName, int done, int totalAssets);
void bake_done();

} // namespace update_ui
