#pragma once
#include <cstddef>
#include <cstdint>

namespace roads_download {
// Transport supplies nonblocking available(), read(), connected(), finished(),
// failed(), cancelled(). Clock supplies now(), micros() and pause(). Kept portable so the
// exact retry/deadline behavior can be tested without a live TLS socket.
template<class Transport, class Clock> class BufferedInput {
    Transport &source;
    Clock &clock;
    uint32_t started, lastProgress;
    uint32_t idleLimit, totalLimit, byteLimit;
    uint8_t buffer[1024];
    size_t position = 0, size = 0;
public:
    uint32_t bytes = 0;
    uint64_t transportUs = 0, idleUs = 0;
    uint32_t readCalls = 0, emptyPolls = 0;
    const char *error = nullptr;
    BufferedInput(Transport &s, Clock &c, uint32_t idleMs = 10000,
                  uint32_t totalMs = 150000, uint32_t maxBytes = 8 * 1024 * 1024)
        : source(s), clock(c), started(c.now()), lastProgress(started),
          idleLimit(idleMs), totalLimit(totalMs), byteLimit(maxBytes) {}
private:
    bool refill() {
        while (position == size) {
            if (error) return false;
            if (source.cancelled()) { error = "map request cancelled"; return false; }
            if (clock.now() - started >= totalLimit) { error = "map body total timeout"; return false; }
            const int available = source.available(); // also advances chunk framing
            if (source.failed()) { error = "invalid HTTP chunk framing"; return false; }
            if (source.finished()) return false;
            if (bytes >= byteLimit) { error = "map response exceeds byte limit"; return false; }
            if (available > 0) {
                size_t want = (size_t)available < sizeof(buffer) ? (size_t)available : sizeof(buffer);
                if (want > byteLimit - bytes) want = byteLimit - bytes;
                ++readCalls;
                const int got = source.read(buffer, want);
                if (got > (int)want) { error = "invalid map transport read size"; return false; }
                if (got > 0) {
                    position = 0; size = (size_t)got; lastProgress = clock.now();
                    break;
                }
                // TLS WANT_READ/temporary no-data is NOT EOF. Retry under the
                // same idle/total deadlines instead of handing JSON a false EOF.
            }
            if (source.finished()) return false;
            if (!source.connected() && source.available() <= 0) {
                error = "map connection closed before response completed"; return false;
            }
            if (clock.now() - lastProgress >= idleLimit) { error = "map body idle timeout"; return false; }
            ++emptyPolls;
            const uint32_t waitStarted = clock.micros();
            clock.pause();
            idleUs += (uint32_t)(clock.micros() - waitStarted);
        }
        return true;
    }
public:
    int read() {
        if (position == size) {
            const uint32_t refillStarted = clock.micros();
            const bool filled = refill();
            transportUs += (uint32_t)(clock.micros() - refillStarted);
            if (!filled) return -1;
        }
        ++bytes;
        return buffer[position++];
    }
};
} // namespace roads_download
