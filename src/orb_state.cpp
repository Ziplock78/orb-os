// The Orb OS — the state the firmware's top-level modules share. Declared in orb_state.h.
//
// Moved here from main.cpp unchanged, comments and all; only `static` came off, because
// these are now read from more than one file. See orb_state.h for why they are still
// plain globals.
#include "config.h"
#include "orb_state.h"

// ---- shared state ----
std::vector<Aircraft> g_aircraft;      // latest snapshot: g_acTable, flattened, every poll
std::map<std::string, Aircraft> g_acTable;   // hex -> last known fix. Upserted, never wholesale
                                                     // replaced, so one missed poll dims a contact
                                                     // instead of deleting it. See applyPolledAircraft().
SemaphoreHandle_t     g_ac_mutex;      // guards g_aircraft
// Handles captured at creation, purely so /taskmem can ask each task how much of its own
// stack it has ever come within of using (uxTaskGetStackHighWaterMark). Phase 0 of the
// 2026-08-22 memory investigation: before trimming anything, find out who is actually
// holding the internal RAM that ADS-B polling needs.
TaskHandle_t          g_adsbTaskHandle = nullptr;
TaskHandle_t          g_audioTaskHandle = nullptr;
volatile bool         g_acDirty = false; // set when a new snapshot is ready
AdsbClient            g_adsb;
RadarSettings         g_settings;
WiFiManager           g_wm;
int                   g_brightnessDay = BRIGHTNESS_DEFAULT;   // user brightness (web/NVS)
int                   g_volume = 60;                          // alert volume 0..100 (web/NVS)
// The clock's tick and the hourly chime, each 0..100 against g_volume, 0 being off.
// Deliberately low: a theme that arrives ticking at full volume is one somebody switches off
// rather than turns down. The tick is quieter than the chime because it is in the room all
// day where a chime is an event once an hour. Zion, 2026-10-05.
int                   g_tickVol = 20;
int                   g_chimeVol = 30;
bool                  g_muted  = false;                       // mute alert pings
int                   g_chimeIdx = 0;                         // selected chime (Settings/NVS)
bool                  g_soundRadar = false;                   // on-device: radar pings on/off
bool                  g_soundChime = false;                   // on-device: top-of-hour clock chime
int                   g_alertMode = 2;                        // 0=off 1=emergencies 2=new+emergencies (web/NVS)
float                 g_proximityKm = 0.0f;                   // proximity alert radius, km (0=off) (web/NVS)
uint32_t              g_idleDimMs = IDLE_DIM_MS;              // dim after this idle time (0 = never)
bool                  g_showSweep = true;                     // rotating sweep line on/off (web/NVS)
int                   g_units = 0;                            // 0=Aviation 1=Metric 2=Imperial (web/NVS)
int                   g_wxUnits = 0;                          // Weather app only: 0=Auto 1=Metric 2=Imperial (Settings/NVS)
// The weather app's two requests to the network task, which owns the frame buffers.
//
// The UI never allocates or frees them. It says "I have been opened" and "I have been
// closed", and the task acts on that between fetches, when it knows it is not writing into
// them. Freeing from the UI thread hung the device inside a minute: a decode already in
// flight kept writing into memory that had just been handed back.
volatile bool         g_wxOpened   = false;
volatile bool         g_wxClosed   = false;
int                   g_wxZoomTier = 0;                       // Weather map range: 0=50mi 1=100mi (Settings/NVS)
volatile bool         g_wxZoomChanged = false;                // set on cycle so adsb_task refetches immediately
bool                  g_showAirports = true;                  // airport markers on/off (web/NVS)
bool                  g_hideGround   = false;                 // skip on-ground aircraft in the feed (web/NVS)
int                   g_minAltFt     = 0;                     // only show aircraft above this altitude, ft (0 = off) (web/NVS)
int                   g_deadZonePx   = 0;                     // ignore aircraft inside this many px of scope center (0 = off) (pushed design)
bool                  g_milOnly      = false;                 // only show military-flagged aircraft (web/NVS)
// Has a real location ever been established on this device? (NVS "locSet")
//
// Read by core 0 to decide whether polling the aircraft feed is worth doing at all, and by
// the status tick to tell the Flight Tracker whether it can honestly draw a sky. False on a
// fresh or factory-reset device until either the network lookup succeeds or the owner picks
// somewhere in Settings. Without it there was no difference between a device that had never
// located itself and one legitimately sitting on the compiled default, so a failed lookup
// showed the default's sky with the same confidence as a real fix.
volatile bool         g_locationSet  = false;
// Has this boot already asked the network where it is? One attempt per boot: a network that
// refuses the lookup will keep refusing it, and the owner can always pick a place in
// Settings. File scope rather than a static inside loop() so host_location_reset() below can
// set it — without that, clearing locSet over the debug channel would fire the retry on the
// very next tick and the state under test would last about a second.
bool                  g_locateTried  = false;
int                   g_rotation = 0;                         // clockwise display rotation, 0..359° (web/NVS)
int                   g_trailLen = 2;                         // aircraft trails 0=off 1=short 2=med 3=long (web/NVS)
int                   g_maxAc = 12;                           // max aircraft drawn on the scope (web/NVS)
bool                  g_bigText = false;                      // accessibility: large fonts (web/NVS, applied at boot)
volatile bool         g_onBattery = false;                    // discharging (set on core 1, read on core 0)
bool                  g_rtcSynced = false;                    // RTC written from NTP this session?
std::vector<Aircraft> g_snap;                                 // last snapshot (instant re-render on zoom)
volatile bool         g_requery = false;                      // range changed -> adsb_task re-begins
// Diagnostic override for the ADS-B poll interval (0 = use the compiled default). Exists
// because the poll is the biggest periodic work on the device and the prime suspect for a
// periodic frame hitch: proving that by changing one number live beats one flash per guess.
volatile uint32_t     g_pollOverrideMs = 0;
// Set from the web /rdbg AND from the cable (orb_link "poll"). The cable matters: the whole
// point of this investigation is a device whose memory is too low to serve its own web page,
// which is precisely when the WiFi route to these controls stops existing.
void host_set_poll_override(uint32_t ms) { g_pollOverrideMs = ms; }
uint32_t host_get_poll_override() { return g_pollOverrideMs; }
float                 g_requeryKm = 0.0f;
volatile bool         g_feedOk = true;                        // ADS-B feed healthy? (HUD warning)
// Flight Tracker is the only consumer of ADS-B data, but adsb_task used to poll it
// every 2s regardless of which app was on screen. Each poll opens a fresh TLS
// session (WiFiClientSecure), and mbedTLS's handshake buffers come from internal
// RAM, not PSRAM — dozens of alloc/free cycles a minute fragment that heap even
// while the user is sitting on Clock or the menu. Set true only while Flight
// Tracker is the entered app (radar_show_home_custom / radar_exit_release_style).
volatile bool         g_radarViewActive = false;
volatile uint32_t     g_lastFeedOkMs = 0;                     // millis() of the last good poll (HUD staleness)

volatile uint32_t     g_rebootAtMs = 0;
// Files written by the browser install page this session. Was a static local inside the
// upload handler; /installed needs to read it to report the count and reset it for the
// next install, so it lives out here now.
int g_sdUpCount = 0;
// /theme?slug=... — applied from loop() rather than the request handler, because
// theme_select::set() reboots and would cut the HTTP reply off mid-flight.
String                g_pendingSlug;
volatile uint32_t     g_applySlugAtMs = 0;                       // !=0: reboot when millis() reaches it (clean start after WiFi config)
String                g_tz = TZ_STR;                          // POSIX timezone (web-configurable, NVS); applied via configTzTime
// The name of wherever the Orb is, as the person who set it would say it ("Leeds, Utah").
// Beside g_tz because it is loaded in the same block and for the same reason: both are what
// a position MEANS rather than the position itself. Empty until something names it, which is
// the honest state for a device handed bare coordinates. THEME_CAPS 54.
String                g_locName;
volatile bool         g_weatherDirty = false;
volatile bool         g_wxRadarDirty = false;
volatile bool         g_wxAnimDirty = false;      // new Weather app: frame set ready
volatile bool         g_cloudImageDirty = false;
volatile bool         g_intelDirty = false;      // headlines: a fresh set is in the store
volatile bool         g_tickerDirty = false;     // quotes: a fresh set is in the store
