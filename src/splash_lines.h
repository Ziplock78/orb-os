#pragma once
#include <lvgl.h>

// The three standing lines on the splash / About screen, and the glass over them.
//
// Why this is a module and not two copies: the splash and the About page are the same
// picture in two places, and before this they diverged. The boot splash was the picture
// alone; About was the picture plus three labels nailed to hardcoded offsets in
// settings_view.cpp. Making those three themeable in one of the two would have finished the
// job in half the places it exists.
//
// What it draws, in order, over whatever the parent already shows:
//
//   1. the firmware version, the config address, and the data credits, into one canvas
//   2. clock_overlay.png, the glass and CRT, on top of all of it
//
// The glass is last and is not negotiable. It is the top layer of every screen on this
// device, and a screen that puts anything above it looks like a mistake because it is one.
// The splash used to get this by having the glass painted into splash.png in the browser,
// which worked only for as long as the firmware drew nothing afterwards.
//
// The three lines are drawn for EVERY theme: their compiled defaults are the offsets
// settings_view.cpp used to hardcode, so nothing already on a card loses them. Only the
// GLASS is conditional, on theme_style::Splash::styled, because an older theme's splash.png
// has it painted in already and compositing it again would show it twice.
namespace splash_lines {

// Build onto a parent that is already showing splash.png. Safe to call on a parent that
// has had attach() called before: the previous objects are dropped first.
void attach(lv_obj_t *parent);

// The config-page address, once the network knows it. Before that the line reads
// capsuleradar.local, which is true from the moment mDNS is up and does not need an IP.
void setNetwork(const char *line);

// Drop the canvas and the decoded overlay. The canvas is a full-screen RGB565+alpha
// raster, which is 651 KB of PSRAM that has no business staying resident behind a clock.
void release();

} // namespace splash_lines
