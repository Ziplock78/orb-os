// The Orb OS — settings in NVS. See orb_storage.cpp.
//
// The host_* setters Settings calls (brightness, idle, sound, chime, factory reset) are
// defined in orb_storage.cpp too; settings_view.cpp declares them itself, as it always has.
#pragma once

void loadSettings();                 // NVS -> globals; runs first in setup()
void applyThemeSettings();           // the active theme's overrides; after theme_select::init()
void applyStoredDisplaySettings();   // scope theme, filters, rotation; after the UI exists
void saveTheme(int t);               // radar::setThemeChangedCb target
