#pragma once
// Every network the Orb has actually been on, so one carried between two places finds the
// second on its own instead of sitting on "No WiFi" until somebody opens Settings.
//
// UX-025 already says an Orb joins the network it is near and shows that place's sky, and
// UX-021 says setup values survive. Holding one network meant the second of those was only
// true for whichever network was set up last.
//
// The list rules live in wifi_store_list.h, apart from the storage, so they can be tested
// on a host compiler.

#include "wifi_store_list.h"

namespace wifi_store {

// Record the network the radio is associated with RIGHT NOW.
//
// Reads the SSID and password back out of the driver rather than taking them as arguments,
// so there is one kind of truth here: a network is remembered because the Orb is on it,
// never because a caller believed it was about to be. That distinction is the whole of the
// two credential faults main.cpp records, both of which wrote an intention to flash and
// then failed to associate.
void remember_current();

int  count();
bool at(int i, char *ssid, size_t ssidCap, char *pass, size_t passCap);

// The stored password for a network seen in a scan, or false if it is not one of ours.
bool password_for(const char *ssid, char *out, size_t cap);

bool forget(const char *ssid);
void forget_all();

} // namespace wifi_store
