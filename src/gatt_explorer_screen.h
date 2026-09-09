#pragma once
#include <lvgl.h>

// "GATT" — SkeletonKey-style BLE GATT explorer. Scans for connectable devices,
// connect to one, then enumerates every service and characteristic (with R/W/N
// properties and handles). Read-only enumeration; a base for probing smart
// locks / IoT gadgets. Uses the shared scan manager + its own GATT client.
void gatt_explorer_screen_create();
void gatt_explorer_screen_show();
void gatt_explorer_screen_stop();
bool gatt_explorer_screen_is_active();
