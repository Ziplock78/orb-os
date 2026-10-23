#include "printsphere/dial_logic.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace printsphere::dial;
static void turn(Decoder& decoder, int direction, uint32_t now) {
  const uint8_t cw[] = {1, 0, 2, 3}, ccw[] = {2, 0, 1, 3};
  for (uint8_t ab : direction > 0 ? cw : ccw) decoder.edge(ab, now);
}

int main() {
  {
    Decoder d; Router r; d.seed(3, true, 0);
    d.button(false, 1000);
    auto press = r.poll(d.snapshot(), 1000);
    assert(press.pressed && press.activity && !press.confirmed);
    assert(!r.poll(d.snapshot(), 1020).pressed);  // one action per press
    d.button(true, 1040); d.button(false, 1060);
    assert(!r.poll(d.snapshot(), 1060).pressed);  // reject release bounce
    d.button(false, 1300);
    assert(r.poll(d.snapshot(), 1300).pressed);
  }
  {
    Decoder d; d.seed(3, true, 0);
    // Sub-detent contact chatter cannot change screens or open the menu.
    for (int n = 0; n < 100; ++n) { d.edge(1, 1000+n); d.edge(3, 1000+n); }
    assert(d.snapshot().position == 0 && d.snapshot().rock_sequence == 0);
    turn(d, 1, 1200); assert(d.snapshot().position == 1);
    turn(d, -1, 1600); assert(d.snapshot().position == 0 && d.snapshot().rock_sequence == 0);
  }
  for (int direction : {-1, 1}) {
    Decoder d; Router r; d.seed(3, true, 0);
    turn(d, direction, 1000); assert(r.poll(d.snapshot(),1000).turn == 0);
    assert(r.needs_poll(d.snapshot()));
    turn(d, -direction, 1100);
    auto pending = r.poll(d.snapshot(),1100);
    assert(pending.opened && pending.turn == 0);
    assert(!r.poll(d.snapshot(),1259).opened);
    auto menu = r.poll(d.snapshot(),1260);
    assert(r.menu_open() && !menu.opened && menu.turn == 0);
    assert(!r.poll(d.snapshot(),1261).confirmed);
    d.button(false, 1300);
    auto confirm = r.poll(d.snapshot(),1300);
    assert(confirm.confirmed && !confirm.pressed);
    assert(!r.poll(d.snapshot(),1350).confirmed);  // one return per confirmation
  }
  for (uint32_t gap : {44U, 45U, 250U, 251U}) {
    Decoder d; Router r; d.seed(3,true,0);
    turn(d,1,1000); turn(d,-1,1000+gap);
    assert(r.poll(d.snapshot(),1160+gap).opened == (gap>=45 && gap<=250));
  }
  {
    Decoder d; Router r; d.seed(3,true,0);
    turn(d,1,1000); turn(d,-1,1100);  // both halves between UI polls
    auto opened = r.poll(d.snapshot(),1260);
    assert(opened.opened && !opened.pressed);
    assert(r.poll(d.snapshot(),9260).closed);  // timeout never returns
    assert(!r.menu_open());
    d.button(false,9300); assert(!r.poll(d.snapshot(),9300).confirmed);
  }
  {
    Decoder d; Router r; d.seed(3,true,0);
    turn(d,1,1000); turn(d,-1,1100);
    d.button(false,1200);  // press before the menu appears is swallowed
    auto opened = r.poll(d.snapshot(),1260);
    assert(opened.opened && !opened.pressed);
    assert(!r.poll(d.snapshot(),1300).confirmed);
    d.button(true,1320); d.button(false,1350);  // release bounce cannot confirm
    assert(!r.poll(d.snapshot(),1360).confirmed);
    d.button(false,1600); assert(r.poll(d.snapshot(),1600).confirmed);
  }
  {
    Decoder d; Router r; d.seed(3,true,0);
    turn(d,1,1000); turn(d,-1,1100); assert(r.poll(d.snapshot(),1260).opened);
    r.poll(d.snapshot(),1610); // let the gesture tail settle before a cancel turn
    turn(d,1,1700); auto action=r.poll(d.snapshot(),1700);
    assert(action.closed && !action.confirmed && action.turn == 0); // turn cancels
    d.button(false,1900); assert(!r.poll(d.snapshot(),1900).confirmed);
  }
  {
    Decoder d; Router r; d.seed(3,true,0);
    turn(d,1,1000); r.poll(d.snapshot(),1000);
    turn(d,-1,1100); assert(r.poll(d.snapshot(),1100).opened);
    turn(d,-1,1150); turn(d,-1,1200);
    auto action=r.poll(d.snapshot(),1260); // continued return stroke is absorbed
    assert(!action.opened && action.turn == 0);
    assert(r.poll(d.snapshot(),1610).turn == 0 && r.menu_open());
  }
  {
    Decoder d; Router r; d.seed(3,true,0);
    for (int n=0;n<5;++n) turn(d,1,1000+n*100);
    turn(d,-1,1500); assert(r.poll(d.snapshot(),1500).opened);
  }
  for (int direction : {-1, 1}) {
    Decoder d; Router r; d.seed(3,true,0);
    for (int n=0;n<10;++n) turn(d,direction,1000+n*50);
    for (int n=0;n<8;++n) turn(d,-direction,1550+n*50);
    // Both long strokes can arrive between UI polls; neither has a travel limit.
    auto action = r.poll(d.snapshot(),2200);
    assert(action.opened && action.turn == 0 && !action.confirmed);
    // Queued movement after a blocked frame extends draining before its expiry.
    turn(d,-direction,2500);
    assert(!r.poll(d.snapshot(),3000).closed && r.menu_open());
    // Net-zero motion also extends the quiet period even without a new jig.
    turn(d,-direction,3050); turn(d,direction,3070);
    assert(!r.poll(d.snapshot(),3400).closed);
    assert(!r.poll(d.snapshot(),3749).closed);
    assert(!r.poll(d.snapshot(),3750).closed);
    turn(d,direction,3800);
    assert(r.poll(d.snapshot(),3800).closed);
  }
  {
    Decoder d; Router r; d.seed(3,true,0);
    turn(d,1,1000); turn(d,-1,1300); // slow reversal remains ordinary navigation
    auto action = r.poll(d.snapshot(),1551);
    assert(!action.opened && action.turn == 0 && !r.menu_open());
  }
  {
    Decoder d; Router r; d.seed(3,true,0xFFFFF000U);
    turn(d,1,0xFFFFFF80U); turn(d,-1,0xFFFFFFE4U);
    assert(r.poll(d.snapshot(),132).opened);  // uint32 millisecond wrap
  }
  for (int direction : {-1, 1}) {
    Decoder d; Router r; d.seed(3,true,0);
    turn(d,direction,1000);
    assert(r.poll(d.snapshot(),1000).turn == 0);
    assert(r.poll(d.snapshot(),1250).turn == 0);
    assert(r.poll(d.snapshot(),1251).turn == direction);
    assert(!r.needs_poll(d.snapshot()));
    turn(d,direction,2000); r.poll(d.snapshot(),2000);
    turn(d,direction,2100); r.poll(d.snapshot(),2100);
    assert(r.poll(d.snapshot(),2351).turn == 2*direction);
    assert(r.poll(d.snapshot(),2400).turn == 0);
  }
  std::puts("Dial tests passed: center press, debounce, deferred navigation, jig without page movement, cancellation, fresh press, and clock wrap");
}
