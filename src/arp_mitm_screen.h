#pragma once
#include <lvgl.h>

// ARP MitM UI: reuse/join WiFi, SCAN the /24, tap a host (or "BLOCK ALL") to
// target, START/STOP the poison. Back = swipe down from the top (keeps controls
// out of the watch's rounded corners).
void arp_mitm_screen_create();
void arp_mitm_screen_show();
void arp_mitm_screen_stop();          // stop attack + radio cleanup (back-button exit)
bool arp_mitm_screen_is_active();
