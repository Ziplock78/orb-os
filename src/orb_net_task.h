// The Orb OS — the networking task (core 0). See orb_net_task.cpp.
#pragma once

// FreeRTOS task body: WiFi keepalive, the ADS-B poll, weather and headline fetches, and
// the on-demand route/photo lookups. Never touches the display. Pinned to core 0 by setup().
void adsb_task(void *);
