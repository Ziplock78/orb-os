#pragma once
// M0 bring-up: CO5300 AMOLED (Arduino_GFX over QSPI) + LVGL.
// Owns the panel + LVGL display driver so main.cpp stays glue-only.
// Touch (CST9217 indev) and the radar UI come in later milestones.
#include <stdint.h>

namespace display {

// Init the panel and LVGL (draw buffers in PSRAM) and show the M0 hello screen.
// Returns false if the panel failed to initialize.
bool begin();

// Pump LVGL: render dirty areas + run LVGL timers. Call every loop() iteration.
void loop();

// 0..255 panel brightness (CO5300 command 0x51).
// Stamp an input so the next COMPLETED frame reports how long it took to reach the glass.
//
// The detent handler was measured at 1 ms and the menu still felt slow, which means the
// gap is not the handler, it is everything between the handler and the pixels: LVGL's
// refresh tick, and whatever else is still drawing on that frame. Timing the handler
// answered the wrong question. This answers the one that was asked.
void markInput(uint32_t ms);

void setBrightness(uint8_t v);

// ms since the last touch (LVGL inactivity timer) — for idle auto-dim.
uint32_t inactiveMs();

// Count as user activity (resets the inactivity timer) — call on knob use so the
// screen doesn't auto-dim while turning/pressing the encoder.
void noteActivity();

// Rotate the whole UI clockwise by an arbitrary number of degrees (normalized to
// 0..359), e.g. to compensate for any enclosure/mounting angle. Cardinal rotations
// keep their optimized flush paths; other angles use a PSRAM framebuffer. Touch input
// is transformed back into the same logical coordinate space.
void setRotation(uint16_t degrees);
uint16_t rotation();

} // namespace display

uint32_t display_frames();   // total rendered frames (for FPS measurement)
uint32_t display_lvgl_us();  // cumulative microseconds inside lv_timer_handler()
uint32_t display_flush_us(); // cumulative microseconds pushing pixels to the panel
uint32_t display_flushed_px(); // cumulative pixels flushed (how much screen is repainted)

// Silence the per-event console chatter, for a screen where the chatter is a cost rather
// than a diagnostic.
//
// Serial is not free on this chip. HWCDC::write pushes into a ring buffer with a 100 ms
// timeout, and a FULL buffer blocks the caller for up to that long — so with a monitor
// attached, printing is a real part of the frame. The Orb writes two lines per detent, one
// from the knob and one from the flush, and a brisk wind is five or six detents a second.
// The winding investigation was therefore measuring itself: attach a recorder to find out
// why winding is slow, and the recorder is part of the answer.
//
// Lives here rather than in knob.h or wind_notice.h because it has two readers in different
// layers and one writer, and a flag with two copies is a flag that will disagree with
// itself. Set it in ONE place (loop(), on the wind screen appearing) and both honour it.
void orb_log_set_quiet(bool quiet);
bool orb_log_quiet();
