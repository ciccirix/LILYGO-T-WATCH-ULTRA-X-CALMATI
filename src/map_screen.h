#pragma once
#include <lvgl.h>

// Slippy-tile map screen for the Meshtastic flow (between NODES and SEND
// MESSAGE). When 256x256 PNG tiles exist on the SD card under /map/<z>/<x>/<y>
// — the Meshtastic UI layout — they are rendered centred on the current GPS
// location.

void map_screen_create();
void map_screen_show();
bool map_screen_is_active();

// Open the map pinned to a specific point (used by the Auto/find-my-car tile to
// jump straight to the parked spot). Enters manual-pan centred on lat/lon.
void map_screen_focus(double lat, double lon);

// True only when /sd/map exists; the mesh screens use this to decide whether
// to insert the map screen into their swipe navigation.
bool map_screen_available();
