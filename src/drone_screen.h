#pragma once
#include <lvgl.h>

// "Drone ID" — passive detector for the Remote ID broadcast that ASTM F3411 /
// EU regulation requires most drones to emit. Decodes the Open Drone ID BLE
// signature (Service Data UUID 0xFFFA + AD app-code 0x0D) and shows Basic-ID
// (serial / UAV id), UA type, and signal strength. Passive only.
void drone_screen_create();
void drone_screen_show();
void drone_screen_stop();
bool drone_screen_is_active();
