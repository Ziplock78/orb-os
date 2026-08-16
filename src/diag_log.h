#pragma once
// Small crash/event diagnostic ring buffer, kept in RTC memory so it survives a
// software reboot (ESP.restart(), a watchdog reset, a panic) — but NOT a true power
// loss (unplug/replug), which clears RTC memory. Call diag::log() at points worth
// remembering; diag::boot() prints the trailing history (spanning the reboot) to
// Serial and diag::text() serves the same thing over the web (see /diag).
#ifdef ARDUINO
#include <Arduino.h>
namespace diag {
    void   boot();                       // call once, first thing in setup() (after Serial.begin)
    void   log(const char *fmt, ...);    // printf-style, truncated to fit one entry
    String text();                       // the whole ring buffer as plain text
}
#else
// Desktop/native build: no RTC memory, no Serial-backed ring buffer. Just print,
// so the same call sites work unmodified in the LVGL simulator.
#include <cstdio>
#include <cstdarg>
namespace diag {
    inline void boot() {}
    inline void log(const char *fmt, ...) {
        va_list a; va_start(a, fmt); vprintf(fmt, a); putchar('\n'); va_end(a);
    }
    inline const char *text() { return ""; }
}
#endif
