#pragma once

// Generic read-only result list fetched from a dongle endpoint (e.g.
// "/skimmers", "/ble", "/stations"). `alert` styles the rows as warnings
// (violet), used for the skimmer list.
void calmati_result_screen_show(const char* title, const char* endpoint, bool alert);
bool calmati_result_screen_is_active();
