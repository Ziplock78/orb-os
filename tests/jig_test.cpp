// Build twice with src/input_router.cpp and -Itests/knob_stubs -Isrc:
// default tests the device detector; -DJIG_SIM_TEST tests the simulator detector.
#include <assert.h>
#include <stdint.h>
#include <initializer_list>
#ifdef JIG_SIM_TEST
#include <thread>
#include <chrono>
#include "../src/sim_knob.cpp"
static void advance(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
static void turn(int detents) { simknob::injectTurn(detents); }
uint32_t lv_tick_get() { return sim_now_ms(); }
#else
static uint32_t now = 1000;
uint32_t millis() { return now; }
int64_t esp_timer_get_time() { return (int64_t)now * 1000; }
uint32_t lv_tick_get() { return now; }
bool orb_log_quiet() { return true; }
#include "../src/knob.cpp"
static void advance(uint32_t ms) { now += ms; }
static void turn(int detents) {
    const int dir = detents > 0 ? 1 : -1;
    for (int k = 0; k < (detents > 0 ? detents : -detents); ++k) on_detent(dir);
    knob::poll();
}
#endif
#include "input_router.h"

static bool browsing, winding;
static int opens, appTurns, browseTurns, presses;
namespace app_shell {
bool browsing() { return ::browsing; }
void openSwitcher() { ++opens; ::browsing = true; }
void browseTurn(int delta) { browseTurns += delta; }
void browsePress() { ++presses; }
void turnCurrent(int delta) { appTurns += delta; }
bool pressCurrent() { ++presses; return true; }
int count() { return 4; }
}
namespace update_ui {
bool awaitingAck() { return false; }
void ackReady() {}
}
namespace knob_help {
bool showing() { return false; }
void dismiss() {}
void show() {}
}
namespace wind_notice {
bool showing() { return winding; }
void turn(int delta) { appTurns += delta; }
}
static void pump() {
    input_router::dispatch(knob::takeDelta(), false);
    input_router::tick();
}
static void jig(int first, int back) {
    browsing = false;
    advance(500);
    pump(); // Observe quiet before beginning a new gesture.
    const int before = opens;
    turn(first); pump();
    assert(opens == before);
    advance(70);
    turn(back);
    const int appBefore = appTurns;
    pump();
    assert(opens == before + 1 && appTurns == appBefore);
    input_router::tick(); pump();
    assert(opens == before + 1); // A reversal fires once, with no settle delay.
}
int main() {
    jig(1, -1);
    jig(-1, 1);
    jig(12, -20);
    jig(-30, 40);
    jig(100, -100);

    // The return stroke is swallowed until an input-free poll confirms 350 ms of quiet.
    const int before = opens;
    advance(70); turn(25); pump();
    assert(opens == before && browseTurns == 0);
    advance(150); turn(-30); pump();
    assert(opens == before && browseTurns == 0);
    advance(150); turn(20); pump();
    assert(opens == before && browseTurns == 0);
    advance(360); pump();
    turn(25); pump();
    assert(opens == before && browseTurns == 24); // First tick of the batch absorbed.

    // Once navigation is enabled, quick reversals browse rather than reopen.
    advance(70); turn(-10); pump();
    assert(opens == before && browseTurns == 14);

    // Buffered movement after a slow render must not escape through an expired timer.
    jig(-12, 15);
    const int browseBeforeStall = browseTurns;
    advance(600); turn(20); pump();
    assert(browseTurns == browseBeforeStall);
    advance(349); pump();
    turn(-10); pump();
    assert(browseTurns == browseBeforeStall);
    advance(350); pump(); // Release on an idle poll, then accept deliberate navigation.
    turn(3); pump();
    assert(browseTurns == browseBeforeStall + 2);

    // A late isolated tick cannot shift the menu, even after draining has finished.
    jig(8, -8);
    advance(400); pump();
    const int beforeLateTick = browseTurns;
    turn(1); pump();
    assert(browseTurns == beforeLateTick);
    advance(400); pump();
    turn(1); pump(); // The old isolated tick has expired; start confirmation anew.
    assert(browseTurns == beforeLateTick);
    advance(70); turn(-1); pump(); // Reversal also starts confirmation anew.
    assert(browseTurns == beforeLateTick);
    advance(70); turn(-1); pump();
    assert(browseTurns == beforeLateTick - 1);
    advance(70); turn(1); pump(); // Normal one-tick navigation after confirmation.
    assert(browseTurns == beforeLateTick);

    const int afterStall = opens;
    browsing = false;
    advance(500); turn(10); pump();
    advance(500); turn(-10); pump();
    assert(opens == afterStall); // An unhurried reversal remains ordinary app input.

    // Both halves can happen between polls and cancel to zero.
    advance(500); turn(-15);
    advance(70); turn(15);
    pump();
    assert(opens == afterStall + 1);
    winding = true;
    jig(-8, 9);
    winding = false;

#ifndef JIG_SIM_TEST
    // Exact timing boundaries and wraparound, using the device's ISR clock.
    for (uint32_t gap : {44u, 45u, 250u, 251u}) {
        browsing = false;
        advance(500); pump(); turn(8); pump();
        const int start = opens;
        advance(gap); turn(-8); pump();
        assert(opens == start + (gap >= 45 && gap <= 250 ? 1 : 0));
    }
    browsing = false;
    now = UINT32_MAX - 39;
    pump();
    turn(-10); pump();
    const int start = opens;
    advance(70); turn(10); pump();
    assert(opens == start + 1);

    // The quiet period also survives millisecond rollover.
    browsing = false;
    now = UINT32_MAX - 170;
    pump();
    turn(-10); pump();
    advance(70); turn(10); pump();
    const int browseBefore = browseTurns;
    advance(150); turn(-10); pump();
    assert(browseTurns == browseBefore);
    advance(349); pump();
    advance(1); pump();
    turn(-3); pump();
    assert(browseTurns == browseBefore - 2);
#endif
    assert(presses == 0);
}
