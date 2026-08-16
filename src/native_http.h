#pragma once
#include <string>
// Desktop-simulator-only HTTPS GET (libcurl), standing in for WiFiClientSecure +
// HTTPClient in the native build of wx_radar_client.cpp / location_view.cpp. Not
// compiled into the device firmware — see platformio.ini's esp32 build_src_filter.
bool native_https_get(const char *url, const char *userAgent, std::string &body, int timeoutMs);
