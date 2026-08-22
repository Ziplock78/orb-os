#pragma once
// Fetches nearby aircraft from airplanes.live (fallback adsb.lol) and parses
// the readsb JSON into a vector<Aircraft>. See docs/DATA_SOURCE.md.
#include <vector>
#include "aircraft.h"
#ifdef ARDUINO
#include <WiFiClient.h>
#include <HTTPClient.h>
#endif

class AdsbClient {
public:
    void begin(double homeLat, double homeLon, float rangeKm);
    void setHome(double lat, double lon) { _lat = lat; _lon = lon; }
    void setRange(float km) { _rangeKm = km; }
    void setHideGround(bool h) { _hideGround = h; }   // skip on-ground aircraft during parse
    void setMinAltFt(float ft) { _minAltFt = ft; }    // skip aircraft below this altitude (0 = off)
    void setMinDistKm(float km) { _minDistKm = km; }  // skip aircraft closer than this to home (0 = off)
    void setMilitaryOnly(bool m) { _milOnly = m; }    // keep only military-flagged aircraft

    // Fetch + parse. Returns true on success and fills `out` (replaces contents).
    // On failure, leaves `out` untouched and returns false (caller keeps last good).
    bool poll(std::vector<Aircraft>& out);

    uint32_t lastOkMs() const { return _lastOkMs; }

    // True when the last poll failed because a SERVER said no (any 4xx), rather than because
    // this board could not make the request. The difference matters a great deal: a refusal
    // is a policy answer that more attempts cannot change, while a local failure is the
    // fragmented-heap case that a reboot really does clear. Treating the two the same is
    // what had the Orb rebooting itself every three minutes against a feed that was
    // rate-limiting it, which is the one response guaranteed to make a rate limit worse.
    bool lastWasRefused() const { return _refused; }
    int  lastStatus() const { return _lastStatus; }
    // The 4xx that caused the refusal, which is NOT lastStatus(): poll() tries the primary
    // and then the fallback, so the last status belongs to whichever host failed second. The
    // message said "refused (HTTP -1)" while the actual refusal was a 403 from the host
    // before it.
    int  refusedStatus() const { return _refusedStatus; }

private:
    // tls picks the transport per host: see the ADSB_*_TLS notes in config.h for why the
    // primary deliberately runs over plain HTTP on this board.
    bool fetchFrom(const char* host, bool tls, std::vector<Aircraft>& out);   // one host, one attempt

    bool   _refused = false;
    int    _lastStatus = 0;
    int    _refusedStatus = 0;

#ifdef ARDUINO
    // THE SOCKET AND THE HTTP CLIENT PERSIST ACROSS POLLS. Both used to be stack locals
    // rebuilt from nothing on every fetchFrom(), i.e. every 5 seconds forever: a fresh TCP
    // socket opened, a full request/response, then a complete teardown, all so the next poll
    // five seconds later could do it again to the same host.
    //
    // Keeping them as members does two separate things. The objects themselves stop being
    // constructed and destroyed on a loop, which is the same class of churn that the
    // WiFiClientSecure fix removed. And because the WiFiClient outlives the request, the
    // TCP connection underneath it can be kept alive between polls: HTTPClient::connect()
    // reuses an already-connected client instead of dialling again, and end() leaves the
    // socket open when reuse is on and the server agreed to keep-alive.
    //
    // Safe if the server declines: HTTPClient tracks whether the response actually permitted
    // reuse (_canReuse), and closes the socket when it did not. That degrades to exactly the
    // old connect-every-time behaviour rather than breaking.
    WiFiClient _plain;
    HTTPClient _http;
#endif

    double _lat = 0, _lon = 0;
    float  _rangeKm = 15.0f;
    bool   _hideGround = false;
    float  _minAltFt = 0.0f;
    float  _minDistKm = 0.0f;
    bool   _milOnly = false;
    uint32_t _lastOkMs = 0;
};
