#pragma once
#include <lvgl.h>

// "Skimmer" — dedicated interactive scanner for the BT/BLE serial modules card
// skimmers are built from. Live list sorted by proximity (signal bars), tap a
// row to lock it and read its GPS position + live NEAR/MID/FAR so you can walk
// the physical module down. Premium LVGL UI; self-contained bounded scan.
void skimmer_screen_create();
void skimmer_screen_show();
void skimmer_screen_stop();          // detach scan + release BT stack
bool skimmer_screen_is_active();
