#pragma once
// Whole-device visual skin: background/text/accent palette used by every screen
// (distinct from radar::setTheme, which only retints the flight-scope chrome).
// Switching writes NVS and reboots (device) so no screen ever runs half old /
// half new palette in RAM — see the memory-budget discussion this followed.
#include <lvgl.h>

enum AppThemeId { APP_THEME_DEFAULT = 0, APP_THEME_OFFICE = 1, APP_THEME_COUNT = 2 };

struct AppPalette {
    lv_color_t bg;         // screen background
    lv_color_t panel;      // cards, tracks, the boot-splash halo
    lv_color_t highlight;  // selected menu row / active control fill
    lv_color_t ink;        // primary text
    lv_color_t soft;       // secondary text / labels
    lv_color_t dim;        // hints, disabled, least-emphasis text
    lv_color_t accent;     // clock hand, links, active state, radar blips
    lv_color_t hairline;   // list dividers
    lv_color_t onAccent;   // text drawn on top of an accent-filled control
};

namespace app_theme {

void init();                        // load saved choice from NVS; call once at boot, before any view init()
int  get();                         // APP_THEME_DEFAULT / APP_THEME_OFFICE
const char *name(int t);            // "Default" / "Office"
const AppPalette &palette();        // active palette (by const ref — no copies in draw callbacks)
void set(int t);                    // persists, then reboots (device: ESP.restart(); sim: setRestartHook's hook)

// Native only: every screen's colors are baked in at its own init() (see ui.cpp's
// UI_* / settings_view.cpp's C_WHITE etc.), so set() can't repaint a running sim in
// place any more than it can on the device — it needs an actual process restart to
// take effect. sim_main.cpp registers its own re-exec here; hardware ignores this
// entirely (ESP.restart() ends the process, nothing left to call back into).
void setRestartHook(void (*hook)());

} // namespace app_theme
