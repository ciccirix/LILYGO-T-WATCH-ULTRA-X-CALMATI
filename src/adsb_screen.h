#pragma once

// ADS-B flight radar screen. Built once at boot; shows a polar radar of live
// aircraft around your GPS position. Owns start/stop of the adsb.cpp fetch task
// (started on show, stopped on the swipe-up exit).
void adsb_screen_create();
void adsb_screen_show();
bool adsb_screen_is_active();
