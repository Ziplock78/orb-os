// ES8311 codec "ping" generator. See audio.h for the core/bus discipline.
//
// Uses the IDF I2S driver directly (the Arduino ESP_I2S wrapper hit an IRAM-safe
// GDMA/interrupt mismatch in the precompiled libs: "Register tx callback failed").
// The ES8311 register init below is the canonical DAC-playback sequence; if the
// speaker stays silent, cross-check it against the Waveshare 08_ES8311 demo — only
// that table is board-specific, the rest is independent.
#include "audio.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include "driver/i2s.h"
#include "esp_heap_caps.h"
#include <math.h>
#include "chime_westminster.h"
#include <SD.h>
#include "theme_sd.h"

#define ES8311_ADDR   0x18
#define SR            16000          // playback sample rate (a beep; pitch-tolerant)
#define I2S_PORT      I2S_NUM_0

static TaskHandle_t s_taskHandle = nullptr;
static bool s_ok = false;
static int16_t *s_buf = nullptr;     // tone scratch in PSRAM (keeps internal RAM free for TLS)
static const size_t S_BUF_LEN = SR / 2 * 2;   // up to 500 ms, stereo interleaved

// How much is handed to I2S at a time while streaming a clip. NOT the buffer size.
//
// play_pcm checks between writes whether something newer has been asked for, and i2s_write
// blocks until the DMA has taken what it was given, so the write size IS the abort latency.
// Writing the whole 500 ms buffer meant a new request waited up to half a second to be
// noticed: the restart was real but late, and late enough that rolling the knob three times
// quickly sounded like the clip repeating rather than restarting. About 30 ms is a fast
// enough answer to feel immediate and still far longer than the DMA needs to stay fed.
static const size_t WRITE_CHUNK = SR / 32 * 2;
static volatile int  s_vol = 60;     // 0..100
static volatile bool s_muted = false;
static volatile int  s_cue = -1;
static SemaphoreHandle_t s_sem = nullptr;

// Named chime library: real recorded audio baked into flash (see chime_westminster.h),
// not synthesized. Add more entries here as more chime files get baked in the same way.
struct ChimeInfo { const char *name; const uint8_t *pcm; size_t bytes; };
static const ChimeInfo CHIMES[] = {
    { "Westminster", CHIME_WESTMINSTER_PCM, CHIME_WESTMINSTER_BYTES },
};
static const int CHIME_COUNT = (int)(sizeof(CHIMES) / sizeof(CHIMES[0]));

// A theme's own sound, waiting to be played. Owned by theme_audio, which holds it for as long
// as its theme is active, so this is a borrow rather than a handover.
static const uint8_t *s_pcm    = nullptr;
static size_t         s_pcmLen = 0;
static volatile int s_chimeIdx = 0;     // AUDIO_CHIME plays this one

// Bumped by every new request. Playback checks it between chunks and gives up the moment it
// changes, so a sound already in flight is INTERRUPTED rather than finished politely.
//
// The picker is what needs this. Turning the knob previews each chime as you pass it, and
// with playback run to completion a fast scroll queued them: you heard the one you left three
// entries ago while looking at a name you had not heard yet. A preview that arrives after you
// have moved on is not a preview of anything.
static volatile uint32_t s_gen = 0;

// Which caller-owned buffer the task is reading right now, or null. The owner watches this
// before freeing; see audio_release_pcm().
static const uint8_t *volatile s_playingPcm = nullptr;

// A file to stream, and whether what is playing right now must be allowed to finish. A chime
// is SUSTAINED: it is the one sound on this device long enough that being cut off is a fault
// rather than a mercy, and the alerts that would cut it fire on their own schedule.
static char s_filePath[96] = { 0 };
static volatile bool s_sustained = false;
static volatile int s_previewIdx = 0;   // cue 4 (preview) plays this one instead

static void es_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}
static uint8_t es_read(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom((int)ES8311_ADDR, 1) != 1) return 0;
    return Wire.read();
}

static void es_update(uint8_t reg, uint8_t andmask, uint8_t ormask) {
    es_write(reg, (uint8_t)((es_read(reg) & andmask) | ormask));
}

// ES8311 init replicated faithfully from the vendor esp_codec_dev driver for this
// exact board: DAC playback, I2S slave, external MCLK = 256*fs, fs = 16 kHz, 16-bit,
// internal reference (ADCL+DACR). Mirrors es8311_open + config_sample + config_fmt +
// set_bits + start + volume/unmute.
static void es8311_init() {
    // --- open() ---
    es_write(0x0D, 0xFA);                 // power up system
    es_write(0x44, 0x08);                 // (written twice: ES8311 first-write quirk)
    es_write(0x44, 0x08);
    es_write(0x01, 0x30);
    es_write(0x02, 0x00);
    es_write(0x03, 0x10);
    es_write(0x16, 0x24);
    es_write(0x04, 0x10);
    es_write(0x05, 0x00);
    es_write(0x0B, 0x00);
    es_write(0x0C, 0x00);
    es_write(0x10, 0x1F);
    es_write(0x11, 0x7F);
    es_write(0x00, 0x80);                 // reset csm/clock, slave mode
    es_write(0x00, 0x80);                 // slave: bit6=0
    es_write(0x01, 0xBF);                 // clk src = SCLK/BCLK-derived (no external MCLK needed)
    es_update(0x06, (uint8_t)~0x20, 0x00); // SCLK not inverted
    es_write(0x13, 0x10);
    es_write(0x1B, 0x0A);
    es_write(0x1C, 0x6A);
    es_write(0x44, 0x58);                 // internal reference (ADCL + DACR) -> drives DAC

    // --- config_sample(): MCLK 4.096 MHz / 16 kHz coeff {pre=1,mult=1,adc=1,dac=1,osr 0x10/0x20,lrck 0xFF,bclk 4} ---
    es_write(0x02, 0x18);                 // pre_div=1, pre_multi=8 (BCLK*8 = DIG_MCLK, use_mclk=false)
    es_write(0x05, 0x00);                 // adc_div=1, dac_div=1
    es_write(0x03, 0x10);                 // fs_mode=0, adc_osr=0x10
    es_write(0x04, 0x20);                 // dac_osr=0x20
    es_update(0x07, 0xC0, 0x00);          // lrck_h=0
    es_write(0x08, 0xFF);                 // lrck_l=0xFF
    es_update(0x06, 0xE0, 0x03);          // bclk_div=4 (preserves SCLK-invert bit)

    // --- config_fmt(NORMAL) + set_bits(16) -> SDP in/out = standard I2S, 16-bit ---
    es_write(0x09, 0x0C);
    es_write(0x0A, 0x0C);

    // --- start() (DAC, slave) ---
    es_write(0x00, 0x80);
    es_write(0x01, 0xBF);                 // keep BCLK-derived clock
    es_write(0x09, 0x0C);                 // DAC iface enabled (bit6=0)
    es_write(0x0A, 0x0C);
    es_write(0x17, 0xBF);
    es_write(0x0E, 0x02);
    es_write(0x12, 0x00);                 // enable DAC
    es_write(0x14, 0x1A);
    es_write(0x0D, 0x01);
    es_write(0x15, 0x40);
    es_write(0x37, 0x08);
    es_write(0x45, 0x00);

    // --- volume + unmute ---
    es_write(0x32, 0xBF);                 // DAC volume ~0 dB
    es_update(0x31, 0x9F, 0x00);          // unmute DAC
}

static bool i2s_setup() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = SR;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;       // stereo
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 4;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.fixed_mclk = 0;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
    if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK) return false;

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = PIN_I2S_MCLK;
    pins.bck_io_num   = PIN_I2S_BCLK;
    pins.ws_io_num    = PIN_I2S_LRCLK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) { i2s_driver_uninstall(I2S_PORT); return false; }
    i2s_zero_dma_buffer(I2S_PORT);
    return true;
}

// Synthesize one beep (freq Hz, ms) with a short fade in/out, into a stereo buffer.
static size_t gen_beep(int16_t *buf, size_t cap, float freq, int ms, float amp) {
    const size_t n = (size_t)((long)SR * ms / 1000);
    const size_t fade = SR / 200;                 // ~5 ms ramps (anti-click)
    size_t i = 0;
    for (; i < n && (i * 2 + 1) < cap; ++i) {
        float env = 1.0f;
        if (i < fade)            env = (float)i / fade;
        else if (i > n - fade)   env = (float)(n - i) / fade;
        const int16_t s = (int16_t)(amp * env * sinf(2.0f * (float)M_PI * freq * i / SR));
        buf[i * 2] = s; buf[i * 2 + 1] = s;       // L = R
    }
    return i * 2;                                  // samples written (stereo interleaved)
}

// Streams pre-recorded PCM (flash-resident, read sequentially — no need to copy it all
// into RAM first) out to I2S in chunks, applying the current software volume on the fly
// by scaling each chunk into the existing tone scratch buffer before writing it. Assumes
// the source is already at the playback format (16kHz/16-bit/stereo) — see
// chime_westminster.h's comment for how it was prepared.
static void play_pcm(const uint8_t *data, size_t bytes) {
    if (!s_buf || !data || bytes < 2) return;
    const uint32_t myGen = s_gen;
    const float g = s_vol / 100.0f;
    const int16_t *src = (const int16_t *)data;
    const size_t totalSamples = bytes / 2;
    size_t i = 0;
    while (i < totalSamples) {
        // Something else asked to be played. Drop the rest of this one and clear what the DMA
        // is still holding, or the tail keeps sounding after the decision to stop it.
        if (s_gen != myGen) { i2s_zero_dma_buffer(I2S_PORT); return; }
        size_t chunk = totalSamples - i;
        if (chunk > WRITE_CHUNK) chunk = WRITE_CHUNK;
        for (size_t k = 0; k < chunk; ++k) s_buf[k] = (int16_t)(src[i + k] * g);
        size_t bw;
        i2s_write(I2S_PORT, s_buf, chunk * sizeof(int16_t), &bw, portMAX_DELAY);
        i += chunk;
    }
}

// Abandon whatever is playing if a newer request has arrived. Called between the pieces of
// the multi-part cues, which are the only ones long enough for it to matter.
static bool superseded(uint32_t myGen) {
    if (s_gen == myGen) return false;
    i2s_zero_dma_buffer(I2S_PORT);
    return true;
}

// Stream a file to I2S in the same short pieces play_pcm uses, so an abort is noticed just as
// quickly and a two minute chime costs no more memory than a two second one.
static void play_file(const char *path, bool sustained) {
    if (!s_buf || !path || !*path) return;
    // The card is shared with the main task and is not thread safe. Held for the whole
    // stream rather than per chunk: a theme load part way through a chime would otherwise
    // interleave reads on one file handle, which is what truncated a chime mid-phrase.
    theme_sd::lock();
    File f = SD.open(path, FILE_READ);
    if (!f) { theme_sd::unlock(); Serial.printf("[audio] cannot open %s\n", path); return; }
    const uint32_t myGen = s_gen;
    const float g = s_vol / 100.0f;
    s_sustained = sustained;
    for (;;) {
        if (s_gen != myGen) { i2s_zero_dma_buffer(I2S_PORT); break; }
        const int got = f.read((uint8_t *)s_buf, WRITE_CHUNK * sizeof(int16_t));
        if (got < 2) break;
        const size_t n = (size_t)got / sizeof(int16_t);
        for (size_t k = 0; k < n; ++k) s_buf[k] = (int16_t)(s_buf[k] * g);
        size_t bw;
        i2s_write(I2S_PORT, s_buf, n * sizeof(int16_t), &bw, portMAX_DELAY);
    }
    s_sustained = false;
    f.close();
    theme_sd::unlock();
}

static void play_cue(int cue) {
    const uint32_t myGen = s_gen;
    // Preview (4) and self-test (2) both ignore mute — they're a deliberate "let me hear
    // it" action from the Settings menu, not an automatic notification.
    if (!s_ok || !s_buf || (s_muted && cue != 2 && cue != 4 && cue != 7 && cue != 9) || s_vol <= 0) return;
    int16_t *buf = s_buf;
    const float amp = (s_vol / 100.0f) * 17000.0f;
    digitalWrite(PIN_AUDIO_PA, HIGH);              // enable speaker amp
    delay(8);                                      // let the amp power up
    size_t bw;
    if (cue == 2) {                                // self-test: ~2 s continuous tone, PA held
        size_t ns = gen_beep(buf, S_BUF_LEN, 1000.0f, 480, amp);
        for (int k = 0; k < 4; ++k) { if (superseded(myGen)) break; i2s_write(I2S_PORT, buf, ns * 2, &bw, portMAX_DELAY); }
    } else if (cue == AUDIO_ALERT) {
        for (int k = 0; k < 2; ++k) {
            if (superseded(myGen)) break;
            size_t ns = gen_beep(buf, S_BUF_LEN, 1320.0f, 80, amp);
            i2s_write(I2S_PORT, buf, ns * 2, &bw, portMAX_DELAY);
            delay(40);
        }
    } else if (cue == AUDIO_CHIME) {                // real recorded chime, whichever is selected
        const int idx = constrain(s_chimeIdx, 0, CHIME_COUNT - 1);
        // Sustained, like a chime off the card. This was missed: the flash chimes went
        // straight to play_pcm and an aircraft beep could truncate Westminster just as easily.
        s_sustained = true;
        play_pcm(CHIMES[idx].pcm, CHIMES[idx].bytes);
        s_sustained = false;
    } else if (cue == 4) {                          // preview: a specific chime, for the picker UI
        const int idx = constrain(s_previewIdx, 0, CHIME_COUNT - 1);
        play_pcm(CHIMES[idx].pcm, CHIMES[idx].bytes);
    } else if (cue == 8 || cue == 9) {              // streamed from the card, never preloaded
        play_file(s_filePath, cue == 8);
    } else if (cue == 6 || cue == 7) {              // a theme's own sound, from the SD card
        if (s_pcm && s_pcmLen >= 2) {
            s_playingPcm = s_pcm;
            play_pcm(s_pcm, s_pcmLen);
            s_playingPcm = nullptr;
        }
    } else if (cue == AUDIO_WIND) {
        // A tick, not a beep: short, high and quiet, so a hundred of them in a row read as
        // a ratchet rather than as an alarm. Half amplitude for the same reason — this one
        // fires per detent while somebody is deliberately turning the knob, which is the
        // opposite situation to an alert that has to interrupt.
        size_t ns = gen_beep(buf, S_BUF_LEN, 2400.0f, 9, amp * 0.45f);
        i2s_write(I2S_PORT, buf, ns * 2, &bw, portMAX_DELAY);
    } else {
        size_t ns = gen_beep(buf, S_BUF_LEN, 880.0f, 160, amp);
        i2s_write(I2S_PORT, buf, ns * 2, &bw, portMAX_DELAY);
    }
    delay(90);                                     // let the DMA clock the tail out before cutting the amp
    digitalWrite(PIN_AUDIO_PA, LOW);               // mute amp between pings (saves power, kills hiss)
}

static void audio_task(void *) {
    for (;;) {
        if (xSemaphoreTake(s_sem, portMAX_DELAY) != pdTRUE) continue;
        // Drain anything that arrived while the last one was playing. The semaphore is binary
        // so at most one is ever pending, and s_cue already holds the newest request, so
        // honouring that token would replay a sound whose turn has passed. One playback per
        // burst, and it is always the latest.
        while (xSemaphoreTake(s_sem, 0) == pdTRUE) { }
        play_cue(s_cue);
    }
}

bool audio_begin() {
    pinMode(PIN_AUDIO_PA, OUTPUT);
    digitalWrite(PIN_AUDIO_PA, LOW);

    const uint8_t id1 = es_read(0xFD), id2 = es_read(0xFE);   // expect 0x83, 0x11
    if (id1 != 0x83) {
        Serial.printf("[audio] ES8311 not found (id=0x%02X 0x%02X)\n", id1, id2);
        s_ok = false;
        return false;
    }
    es8311_init();

    const uint8_t dr[] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x09,0x0A,0x0D,0x0E,0x12,0x14,0x31,0x32,0x37,0x44};
    Serial.print("[audio] ES8311 regs:");
    for (uint8_t r : dr) Serial.printf(" %02X=%02X", r, es_read(r));
    Serial.println();

    if (!i2s_setup()) {
        Serial.println("[audio] I2S init failed");
        s_ok = false;
        return false;
    }
    s_buf = (int16_t *)heap_caps_malloc(S_BUF_LEN * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        Serial.println("[audio] tone buffer alloc failed");
        s_ok = false;
        return false;
    }
    s_sem = xSemaphoreCreateBinary();
    // 2048, not 4096. Measured with uxTaskGetStackHighWaterMark on the device: this task
    // never came within 3,276 bytes of filling its 4 KB, i.e. it uses about 820 B. The other
    // 3 KB sat reserved in INTERNAL RAM, which is the memory the networking stack starves
    // for — a clean boot leaves roughly 9.5 KB free in total. 2048 was well over double the
    // observed peak WHEN THIS TASK ONLY TOUCHED I2S.
    //
    // It streams chimes off the card now, and SD.open plus the FAT layer underneath it needs
    // several kilobytes of stack on its own. 2048 was not close, and the result was exactly
    // what a blown stack looks like from the outside: choosing a chime in Settings crashed the
    // Orb and rebooted it. Sized for the file work now, not for a sine wave.
    xTaskCreatePinnedToCore(audio_task, "audio", 8192, nullptr, 1, &s_taskHandle, 0);  // I2S + SD reads -> core 0
    s_ok = true;
    Serial.println("[audio] ES8311 ready");
    return true;
}

bool audio_present() { return s_ok; }
// Bytes of its 4 KB stack this task has never come within of using, for the memory
// investigation started 2026-08-22 (a fixed number reported per task, cheaper than
// exposing a raw TaskHandle_t across the header and letting every caller learn FreeRTOS).
uint32_t audio_stack_free_bytes() { return s_taskHandle ? uxTaskGetStackHighWaterMark(s_taskHandle) : 0; }
void audio_set_volume(int pct) { s_vol = constrain(pct, 0, 100); }
void audio_set_muted(bool m) { s_muted = m; }

void audio_play(AudioCue cue) {
    if (!s_ok || s_muted) return;
    // Never truncate the hour. An alert or a new-contact beep landing mid-chime is dropped
    // rather than queued: it is a notification about a moment that has passed by the time the
    // chime ends, and cutting a chime off to deliver it is the worse of the two.
    if (s_sustained) return;
    s_cue = (int)cue;
    ++s_gen;
    if (s_sem) xSemaphoreGive(s_sem);
}

// Cue 5 is "play whatever is in s_pcm". The pointer is set before the semaphore is given, and
// the buffer belongs to the caller for the life of the theme, so there is nothing to copy and
// nothing to free here.
void audio_play_pcm(const uint8_t *pcm, size_t bytes, bool ignoreMute) {
    if (!s_ok || (s_muted && !ignoreMute) || !pcm || bytes < 2) return;
    if (s_sustained && !ignoreMute) return;   // see audio_play()
    s_pcm = pcm; s_pcmLen = bytes;
    s_cue = ignoreMute ? 7 : 6;
    ++s_gen;
    if (s_sem) xSemaphoreGive(s_sem);
}

void audio_selftest() {   // ~2 s continuous tone, ignores mute, PA held on
    if (!s_ok) return;
    s_cue = 2;
    ++s_gen;
    if (s_sem) xSemaphoreGive(s_sem);
}

int audio_chime_count() { return CHIME_COUNT; }
const char *audio_chime_name(int idx) {
    if (idx < 0 || idx >= CHIME_COUNT) return "";
    return CHIMES[idx].name;
}
// Bounded at about a fifth of a second. Long enough for a chunk to finish on any healthy
// task, short enough that a wedged one cannot hold up the menu somebody is scrolling.
void audio_release_pcm(const uint8_t *pcm) {
    if (!pcm) return;
    ++s_gen;
    for (int i = 0; i < 100 && s_playingPcm == pcm; ++i) delay(2);
}

void audio_play_file(const char *path, bool preview) {
    if (!s_ok || (s_muted && !preview) || !path || !*path) return;
    // A preview is somebody in the picker asking to hear this one, so it is the single thing
    // allowed to interrupt a chime already ringing.
    if (s_sustained && !preview) return;
    snprintf(s_filePath, sizeof(s_filePath), "%s", path);
    s_cue = preview ? 9 : 8;
    ++s_gen;
    if (s_sem) xSemaphoreGive(s_sem);
}

int  audio_chime_index() { return s_chimeIdx; }
void audio_set_chime(int idx) { s_chimeIdx = constrain(idx, 0, CHIME_COUNT - 1); }

void audio_preview_chime(int idx) {   // ignores mute, like audio_selftest() — see play_cue()
    if (!s_ok) return;
    s_previewIdx = constrain(idx, 0, CHIME_COUNT - 1);
    s_cue = 4;
    ++s_gen;
    if (s_sem) xSemaphoreGive(s_sem);
}
