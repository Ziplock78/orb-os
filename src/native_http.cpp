#include "native_http.h"
#include <curl/curl.h>
#include <cstdio>

namespace {
    size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
        std::string *out = (std::string *)userdata;
        out->append(ptr, size * nmemb);
        return size * nmemb;
    }
}

bool native_https_get(const char *url, const char *userAgent, std::string &body, int timeoutMs) {
    body.clear();
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    if (userAgent) curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeoutMs);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3500L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);   // matches the device's client.setInsecure() (hobby project, public non-sensitive APIs)
    const CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK || code != 200) {
        fprintf(stderr, "[net] HTTP %ld (curl: %s) %s\n", code, curl_easy_strerror(res), url);
        return false;
    }
    return !body.empty();
}
