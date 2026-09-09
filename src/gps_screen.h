#pragma once
#include <lvgl.h>

void gps_screen_create();
void gps_screen_show();
bool gps_screen_is_active();
bool gps_screen_is_powered();
bool gps_screen_has_lock();

// Power the GPS radio on if it isn't already (no-op when already on). Used by
// the Auto/find-my-car tile so opening it starts acquiring a fix automatically.
void gps_screen_power_on();
// Power the GPS radio off (no-op if already off). Used by the Task Manager.
void gps_screen_power_off();
