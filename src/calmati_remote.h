#pragma once
#include <Arduino.h>

// Calmati Remote: drives the T-Dongle-C5 "Calmati" control AP from the watch.
// The watch joins the CALMATI SoftAP (WiFi STA) and calls the dongle's HTTP API
// (GET /cmd, /status). Credentials come from wifi_creds (seed CALMATI in
// /wifi.txt) with a sane fallback. HTTP calls are short and blocking.

// Ensure we're associated to the CALMATI AP. Returns true when connected.
bool calmati_connect(uint32_t timeout_ms = 8000);
bool calmati_is_connected();

// Fire a Marauder command on the dongle (e.g. "scanall", "stopscan",
// "channel -s 6", "sniffskim"). Connects first if needed. Fills `out` with the
// dongle's reply. Returns true on HTTP 200.
bool calmati_send(const char* cmd, String& out);

// Fetch the dongle status JSON (ap/sta/ble/ch/mode) into `out`.
bool calmati_status(String& out);

// Fetch the scanned access-point list JSON ([{i,e,c,r,s},...]) into `out`.
bool calmati_aps(String& out);

// Generic GET of a dongle endpoint path (e.g. "/skimmers", "/ble", "/stations").
bool calmati_get(const char* path, String& out);

const char* calmati_ap_ssid();
