#pragma once
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Web control panel — a SoftAP + tiny HTTP server at 192.168.4.1. Join the AP
// from a phone/laptop and you get a live dashboard: system status, one-tap
// start/stop of the ADS-B radar and the mic recorder, a command box wired to
// the shared console (console.cpp), and download links for your recordings.
//
// Standalone-AP tool (like the Evil Portal): starting it puts WiFi in AP mode,
// so it doesn't run at the same time as the STA-based radar fetch.
// ---------------------------------------------------------------------------

void webpanel_start();
void webpanel_stop();
bool webpanel_is_running();

// Serve pending HTTP requests. Call from loop() alongside the other *_tick()s.
void webpanel_tick();

const char *webpanel_ssid();
