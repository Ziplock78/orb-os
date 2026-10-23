#pragma once
// Look up an aircraft photo by ICAO hex via planespotters.net, download the
// thumbnail and decode it into photo_buffer(). Device-only (WiFi + JPEG decode).
bool photo_fetch(const char *hex, const char *type = nullptr);
