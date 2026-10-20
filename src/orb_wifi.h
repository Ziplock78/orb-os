// The Orb OS — WiFi. See orb_wifi.cpp.
#pragma once
#include <stddef.h>

void wifi_boot_connect(bool &wantWifiSetup);   // setup(): stored network, or the setup portal
void wifi_mdns_tick();                         // loop(): mDNS (and OTA) once connected

// The join Orb Studio asks for over the cable (orb_link hooks), and its per-loop tick.
bool serial_wifi_join(const char *ssid, const char *pass);
int  serial_wifi_join_status(const char **why);
void serial_wifi_join_tick();

// Also declared by settings_view.cpp, which drives the on-device join.
void host_wifi_saved_ssid(char *out, size_t n);
int  host_wifi_connect_status();
void host_wifi_connected_reboot();
