#include "diag_log.h"
#include <esp_system.h>
#include <esp_idf_version.h>
#include <string.h>

namespace {
    constexpr uint32_t MAGIC       = 0xD1A9C0DEu;
    constexpr int       MAX_EVENTS = 40;
    constexpr int       MSG_LEN    = 48;
    struct Event { uint32_t ms; char msg[MSG_LEN]; };

    const char *reset_reason_str(esp_reset_reason_t r) {
        switch (r) {
            case ESP_RST_POWERON:   return "power-on";
            case ESP_RST_SW:        return "software (restart)";
            case ESP_RST_PANIC:     return "PANIC / crash";
            case ESP_RST_INT_WDT:   return "interrupt watchdog";
            case ESP_RST_TASK_WDT:  return "task watchdog";
            case ESP_RST_WDT:       return "other watchdog";
            case ESP_RST_BROWNOUT:  return "brownout (power dip)";
            case ESP_RST_DEEPSLEEP: return "deep sleep wake";
            case ESP_RST_EXT:       return "external reset pin";
            case ESP_RST_SDIO:      return "SDIO";
            // The ones that were hiding in "unknown", and the reason this list grew.
            //
            // /diag recorded three boots on 2026-10-06 and reported two of them as
            // "unknown" with the heap in perfect health at the time (57 KB free, 52 KB
            // minimum) — so not a crash and not memory, and the word "unknown" was the
            // whole of what anybody had to go on. On an ESP32-S3 whose Serial IS the native
            // USB peripheral, a host opening the port and asserting DTR/RTS can reset the
            // chip: that is ESP_RST_USB, and it is exactly how esptool enters download mode
            // without anybody touching a button. A serial monitor attaching therefore looks
            // identical to a spontaneous reboot unless this list can name it.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
            case ESP_RST_USB:       return "USB peripheral (host opened the port)";
            case ESP_RST_JTAG:      return "JTAG";
            case ESP_RST_EFUSE:     return "efuse error";
            case ESP_RST_PWR_GLITCH:return "power glitch";
            case ESP_RST_CPU_LOCKUP:return "CPU lockup";
#endif
            default:                return "unknown";
        }
    }
}

// RTC_NOINIT_ATTR memory keeps its contents across ESP.restart()/panic/watchdog
// resets (the whole point here), so these must NOT have initializers — the compiler
// enforces that, since "no-init" is the mechanism that lets them survive.
RTC_NOINIT_ATTR static uint32_t s_magic;
RTC_NOINIT_ATTR static uint32_t s_bootCount;
RTC_NOINIT_ATTR static int      s_head;
RTC_NOINIT_ATTR static int      s_count;
RTC_NOINIT_ATTR static Event    s_buf[MAX_EVENTS];

void diag::log(const char *fmt, ...) {
    Event &e = s_buf[s_head];
    e.ms = millis();
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, MSG_LEN, fmt, ap);
    va_end(ap);
    s_head = (s_head + 1) % MAX_EVENTS;
    if (s_count < MAX_EVENTS) s_count++;
}

void diag::boot() {
    const esp_reset_reason_t reason = esp_reset_reason();
    if (s_magic != MAGIC) {                     // true power-on / first flash: buffer is garbage
        s_magic = MAGIC;
        s_bootCount = 0;
        s_head = 0;
        s_count = 0;
        memset(s_buf, 0, sizeof(s_buf));
    }
    s_bootCount++;

    Serial.printf("[diag] boot #%lu, reset reason: %s\n", (unsigned long)s_bootCount, reset_reason_str(reason));
    Serial.println("[diag] recent history (spans the last reboot):");
    const int start = (s_count < MAX_EVENTS) ? 0 : s_head;
    for (int i = 0; i < s_count; ++i) {
        const Event &e = s_buf[(start + i) % MAX_EVENTS];
        Serial.printf("  t+%lums  %s\n", (unsigned long)e.ms, e.msg);
    }

    log("---- boot #%lu (%s) ----", (unsigned long)s_bootCount, reset_reason_str(reason));
}

String diag::text() {
    String out;
    out.reserve((size_t)s_count * 56 + 64);
    const int start = (s_count < MAX_EVENTS) ? 0 : s_head;
    for (int i = 0; i < s_count; ++i) {
        const Event &e = s_buf[(start + i) % MAX_EVENTS];
        out += "t+"; out += e.ms; out += "ms  "; out += e.msg; out += "\n";
    }
    return out;
}

const char *diag::resetReasonText() { return reset_reason_str(esp_reset_reason()); }
