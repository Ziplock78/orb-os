#pragma once
#include <lvgl.h>

// The Stock Ticker: one quote large enough to read across a room, with the rest of the
// watchlist running past underneath. The knob moves between them.
//
// The strip can sit along the bottom, along the top, or bend around the bezel. The curved
// placement is the reason this screen owns a canvas at all; the flat ones are a plain LVGL
// label with its own circular scroll, which costs nothing and is smoother than anything
// re-rendered per frame.
namespace tickerview {
    void      init();
    lv_obj_t *screen();
    void      onPress();          // fetch now rather than waiting out the poll
    void      onTurn(int delta);  // step through the watchlist
    void      onEnter();          // take the strip canvas, reset to the first quote
    void      onExit();           // give the canvas back
    void      tick();             // core 1: advance the strip and refresh the age line
    void      onQuotesReady();    // core 1: a fresh set is in the store
}
