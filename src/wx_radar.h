#pragma once

#include <stdint.h>

#define WX_RADAR_SIZE 360
#define WX_RADAR_SOURCE_SIZE 512
#define WX_RADAR_FRAMES 5        // the past ~50 min of precipitation, one frame per ~10 min, cycled for motion.
                                 // Raised from 3 when the owner asked to watch an hour go by. RainViewer
                                 // offers 12 to 13 past frames, so the source was never the limit; memory
                                 // is. Each frame is a 360x360 RGB565 buffer at 253 KB.
                                 //
                                 // SIX WAS TRIED AND IS TOO MANY. It fetched once and then, after the
                                 // next reboot, the decoder's own 253 KB native buffer could not find a
                                 // contiguous block: PSRAM free had fallen to 382 KB and the weather app
                                 // spent every cycle logging "PSRAM native buffer allocation failed".
                                 // Marginal is worse than fewer. Five leaves room for that buffer and for
                                 // the app menu's artwork, which needs ~1.3 MB transiently when opened.
                                 //
                                 // The full hour needs the frames stored 8-bit paletted rather than
                                 // RGB565, which halves each to ~130 KB. RainViewer's scale is a handful
                                 // of colours, so nothing is lost by it. That is the next piece.
                                 //
                                 // wx_radar_begin() counts what actually landed and the UI only shows
                                 // filled slots, so an Orb that cannot find room degrades to fewer frames
                                 // rather than to a blank screen.
                                 // Reduced from 7 to 3 in the lean-weather-radar redesign (see
                                 // docs/lean-weather-radar-redesign.md): each frame is a full 360x360
                                 // RGB565 PSRAM buffer (~253KB) AND each frame is one more tile fetch
                                 // (TLS handshake + HTTP buffers on the scarce internal heap), so fewer
                                 // frames cuts both the resident PSRAM and the per-cycle internal-heap
                                 // churn that starves the weather radar's own TLS handshake (-32512).

void wx_radar_begin(void);   // take the frame buffers
// Give them back. MUST be called from the network task only: see the note in wx_radar.cpp.
// The UI asks for this by flag; it never calls it.
void wx_radar_release(void);

// Build the map under the weather (major roads and the shoreline) for this centre and zoom.
// CALL THIS FROM THE UI THREAD ONLY: it reads the SD card, and the Arduino SD driver cannot
// be called from two tasks at once. Cheap to call repeatedly — it returns immediately unless
// the centre or zoom actually moved.
void wx_map_prepare(double lat, double lon, int tier);
bool wx_radar_ready(void);   // true while the buffers exist, so a fetch has somewhere to go
uint16_t *wx_radar_back_buffer(void);                    // scratch: the client decodes one frame here

// Commit the scratch buffer as frame `slot` (0 = oldest, FRAMES-1 = newest) of refresh
// generation `gen`. A generation is one full pass over the past frames at one zoom level;
// bumping gen (client-side) starts a fresh loop, so the UI can tell a half-filled new loop
// from last cycle's complete one. `frameTime` is the RainViewer epoch stamp for that frame.
void wx_radar_commit_frame(int slot, uint32_t gen, uint32_t frameTime, double lat, double lon);

// Fetch frame `slot` if it belongs to generation `gen` (false otherwise, e.g. not filled
// yet this cycle). The returned buffer is stable until that same slot is re-committed.
bool wx_radar_frame(int slot, uint32_t gen, const uint16_t **pixels, uint32_t *frameTime);

uint32_t wx_radar_gen(void);              // newest generation any slot holds (0 = nothing yet)
int      wx_radar_gen_count(uint32_t gen); // how many slots (from 0 up) are filled for that gen
uint32_t wx_radar_version(void);          // bumps on every commit — UI uses it to drop the "UPDATING" overlay
bool     wx_radar_center(double *lat, double *lon);   // center of the frames currently held
