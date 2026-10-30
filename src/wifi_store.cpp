#include "wifi_store.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

namespace {

// The same namespace the credential backup in main.cpp already uses, and for the same
// reason its comment there gives: nothing in the WiFi stack touches "capsuleradar", so what
// is kept in it survives whatever WiFi.begin() decides to do to the driver's own
// nvs.net80211. That is the property this list depends on completely.
const char *NS  = "capsuleradar";

// One blob rather than a key per field. A list that is read and written whole can never be
// caught half-updated by a power cut in the middle of setup, which is exactly when somebody
// is most likely to pull the plug.
const char *KEY = "wifiNets";

int load(wifi_store::Entry *list) {
    memset(list, 0, sizeof(wifi_store::Entry) * wifi_store::MAX_NETWORKS);
    Preferences p;
    if (!p.begin(NS, true)) return 0;
    const size_t have = p.getBytesLength(KEY);
    int n = 0;
    // A blob that is not a whole number of entries is from a different build of this
    // struct. Reading it would hand out garbled credentials, so it is treated as absent and
    // the next successful connection writes a fresh one.
    if (have > 0 && have % sizeof(wifi_store::Entry) == 0) {
        n = (int)(have / sizeof(wifi_store::Entry));
        if (n > wifi_store::MAX_NETWORKS) n = wifi_store::MAX_NETWORKS;
        p.getBytes(KEY, list, (size_t)n * sizeof(wifi_store::Entry));
    }
    p.end();
    return n;
}

void save(const wifi_store::Entry *list, int n) {
    Preferences p;
    if (!p.begin(NS, false)) return;
    if (n <= 0) p.remove(KEY);
    else        p.putBytes(KEY, list, (size_t)n * sizeof(wifi_store::Entry));
    p.end();
}

} // namespace

namespace wifi_store {

void remember_current() {
    if (WiFi.status() != WL_CONNECTED) return;
    wifi_config_t cur = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &cur) != ESP_OK) return;
    if (cur.sta.ssid[0] == '\0') return;

    const char *ssid = (const char *)cur.sta.ssid;
    const char *pass = (const char *)cur.sta.password;

    Entry list[MAX_NETWORKS];
    int n = load(list);

    // Already at the front with the same password: nothing to say. Worth the check because
    // this runs on every association, and the usual day is the same network coming back
    // after a blip. Without it each reconnect would cost an NVS erase and rewrite for no
    // change in content, and flash wear is the one cost here that cannot be undone.
    if (n > 0 && strncmp(list[0].ssid, ssid, SSID_LEN) == 0
              && strncmp(list[0].pass, pass, PASS_LEN) == 0) return;

    n = promote(list, n, ssid, pass);
    save(list, n);
    Serial.printf("[wifi] remembered '%s' (%d network%s saved)\n", ssid, n, n == 1 ? "" : "s");
}

int count() {
    Entry list[MAX_NETWORKS];
    return load(list);
}

bool at(int i, char *ssid, size_t ssidCap, char *pass, size_t passCap) {
    Entry list[MAX_NETWORKS];
    const int n = load(list);
    if (i < 0 || i >= n) return false;
    if (ssid) copy_field(ssid, ssidCap, list[i].ssid);
    if (pass) copy_field(pass, passCap, list[i].pass);
    return true;
}

bool password_for(const char *ssid, char *out, size_t cap) {
    Entry list[MAX_NETWORKS];
    const int n = load(list);
    const int at = find(list, n, ssid);
    if (at < 0) return false;
    copy_field(out, cap, list[at].pass);
    return true;
}

bool forget(const char *ssid) {
    Entry list[MAX_NETWORKS];
    const int n = load(list);
    const int left = drop(list, n, ssid);
    if (left == n) return false;
    save(list, left);
    Serial.printf("[wifi] forgot '%s' (%d left)\n", ssid ? ssid : "", left);
    return true;
}

void forget_all() {
    save(nullptr, 0);
    Serial.println("[wifi] saved networks cleared");
}

} // namespace wifi_store
