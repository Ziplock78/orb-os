// The Orb OS — where the Orb is. See orb_location.cpp.
#pragma once

void host_locate_if_unset();   // one IP lookup per boot for a never-located device; no restart
bool host_locate_current();    // IP lookup, save, and restart; false (no restart) if it failed
void host_set_location(double lat, double lon);   // save and restart, with a countdown
void host_recents_add(const char *name, double lat, double lon);
