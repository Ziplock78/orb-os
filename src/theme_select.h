#pragma once
// Which Launch Kit theme (of however many are installed on the SD card) is active
// on this Orb — the data half of the Settings "Design" page. Same model as
// app_theme.h/.cpp (Default/Office): persists the choice, then does a real reboot
// (device: ESP.restart(); sim: an actual re-exec via setRestartHook, not a live
// in-place repaint) so no screen ever runs a mix of the old and new theme's
// decoded art. This mirrors app_theme almost exactly, just for a dynamic list of
// string slugs (whichever /themes/<slug>/ folders actually exist on the card)
// instead of a fixed two-entry enum.
#include <stddef.h>

namespace theme_select {

constexpr int MAX_SLUG_LEN = 32;
constexpr int MAX_THEMES   = 16;   // generous — SD capacity isn't the constraint, this array is

void init();                 // load the saved slug from NVS; call once at boot (mirrors app_theme::init())
const char *activeSlug();    // "" if none saved, or nothing installed — callers fall through to flash/stock
void set(const char *slug);  // persists, then reboots/re-execs — see app_theme::set()

// Delete an installed theme's folder from the card. Returns false if the slug is not
// installed, or if it is the one currently being worn: a theme cannot be pulled out from
// under the screens drawing it, and the caller is expected to switch first and say so.
//
// The _installed sentinel goes FIRST, mirroring the install contract that writes it last.
// A delete interrupted halfway then leaves a folder that no longer counts as a theme
// rather than a half-empty one still offering itself in the list.
bool removeInstalled(const char *slug);

// Native only: sim_main.cpp registers its own re-exec here, same reasoning as
// app_theme::setRestartHook — a fresh process needs this file's static state
// reloaded from the persisted slug, not defaulted, right after re-exec.
void setRestartHook(void (*hook)());

// Scans /themes/ on the card for installed theme folders (subdirectories, one per
// slug — see the Launch Kit export pipeline). Fills out[0..n) with each slug name,
// found order (not necessarily alphabetical). Returns the count found, capped at
// MAX_THEMES. No card / no /themes/ folder -> returns 0, not an error.
int listInstalled(char out[][MAX_SLUG_LEN]);

} // namespace theme_select
