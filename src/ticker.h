#pragma once
#include <stdint.h>
#include "theme_style.h"

// The quote store: what the Stock Ticker screen draws from.
//
// Written by the network task, read by the UI, and deliberately tiny. The whole watchlist
// is under 400 bytes, so unlike the news store there is no windowing to do here: a caller
// asks for one quote by index and gets a copy.
//
// Prices are held as they arrived rather than pre-formatted, because how many decimal
// places an index wants is a rendering question and the store is not the renderer.

constexpr int TICKER_NAME_BYTES = 23;   // 22 characters and a NUL, matching the gateway

struct TickerQuote {
    char  sym[theme_style::TICKER_SYM_BYTES]   = "";
    char  name[TICKER_NAME_BYTES] = "";
    float price = 0.0f;
    float prev  = 0.0f;    // previous close: the change is price - prev, done at draw time
    char  cur[5]  = "";
    uint32_t at   = 0;     // unix seconds, when the price was struck
};

// What the app is doing, in the same shape and for the same reason as the weather map's
// phases: a screen that says "no WiFi" is telling you something a spinner cannot.
enum TickerState : uint8_t {
    TICKER_IDLE = 0,     // never asked
    TICKER_LOADING,      // a request is in flight and we have nothing yet
    TICKER_READY,        // quotes are on screen
    TICKER_STALE,        // we have quotes, but the latest attempt failed
    TICKER_NO_WIFI,
    TICKER_FAILED,       // the gateway did not answer, and we have nothing to show
    TICKER_NO_SYMBOLS,   // the watchlist is empty: a theme problem, not a network one
};

void        ticker_store_begin();
int         ticker_count();                       // how many quotes are held
bool        ticker_get(int i, TickerQuote &out);  // one quote, copied
TickerState ticker_state();
uint32_t    ticker_updated_ms();                  // millis() of the last good set, 0 if none
// How many of the asked-for symbols the gateway could not price. Shown as its own line,
// because "two of your symbols are wrong" is not the same as "the market is closed".
int         ticker_missing();

// Network step, called from the network task. Returns true when a fresh set landed.
bool ticker_fetch_step();
