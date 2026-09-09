#pragma once
#include <lvgl.h>

// "RID Spoof" — broadcast a fake drone Remote ID (ASTM F3411 / Open Drone ID)
// over BLE: nearby Remote-ID receivers (incl. our own Drone ID tile) see a
// phantom UAV. The offensive twin of Drone ID. Uses the GAP advertiser.
void rid_spoof_screen_create();
void rid_spoof_screen_show();
void rid_spoof_screen_stop();
bool rid_spoof_screen_is_active();
