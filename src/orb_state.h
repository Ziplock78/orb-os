// The Orb OS — the state the firmware's top-level modules share.
//
// Until stage 1 of REWRITE_PLAN.md all of this was file-static in a 4,000-line main.cpp.
// That file is now split by job (orb_net_task, orb_storage, orb_location, orb_wifi,
// orb_web, orb_apps, orb_stats, and a thin main), and these are the variables more than
// one of those files reads or writes. They are still plain globals, touched from both
// cores exactly as before: the split moved code and changed no behaviour. Stage 5
// replaces the cross-core traffic with typed queues; until then this header is the
// inventory of what crosses a module boundary, and the comments on each variable are in
// orb_state.cpp beside its definition.
#pragma once

#include <Arduino.h>
#include <vector>
#include <map>
#include <string>
#include <WiFiManager.h>
#include <lvgl.h>
#include "aircraft.h"
#include "adsb_client.h"
#include "radar_view.h"

// Wireless firmware update (ArduinoOTA + the browser upload page). Needs a second app
// partition to write the incoming image into, and partitions_16MB_themeart.csv gives
// that 6.25 MB to pre-baked theme art instead. Flip this and the partition table
// together, never one alone: with no OTA partition present the code below would compile
// and run but fail at the first write, which is worse than not offering it.
//
// Here rather than in one .cpp because orb_web.cpp and orb_wifi.cpp both test it. Any
// file that tests it must include this header: an #if on an undefined name is silently 0.
#define ORB_OTA_ENABLED 0

// ---- defined in orb_state.cpp ----
extern std::vector<Aircraft>            g_aircraft;
extern std::map<std::string, Aircraft>  g_acTable;
extern SemaphoreHandle_t                g_ac_mutex;
extern TaskHandle_t                     g_adsbTaskHandle;
extern TaskHandle_t                     g_audioTaskHandle;
extern volatile bool                    g_acDirty;
extern AdsbClient                       g_adsb;
extern RadarSettings                    g_settings;
extern WiFiManager                      g_wm;
extern int                              g_brightnessDay;
extern int                              g_volume;
extern int                              g_tickVol;
extern int                              g_chimeVol;
extern bool                             g_muted;
extern int                              g_chimeIdx;
extern bool                             g_soundRadar;
extern bool                             g_soundChime;
extern int                              g_alertMode;
extern float                            g_proximityKm;
extern uint32_t                         g_idleDimMs;
extern bool                             g_showSweep;
extern int                              g_units;
extern int                              g_wxUnits;
extern volatile bool                    g_wxOpened;
extern volatile bool                    g_wxClosed;
extern int                              g_wxZoomTier;
extern volatile bool                    g_wxZoomChanged;
extern bool                             g_showAirports;
extern bool                             g_hideGround;
extern int                              g_minAltFt;
extern int                              g_deadZonePx;
extern bool                             g_milOnly;
extern volatile bool                    g_locationSet;
extern bool                             g_locateTried;
extern int                              g_rotation;
extern int                              g_trailLen;
extern int                              g_maxAc;
extern bool                             g_bigText;
extern volatile bool                    g_onBattery;
extern bool                             g_rtcSynced;
extern std::vector<Aircraft>            g_snap;
extern volatile bool                    g_requery;
extern volatile uint32_t                g_pollOverrideMs;
extern float                            g_requeryKm;
extern volatile bool                    g_feedOk;
extern volatile bool                    g_radarViewActive;
extern volatile uint32_t                g_lastFeedOkMs;
extern volatile uint32_t                g_rebootAtMs;
extern int                              g_sdUpCount;
extern String                           g_pendingSlug;
extern volatile uint32_t                g_applySlugAtMs;
extern String                           g_tz;
extern String                           g_locName;
extern volatile bool                    g_weatherDirty;
extern volatile bool                    g_wxRadarDirty;
extern volatile bool                    g_wxAnimDirty;
extern volatile bool                    g_cloudImageDirty;
extern volatile bool                    g_intelDirty;
extern volatile bool                    g_tickerDirty;

void     host_set_poll_override(uint32_t ms);
uint32_t host_get_poll_override();

// ---- defined in main.cpp ----
// The hold-to-reboot overlay, which the location and factory-reset countdowns reuse.
extern lv_obj_t *g_holdWarning;
extern lv_obj_t *g_holdWarningLbl;
void applyBrightness();   // idle dim + face-down sleep + update-in-progress, combined
