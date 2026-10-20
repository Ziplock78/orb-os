// The Orb OS — app shell hooks and per-app glue. See orb_apps.cpp.
#pragma once

void  checkAudioEvents();    // aircraft alert pings for the latest snapshot; loop(), core 1
float queryRadiusKm();       // the feed's query radius for the current range + alert circle
float deadZoneKm();          // the theme's centre dead zone, converted at the current range

// app_shell onEnter / onExit / onPress / onTurn hooks, registered in setup()
void radar_show_home_custom();
void radar_exit_release_style();
void radar_turn_select(int delta);
void radar_press_custom_or_theme();
void radar_show_weather();
void radar_hide_weather();
void weather_press_cycle();

bool host_wx_is_imperial();
