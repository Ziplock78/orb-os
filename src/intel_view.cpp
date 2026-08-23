#include "intel_view.h"
#include "intel.h"
#include "intel_client.h"
#include "config.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#else
#include <cstdio>
#include <cstdarg>
#include <chrono>
static struct {
    void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
    void println(const char *s) const { std::printf("%s\n", s); }
} Serial;
static uint32_t millis() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
#endif
#include <stdio.h>
#include <string.h>

// The Intel screen: a title, three headlines, and how old they are.
//
// The whole design rule here is that this is read from across a room, in passing. It is not
// a news reader and there is nothing to scroll. Three headlines is the default because
// three fit on a 466 px circle at a size that can be read standing up; five is available
// for people who would rather have density than size, and is the most the gateway sends.
namespace {

// A circle is a hostile place for text. At the vertical centre the dial is 466 px wide, but
// a line sitting 150 px above the middle only has about 350 px of glass under it, and text
// that overruns is clipped by the bezel rather than wrapped. Every headline is therefore
// laid out inside this width, which is the chord at the topmost line's height with a margin
// for the rounding, and LVGL wraps within it.
constexpr int TEXT_W       = 330;
constexpr int TITLE_Y      = -168;
constexpr int FIRST_ROW_Y  = -74;
constexpr int ROW_STEP_5   = 52;   // five headlines: tighter, smaller type
constexpr int ROW_STEP_3   = 74;   // three headlines: room for two wrapped lines each
constexpr int AGE_Y        = 176;

const lv_color_t C_TITLE  = lv_color_hex(0x7E8794);
const lv_color_t C_TEXT   = lv_color_hex(0xE8ECF1);
const lv_color_t C_SOURCE = lv_color_hex(0x5F6874);
const lv_color_t C_STALE  = lv_color_hex(0xC8922E);

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_title  = nullptr;
lv_obj_t *s_age    = nullptr;
lv_obj_t *s_rows[INTEL_MAX_ITEMS]   = {};
lv_obj_t *s_credit[INTEL_MAX_ITEMS] = {};
lv_obj_t *s_empty  = nullptr;       // the one line shown when there is nothing to show

uint32_t s_lastTryMs   = 0;
bool     s_everFetched = false;

void show(lv_obj_t *o, bool on) {
    if (!o) return;
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Say which thing is unwell, the same way the scope does. From the desk an empty Intel
// screen looks identical whether the WiFi dropped, the gateway is down, or it simply has
// not asked yet, and the device knows which it is.
const char *empty_reason() {
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) return "No WiFi\nYour Orb is fine";
#endif
    // Three full stops, not "…": Montserrat has no glyph at U+2026 and draws an empty box
    // for it. Same reason the gateway sends ASCII in the headlines themselves.
    if (!s_everFetched)                return "Getting the headlines...";
    return "Headlines unavailable\nWiFi is fine, the service is not answering";
}

void layout(int count) {
    const bool dense = count > 3;
    const int step   = dense ? ROW_STEP_5 : ROW_STEP_3;
    // Centre the block: an odd count sits symmetrically, an even one straddles the middle.
    const int top = dense ? FIRST_ROW_Y - 16 : FIRST_ROW_Y + ((3 - count) * step) / 2;
    for (int i = 0; i < INTEL_MAX_ITEMS; ++i) {
        if (!s_rows[i]) continue;
        lv_obj_set_style_text_font(s_rows[i], dense ? &lv_font_montserrat_14
                                                    : &lv_font_montserrat_16, 0);
        lv_obj_align(s_rows[i], LV_ALIGN_CENTER, 0, top + i * step);
        // The credit rides just under its own headline. Aligning it to the row rather than
        // to the screen keeps it attached when a headline wraps to two lines.
        lv_obj_align_to(s_credit[i], s_rows[i], LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    }
}

} // namespace

lv_obj_t *intelview::screen() { return s_screen; }

// A knob press asks now instead of waiting out the poll. The fetch itself belongs to the
// network task, so this only clears the timer that task is watching.
void intelview::onPress() {
    s_lastTryMs = 0;
    Serial.println("[intel] refresh requested from the knob");
}

// ---- network step (core 0) --------------------------------------------------
// Called from the same task that polls aircraft. One request, then it is done: this is a
// sub-kilobyte fetch against a cache, so there is no burst to spread out the way the
// weather tiles need.
bool intelview::fetchStep() {
    const uint32_t now = millis();
    if (s_lastTryMs != 0) {
        IntelSnapshot cur;
        const bool have = intel_get(cur) && cur.valid;
        const uint32_t due = have ? INTEL_POLL_MS : INTEL_RETRY_MS;
        if (now - s_lastTryMs < due) return false;
    }
    s_lastTryMs = now ? now : 1;   // never leave it at 0, which means "ask immediately"

    IntelSnapshot snap;
    if (!intel_fetch(INTEL_DEFAULT_TOPICS, INTEL_DEFAULT_COUNT, snap)) return false;
    intel_store(snap);
    s_everFetched = true;
    return true;
}

// ---- UI (core 1) ------------------------------------------------------------
void intelview::onHeadlinesReady() {
    IntelSnapshot s;
    const bool have = intel_get(s) && s.valid && s.count > 0;
    show(s_empty, !have);
    if (!have) {
        lv_label_set_text(s_empty, empty_reason());
        for (int i = 0; i < INTEL_MAX_ITEMS; ++i) { show(s_rows[i], false); show(s_credit[i], false); }
        lv_label_set_text(s_age, "");
        return;
    }
    for (int i = 0; i < INTEL_MAX_ITEMS; ++i) {
        const bool on = i < s.count;
        show(s_rows[i], on);
        show(s_credit[i], on);
        if (!on) continue;
        lv_label_set_text(s_rows[i], s.items[i].text);
        lv_label_set_text(s_credit[i], s.items[i].source);
    }
    layout(s.count);
    intelview::tick();
}

// The age line, refreshed on a timer rather than only on arrival: headlines that stopped
// updating should look stale, not current. Ten minutes is the poll, so anything past about
// twenty means something is wrong and the colour says so.
void intelview::tick() {
    if (!s_age) return;
    IntelSnapshot s;
    if (!intel_get(s) || !s.valid || s.count == 0) { lv_label_set_text(s_age, ""); return; }
    const uint32_t ageS = (millis() - s.fetchedMs) / 1000;
    char buf[32];
    if (ageS < 90)        snprintf(buf, sizeof(buf), "just now");
    else if (ageS < 3600) snprintf(buf, sizeof(buf), "%lu min ago", (unsigned long)(ageS / 60));
    else                  snprintf(buf, sizeof(buf), "%lu hr ago", (unsigned long)(ageS / 3600));
    lv_label_set_text(s_age, buf);
    lv_obj_set_style_text_color(s_age, ageS > 20 * 60 ? C_STALE : C_SOURCE, 0);
}

static void tick_cb(lv_timer_t * /*t*/) {
    if (lv_scr_act() != intelview::screen()) return;
    intelview::tick();
}

void intelview::init() {
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_screen);
    lv_label_set_text(s_title, "INTEL");
    lv_obj_set_style_text_color(s_title, C_TITLE, 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
    // Letter-spaced, because a five-character word in small caps at the top of a dial reads
    // as a label rather than as another headline.
    lv_obj_set_style_text_letter_space(s_title, 4, 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, TITLE_Y);

    for (int i = 0; i < INTEL_MAX_ITEMS; ++i) {
        s_rows[i] = lv_label_create(s_screen);
        lv_label_set_long_mode(s_rows[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_rows[i], TEXT_W);
        lv_obj_set_style_text_align(s_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_rows[i], C_TEXT, 0);
        lv_obj_set_style_text_font(s_rows[i], &lv_font_montserrat_16, 0);
        lv_label_set_text(s_rows[i], "");
        show(s_rows[i], false);

        s_credit[i] = lv_label_create(s_screen);
        lv_obj_set_style_text_color(s_credit[i], C_SOURCE, 0);
        lv_obj_set_style_text_font(s_credit[i], &lv_font_montserrat_12, 0);
        lv_label_set_text(s_credit[i], "");
        show(s_credit[i], false);
    }

    s_empty = lv_label_create(s_screen);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_empty, TEXT_W);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_empty, C_SOURCE, 0);
    lv_obj_set_style_text_font(s_empty, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_empty, empty_reason());
    lv_obj_center(s_empty);

    s_age = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_age, C_SOURCE, 0);
    lv_obj_set_style_text_font(s_age, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_age, "");
    lv_obj_align(s_age, LV_ALIGN_CENTER, 0, AGE_Y);

    layout(INTEL_DEFAULT_COUNT);
    lv_timer_create(tick_cb, 30000, nullptr);
}
