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
#include <stddef.h>
uint32_t audio_stack_free_bytes();  // bytes of its stack never touched, for /taskmem
bool audio_present();
void audio_set_volume(int pct);     // 0..100 (software amplitude)
void audio_set_muted(bool muted);
void audio_play(AudioCue cue);      // non-blocking: signals the playback task
// Play a caller-owned PCM buffer: 16 kHz, 16-bit, stereo interleaved, the same format the
// built-in chimes are in. Non-blocking. The buffer must outlive the playback, which is why
// theme_audio holds its sounds for as long as the theme is active rather than handing over
// something it is about to free.
// `ignoreMute` is for a preview: picking a chime out of a menu is a deliberate "let me hear
// it", the same reason cue 4 and the self-test ignore mute, and a picker that plays nothing
// because the device is muted is a picker you cannot use.
void audio_play_pcm(const uint8_t *pcm, size_t bytes, bool ignoreMute = false);
// Stop `pcm` if it is playing and WAIT until the playback task has let go of it, so the
// caller can free it. Interrupting alone is not enough: the task can be halfway through a
// chunk when the request to stop arrives, and freeing under it is a use-after-free that would
// surface as noise or a crash rather than as anything obviously about audio. Bounded, so a
// wedged task cannot hang the caller.
void audio_release_pcm(const uint8_t *pcm);

// Play raw PCM straight off the SD card, in pieces, without loading it first.
//
// This is what lets a chime be as long as somebody wants. Holding one in memory is what
// created a length limit at all, and on a chip where the clock alone takes two and a half
// megabytes there is no length that would be both generous and safe. A chime rings once an
// hour; the card has an easy time of it.
//
// `preview` means somebody is browsing the picker: it ignores mute, and it is the one thing
// allowed to cut a chime short. Otherwise a chime plays to the end, because an aircraft beep
// arriving at ten past the hour has no business truncating it.
void audio_play_file(const char *path, bool preview = false);
void audio_selftest();              // ~2 s continuous tone for by-ear verification

// Named chime library (real recorded audio, baked into flash — see chime_westminster.h).
// AUDIO_CHIME plays whichever index is currently selected via audio_set_chime().
int         audio_chime_count();          // number of chimes available
const char *audio_chime_name(int idx);    // display name, bounds-checked
int         audio_chime_index();          // currently selected chime
void        audio_set_chime(int idx);     // select active chime (caller persists the index)
void        audio_preview_chime(int idx); // play a specific chime once, ignoring mute (picker UI)
