#pragma once
#include <lvgl.h>

// "Phantom Flood" — flood the air with fake Apple Find My advertisements
// (random address + random key each cycle), so anti-stalking scanners around
// you light up with dozens of phantom trackers. Pure BLE advertising; uses the
// single GAP advertiser (pre-empts the Phone Link while running).
void phantom_flood_screen_create();
void phantom_flood_screen_show();
void phantom_flood_screen_stop();
bool phantom_flood_screen_is_active();
