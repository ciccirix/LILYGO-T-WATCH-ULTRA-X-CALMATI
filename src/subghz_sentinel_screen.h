#pragma once
#include <lvgl.h>

// "SubGHz Sentinel" — jamming/carrier detector on the sub-GHz SX1262. Samples
// the instantaneous RSSI (noise floor) on the LoRa band; a sustained high floor
// with no valid packets = someone is keying a jammer nearby. Defensive.
// NOTE: takes the shared LoRa radio, so it PAUSES Meshtastic while open.
void subghz_sentinel_screen_create();
void subghz_sentinel_screen_show();
void subghz_sentinel_screen_stop();
bool subghz_sentinel_screen_is_active();
