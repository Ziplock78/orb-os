#pragma once
// ES8311 codec + speaker: short alert "pings". Device-only.
//
// Bus discipline: the ES8311 is configured over the shared I2C bus, so audio_begin()
// MUST run on core 1 (setup), like the other I2C devices. Playback afterwards only
// touches the I2S peripheral + the PA GPIO (no I2C), so it runs in its own task.
#include <stdbool.h>

enum AudioCue {
    AUDIO_NEW   = 0,   // new aircraft entered range (soft single beep)
    AUDIO_ALERT = 1,   // emergency / military contact (urgent double beep)
    AUDIO_CHIME = 3,   // top-of-hour clock chime (gentle descending phrase)
    AUDIO_WIND  = 5,   // one detent of winding the clock's mainspring (a short dry tick)
};

bool audio_begin();                 // init ES8311 + I2S + PA + playback task (call on core 1)
#include <stdint.h>
uint32_t audio_stack_free_bytes();  // bytes of its stack never touched, for /taskmem
bool audio_present();
void audio_set_volume(int pct);     // 0..100 (software amplitude)
void audio_set_muted(bool muted);
void audio_play(AudioCue cue);      // non-blocking: signals the playback task
void audio_selftest();              // ~2 s continuous tone for by-ear verification

// Named chime library (real recorded audio, baked into flash — see chime_westminster.h).
// AUDIO_CHIME plays whichever index is currently selected via audio_set_chime().
int         audio_chime_count();          // number of chimes available
const char *audio_chime_name(int idx);    // display name, bounds-checked
int         audio_chime_index();          // currently selected chime
void        audio_set_chime(int idx);     // select active chime (caller persists the index)
void        audio_preview_chime(int idx); // play a specific chime once, ignoring mute (picker UI)
