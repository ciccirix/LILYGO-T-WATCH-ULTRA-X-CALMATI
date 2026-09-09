#pragma once
#include <lvgl.h>

// Passive BLE Pair Auditor UI: SCAN/STOP + CLEAR, a live list of nearby
// advertisers with a risk chip + pairing/privacy tags. Back = swipe down.
void ble_pair_screen_create();
void ble_pair_screen_show();
void ble_pair_screen_stop();        // stop scan (back-button exit)
bool ble_pair_screen_is_active();
