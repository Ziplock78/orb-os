// Run the production router using the main loop's event-only dispatch pattern.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include "input_router.h"
#include "app_shell.h"
#include "knob.h"
#include "knob_help.h"
#include "wind_notice.h"
#include "update_ui.h"

namespace {
uint32_t now = 0, rock_at = 0, menu_touch = 0;
bool menu = false;
int selection = 0, opens = 0, app_turns = 0;
void jig(uint32_t at) {
    now = at;
    if (menu) input_router::dispatch(0, true);
    rock_at = at;
    input_router::tick(); // net-zero jig; no turn or idle dispatch
    assert(menu && selection == 0);
}
void turn(uint32_t at, int delta) {
    now = at;
    input_router::dispatch(delta, false);
}
}

uint32_t lv_tick_get() { return now; }
namespace knob {
uint32_t lastRockMs() { return rock_at; }
uint32_t lastRockGapMs() { return 100; }
// Also let the previous router compile to reproduce its failed navigation.
int32_t rawPosition() { return selection; }
int32_t detentCount() { return selection; }
}
namespace knob_help {
bool showing() { return false; }
void show() {}
void dismiss() {}
}
namespace update_ui {
bool awaitingAck() { return false; }
void ackReady() {}
}
namespace wind_notice {
bool showing() { return false; }
void turn(int) {}
}
namespace app_shell {
bool browsing() { return menu; }
void openSwitcher() { menu = true; selection = 0; menu_touch = now; ++opens; }
void browseTurn(int delta) { selection += delta; menu_touch = now; }
void browsePress() { menu = false; }
void turnCurrent(int delta) { app_turns += delta; }
bool pressCurrent() { return true; }
int count() { return 6; }
}

int main() {
    jig(1000);
    turn(1100, -3); // absorb immediate gesture movement
    assert(selection == 0);
    turn(1349, -1);
    assert(selection == 0);
    turn(1350, 1); // deadline expires without any idle input dispatch
    assert(selection == 1);
    for (uint32_t at : {1800U, 2300U, 2800U, 3300U}) {
        assert(at - menu_touch < 2000); // slow turns keep the menu active
        turn(at, 1);
    }
    assert(selection == 5 && menu_touch == 3300);
    turn(3900, -1); // a reversal must not restart a two-tick confirmation
    assert(selection == 4);

    jig(5000);
    for (uint32_t at : {5050U, 5150U, 5250U}) turn(at, 2);
    rock_at = 5300;
    turn(5300, -2);
    assert(selection == 0);
    const int previous_opens = opens;
    turn(5350, -1); // ongoing movement cannot extend the deadline
    assert(selection == -1 && opens == previous_opens);
    rock_at = 5500;
    turn(5500, 1); // a reversal while browsing navigates without reopening
    assert(selection == 0 && opens == previous_opens);

    jig(7000);
    turn(7900, 1); // deliver the first turn after a delayed frame
    assert(selection == 1);
    turn(8500, 3); // preserve batched turns in full
    assert(selection == 4);
    input_router::dispatch(0, true);
    assert(!menu);
    turn(8600, 1);
    assert(app_turns == 1);

    jig(0xFFFFFF00U);
    turn(0xFFFFFFFFU, 1);
    assert(selection == 0);
    turn(94, -1); // 350 ms across timer wrap
    assert(selection == -1);
    std::puts("Orb router tests passed: bounded jig tail, slow turns, reversals, delayed polling, batched turns, press and timer wrap");
}
