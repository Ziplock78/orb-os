#pragma once

#include <stdint.h>

#define WX_RADAR_SIZE 360
#define WX_RADAR_SOURCE_SIZE 512
#define WX_RADAR_FRAMES 3        // past ~30 min of precipitation, one frame per ~10 min, cycled for motion.
                                 // Reduced from 7 to 3 in the lean-weather-radar redesign (see
                                 // docs/lean-weather-radar-redesign.md): each frame is a full 360x360
                                 // RGB565 PSRAM buffer (~253KB) AND each frame is one more tile fetch
                                 // (TLS handshake + HTTP buffers on the scarce internal heap), so fewer
                                 // frames cuts both the resident PSRAM and the per-cycle internal-heap
                                 // churn that starves the weather radar's own TLS handshake (-32512).

void wx_radar_begin(void);
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
