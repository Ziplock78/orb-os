// The Orb OS — WiFi: joining at boot (WiFiManager and its setup portal), joining a new
// network from Settings or over the cable without losing the old one, and bringing up
// mDNS once connected. Moved out of main.cpp unchanged.
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>
#include "config.h"
#include "orb_state.h"
#include "orb_wifi.h"
#include "orb_location.h"
#include "orb_stats.h"
#if ORB_OTA_ENABLED
#include <ArduinoOTA.h>
#endif

// --- On-device WiFi setup (Settings > WiFi) --------------------------------------
// Lets the user scan/select/enter-password entirely from the knob+touchscreen, no
// phone or captive portal needed. Scanning is async (WiFi.scanNetworks(true)) so it
// never blocks the UI thread; settings_view.cpp polls host_wifi_scan_result() from a
// timer. Connecting hands off cleanly from WiFiManager's non-blocking portal, then
// mirrors WiFiManager's own approach on success: reboot for a clean start, since this
// chip's WiFi/web/mDNS stack doesn't reliably hot-swap networks in place.
void host_wifi_scan_start() { WiFi.scanNetworks(true /*async*/); }

// Returns: -1 = scan still running, -2 = scan failed, >=0 = number of networks written.
// Deduplicates repeated SSIDs (multiple access points/bands) to the strongest signal.
int host_wifi_scan_result(char names[][33], int8_t *rssi, bool *isOpen, int maxN) {
    const int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return -1;
    if (n == WIFI_SCAN_FAILED || n < 0) return -2;
    int count = 0;
    for (int i = 0; i < n && count < maxN; ++i) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        int dupIdx = -1;
        for (int j = 0; j < count; ++j) if (strcmp(names[j], ssid.c_str()) == 0) { dupIdx = j; break; }
        const int8_t r = (int8_t)WiFi.RSSI(i);
        if (dupIdx >= 0) { if (r > rssi[dupIdx]) rssi[dupIdx] = r; continue; }   // keep the stronger AP
        snprintf(names[count], 33, "%s", ssid.c_str());
        rssi[count] = r;
        isOpen[count] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        count++;
    }
    return count;
}

// TRY a network without committing to it. The credentials are a candidate until the Orb has
// actually associated; host_wifi_commit_credentials() below is what makes them permanent.
//
// WiFi.persistent() is the whole fix. Its default on this core is TRUE, so WiFi.begin()
// writes the SSID and password into the driver's own NVS (nvs.net80211) AT THE CALL, before
// a single packet is exchanged — on intent, with no fact behind it. Pressing OK on a
// neighbour's network with a wrong password therefore destroyed the working network the
// owner already had, and a power cycle brought the Orb back asking to be set up again. That
// is UX-021: setup, once completed, stays completed, and a mistyped password is a weaker
// event than the power loss and firmware updates that requirement already survives.
//
// This is the SECOND time this exact fault has shipped. main.cpp's own comment dated
// 2026-08-15 records the first: a secrets.h fallback called WiFi.begin() at boot, "it
// overwrote the user's real saved network on EVERY boot... the device came up in the config
// portal with its good credentials already gone." That call site was fixed and the lesson
// written down. This one reintroduced it from Settings, because the lesson lived in a
// comment beside the old caller rather than in the function every caller goes through.
// Copy whatever network is currently stored into OUR namespace, before anything is allowed
// to disturb it. Restored by host_wifi_restore_credentials() if the attempt fails.
//
// This exists because the previous fix did not work and cost its owner a second setup. That
// one set WiFi.persistent(false) before WiFi.begin(), on the understanding that begin()
// would then leave the stored config alone. Whatever the driver actually does, it is not
// that: a failed attempt against a neighbour's network still destroyed a working one.
//
// So this does not reason about the driver at all. capsuleradar is our namespace and nothing
// in the WiFi stack touches it, so a copy kept there survives whatever begin() decides to do
// to nvs.net80211. The password is written alongside the SSID; it already lives in plain
// form in the driver's own namespace on the same flash, so this moves no secret anywhere it
// was not already, and it is never logged.
static void wifi_backup_credentials() {
    wifi_config_t cur = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &cur) != ESP_OK) return;
    if (cur.sta.ssid[0] == '\0') return;          // nothing stored yet, nothing to protect
    Preferences p;
    p.begin("capsuleradar", false);
    p.putString("wifiBakSsid", (const char *)cur.sta.ssid);
    p.putString("wifiBakPass", (const char *)cur.sta.password);
    p.end();
    Serial.printf("[wifi] stored network '%s' backed up before the attempt\n",
                  (const char *)cur.sta.ssid);
}

// Put it back. Called when an attempt fails or times out, so a wrong password on somebody
// else's network can never cost the owner the network they had.
static void wifi_restore_credentials() {
    Preferences p;
    p.begin("capsuleradar", true);
    const String ssid = p.getString("wifiBakSsid", "");
    const String pass = p.getString("wifiBakPass", "");
    p.end();
    if (ssid.isEmpty()) return;
    WiFi.persistent(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("[wifi] attempt failed; restored '%s'\n", ssid.c_str());
}

static void wifi_clear_backup() {
    Preferences p;
    p.begin("capsuleradar", false);
    p.remove("wifiBakSsid");
    p.remove("wifiBakPass");
    p.end();
}

// The stored network's NAME, for ?orb wifisaved. Never the password: the point of the
// command is to let a failed attempt be tested without a reboot and without anybody having
// to read a secret off a screen or a serial line.
void host_wifi_saved_ssid(char *out, size_t n) {
    wifi_config_t cur = {};
    out[0] = '\0';
    if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK)
        snprintf(out, n, "%s", (const char *)cur.sta.ssid);
}

void host_wifi_restore_saved() { wifi_restore_credentials(); }
void host_wifi_forget_backup() { wifi_clear_backup(); }

void host_wifi_connect(const char *ssid, const char *pass) {
    if (g_wm.getConfigPortalActive()) g_wm.stopConfigPortal();   // hand off cleanly
    wifi_backup_credentials();   // the actual protection; see above
    WiFi.persistent(false);      // belt as well as braces, on the chance it does help
    WiFi.begin(ssid, pass);
}

// Now it is true, so now it is written. Called only once the association has actually
// succeeded, which is the fact the write was missing before.
//
// Re-begins with persistence on rather than reaching into the driver's NVS directly: the
// device is already associated with this exact network, so this is cheap, and it is the one
// path the Arduino core documents. The short wait afterwards is because a re-begin can drop
// the link for a moment, and the caller's very next act is either an HTTP location lookup or
// a reboot — neither of which wants to run through a reconnect.
void host_wifi_commit_credentials(const char *ssid, const char *pass) {
    wifi_clear_backup();     // it took, so there is nothing left to fall back to
    WiFi.persistent(true);
    WiFi.begin(ssid, pass);
    const uint32_t until = millis() + 3000;
    while (WiFi.status() != WL_CONNECTED && millis() < until) delay(50);
    Serial.printf("[wifi] credentials committed for %s (link %s)\n",
                  ssid, WiFi.status() == WL_CONNECTED ? "up" : "still settling");
}

// 0 = still connecting, 1 = connected, 2 = failed/rejected.
int host_wifi_connect_status() {
    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) return 1;
    if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) return 2;
    return 0;
}

// Called on a successful manual connect: reboot shortly after, mirroring
// g_wm.setSaveConfigCallback() — same reasoning (clean web/mDNS start). Also clears the
// post-Reset "needs setup" flag (see host_factory_reset()) so the next boot is normal.
void host_wifi_connected_reboot() {
    Preferences p;
    p.begin("capsuleradar", false);
    p.putBool("needsWifiSetup", false);
    p.end();
    g_rebootAtMs = millis() + 1500;
}

// ----------------------------- WiFi setup over the cable ------------------------
//
// The same join the Settings screen runs with the knob, driven from Orb Studio instead:
// the owner is sitting at a computer with the cable in, so the network name and password
// come off a keyboard. Same protection as the knob path: the candidate is tried with
// persistence off, the previous network is backed up first, and only an association that
// actually happened is written. On success the Orb finds its location and restarts, which
// is what the first-boot prompt does. Polled by orb_link's "wifi-join-status".
static bool     g_sjActive = false;
static uint32_t g_sjStartMs = 0;
static char     g_sjSsid[33] = "";
static char     g_sjPass[65] = "";
static int      g_sjResult = 0;   // 0 joining, 1 joined (restart coming), 2 failed
static char     g_sjWhy[48] = "";

bool serial_wifi_join(const char *ssid, const char *pass) {
    if (!ssid || !*ssid || strlen(ssid) > 32 || (pass && strlen(pass) > 64)) return false;
    if (g_sjActive) return false;
    snprintf(g_sjSsid, sizeof(g_sjSsid), "%s", ssid);
    snprintf(g_sjPass, sizeof(g_sjPass), "%s", pass ? pass : "");
    g_sjActive = true; g_sjResult = 0; g_sjWhy[0] = '\0';
    g_sjStartMs = millis();
    host_wifi_connect(g_sjSsid, g_sjPass);
    Serial.printf("[wifi] joining '%s' by request over the cable\n", g_sjSsid);
    return true;
}
int serial_wifi_join_status(const char **why) { *why = g_sjWhy; return g_sjActive ? 0 : g_sjResult; }

void serial_wifi_join_tick() {
    if (!g_sjActive) return;
    int st = host_wifi_connect_status();
    if (st == 0 && millis() - g_sjStartMs > 20000) st = 2;   // the knob path's own bound
    if (st == 0) return;
    g_sjActive = false;
    if (st == 1) {
        host_wifi_commit_credentials(g_sjSsid, g_sjPass);
        g_sjResult = 1;
        Serial.printf("[wifi] joined '%s' over the cable; locating, then restarting\n", g_sjSsid);
        // Location first, the way the first-boot prompt does it; that call restarts the
        // Orb itself on success and only returns when the lookup failed.
        if (!host_locate_current()) host_wifi_connected_reboot();
    } else {
        host_wifi_restore_saved();
        g_sjResult = 2;
        snprintf(g_sjWhy, sizeof(g_sjWhy), "could not join, check the password");
        Serial.printf("[wifi] join of '%s' failed; previous network restored\n", g_sjSsid);
    }
    g_sjPass[0] = '\0';   // never kept longer than the attempt
}

// Boot-time connect: everything setup() did between "Connecting to your network" and
// deciding whether WiFi setup is owed. Sets wantWifiSetup when there is no network at all.
void wifi_boot_connect(bool &wantWifiSetup) {
    // --- WiFi (captive portal, non-blocking) ------------------------------
    // First boot opens the "The Orb Setup" AP to enter WiFi creds. Non-blocking
    // so the radar keeps animating while you configure WiFi from your phone.
    g_wm.setConfigPortalBlocking(false);
    g_wm.setTitle("The Orb OS");
    // light phosphor-green theme for the captive portal (small CSS, injected into <head>)
    g_wm.setCustomHeadElement(
        "<style>"
        "body{background:#06100a;color:#cdd6d1;font-family:system-ui,sans-serif}"
        "h1,h2,h3{color:#1dff86}"
        "button,input[type=submit],.btn{background:#1dff86!important;color:#04140b!important;"
        "border:0!important;border-radius:8px!important;font-weight:700}"
        "input,select{background:#0c1a12!important;color:#eafff3!important;"
        "border:1px solid #2a4a39!important;border-radius:8px!important}"
        "a{color:#1dff86}.q{filter:hue-rotate(90deg)}"
        "</style>");
    // After the portal saves new credentials, reboot for a clean start: WiFiManager's
    // own port-80 server (and mDNS) don't cleanly hand over to our web server / STA
    // interface in non-blocking mode, so the config page is flaky until a fresh boot.
    g_wm.setSaveConfigCallback([]() {
        Serial.println("[wifi] new credentials saved -> rebooting for a clean web/mDNS start");
        g_rebootAtMs = millis() + 2500;   // let the portal deliver its 'saved' page first
    });
    // DISABLED on purpose (2026-08-15). A secrets.h fallback used to run here and it was
    // destructive: WiFi.SSID() is empty this early because the WiFi driver has not loaded
    // the stored credentials yet, so the guard was always true, and the WiFi.begin() below
    // it overwrote the user's real saved network on EVERY boot. When the fallback
    // credentials then failed to associate, the device came up in the config portal with
    // its good credentials already gone.
    //
    // If this comes back, it must (a) determine "no stored network" properly, which means
    // after WiFi.mode(WIFI_STA) or via WiFiManager's own getWiFiSSID(), and (b) never call
    // WiFi.begin() with fallback credentials before autoConnect() has had its turn.
    psram_mark("before wifi connect");
    // Belt as well as braces: ask the SDK to keep the link up on its own, in addition to the
    // explicit retry in adsb_task. This alone is not enough — it gives up after repeated
    // failures, which is exactly the case that stranded the device — but it recovers the
    // common brief blip without waiting for the 20 s retry.
    WiFi.setAutoReconnect(true);
    // WHAT IS STORED, read properly, before anything tries to use it. The driver has to be
    // up for esp_wifi_get_config() to answer (it loads the saved network from NVS as part
    // of coming up), and the return code has to be checked, because an unchecked call
    // leaves the buffer holding whatever the stack held. That unchecked read is what
    // WiFiManager's getWiFiSSID() does, and below it used to run AFTER a failed autoConnect
    // had opened the portal and switched the station side off, which is exactly when the
    // call fails. So a slow first association on a cold boot read back as "no network
    // stored" on some boots and as the right name on others, purely by what the stack
    // happened to hold: CanadianAvenger's 2.16.17 report, "WiFi not remembered" on one
    // power cycle and remembered on the next.
    WiFi.mode(WIFI_STA);
    char storedSsid[33] = {0};
    {
        wifi_config_t cur = {};
        if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK)
            snprintf(storedSsid, sizeof(storedSsid), "%s", (const char *)cur.sta.ssid);
    }
    const bool haveStoredNetwork = storedSsid[0] != '\0';
    Serial.printf("[wifi] stored network: %s\n", haveStoredNetwork ? storedSsid : "(none)");
    // NEVER OPEN THE PORTAL OVER A STORED NETWORK. When autoConnect fails it starts the
    // "The Orb Setup" access point, and WiFiManager turns the station side OFF to do it
    // (_disableSTAConn). Nothing ever turned it back on: the SDK's own reconnect has
    // nothing to reconnect, and WiFi.reconnect() from the network task is a no-op with the
    // station disabled. So "reconnecting in the background", which the log promised, was
    // never happening; the Orb sat in AP mode until somebody pulled the plug, and the
    // second power cycle was the one that "brought the WiFi back". With a network stored
    // the portal stays shut, the station side stays up, and a slow router costs a few
    // seconds of "No WiFi" instead of a night of it. The portal still opens when there is
    // nothing stored, or after Settings > Reset, which are the two times it is the answer.
    g_wm.setEnableConfigPortal(wantWifiSetup || !haveStoredNetwork);
    // BOUNDED. There was no connect timeout at all, so autoConnect blocked for the library
    // default while the screen above promised "up to twenty seconds" — measured at about
    // forty-five on the bench. Twelve is comfortably more than a healthy association needs
    // and is the number the boot screen can honestly stand behind. Missing it is no longer
    // costly (see above), so it is not worth stretching for a slow router.
    g_wm.setConnectTimeout(12);
    const bool wifiUp = g_wm.autoConnect("The Orb Setup");
    if (wifiUp)                          Serial.println("[wifi] connected");
    else if (g_wm.getConfigPortalActive()) Serial.println("[wifi] config portal open - join 'The Orb Setup' to set WiFi; UI stays live");
    else                                 Serial.printf("[wifi] '%s' did not come up in time; the station side stays on and keeps trying\n", storedSsid);

    // No network means the first thing anyone should see is how to give it one, not a
    // clock that cannot tell the time. Both routes are open from here: the knob walks
    // through scan/pick/password on the screen itself, and the same moment the portal is
    // up on "The Orb Setup" for anyone who would rather type on a phone.
    // "Did not connect just now" and "was never configured" are different things. An Orb
    // with a good saved network that was merely slow, or whose router was still waking up,
    // must not be sent to the setup screen and asked to configure something it already
    // knows. With a network stored and simply not up, the Flight Tracker's "No WiFi" banner
    // says so, the clock shows its face at twelve until the time arrives, and the SDK plus
    // the network task's 20 s retry keep working on it: report the state, keep trying, and
    // do not demand setup for something already set up.
    if (!wifiUp && !haveStoredNetwork) wantWifiSetup = true;
}

// Called every loop(). Brings up mDNS (and OTA, when compiled in) the first time WiFi is
// connected, and services OTA after that.
void wifi_mdns_tick() {
    // mDNS (and OTA, when it is compiled in): set up once WiFi is up.
    // ArduinoOTA::setHostname() used to be what registered the .local name, because it
    // calls MDNS.begin() internally. Compiling OTA out therefore took the device's whole
    // .local name with it and broke Launch Kit's pushes, which address it by name.
    // MDNS.begin() is now called here explicitly and does not depend on OTA at all.
    static bool mdnsUp = false;
    if (!mdnsUp && WiFi.status() == WL_CONNECTED) {
        if (!MDNS.begin(ORB_MDNS_HOST)) Serial.println("[mdns] begin failed");
        MDNS.addService("http", "tcp", 80);            // advertise the config web page
#if ORB_OTA_ENABLED
        ArduinoOTA.setHostname(ORB_MDNS_HOST);
        ArduinoOTA.begin();
        Serial.println("[ota] ready: pio run -e esp32-s3-amoled-175-ota -t upload");
#endif
        mdnsUp = true;
        Serial.println("[mdns] http://" ORB_MDNS_ADDR "/");
    }
#if ORB_OTA_ENABLED
    if (mdnsUp) ArduinoOTA.handle();
#endif
}
