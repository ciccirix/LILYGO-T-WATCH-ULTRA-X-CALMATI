#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// ADS-B flight radar — live aircraft around you, drawn as a polar radar.
//
// Uses your GPS fix as the radar centre and pulls nearby traffic from adsb.fi's
// free OpenData API (no key). A background task owns the WiFi + HTTPS + JSON
// work; the LVGL screen only ever reads a thread-safe snapshot. This mirrors
// the data/UI split used by Threat Radar (threat_radar.cpp vs *_screen.cpp).
//
// Requires WiFi credentials (see wifi_creds.h — seed /wifi.txt on the SD) and a
// GPS lock. Both states are surfaced on the screen so a missing one is obvious.
// ---------------------------------------------------------------------------

enum AdsbStatus {
    ADSB_IDLE = 0,     // stopped
    ADSB_WIFI,         // associating to WiFi
    ADSB_NOGPS,        // WiFi up, waiting for a GPS fix (radar has no centre)
    ADSB_FETCH,        // request in flight
    ADSB_OK,           // last fetch succeeded
    ADSB_ERR_WIFI,     // couldn't associate / no creds
    ADSB_ERR_HTTP      // WiFi up but the API call failed
};

struct AdsbAircraft {
    char    flight[10];   // callsign, trimmed ("" if unknown)
    char    hex[8];       // ICAO 24-bit address, lowercase hex
    float   lat, lon;
    int32_t alt_ft;       // barometric altitude (ft); -1 unknown, 0 on ground
    float   gs_kt;        // ground speed (kt)
    float   track_deg;    // aircraft heading over ground (deg, 0=N)
    float   dist_nm;      // range from radar centre (nm)
    float   bearing_deg;  // bearing centre->aircraft (deg, 0=N, CW)
    bool    on_ground;
};

// Start/stop the background fetch task. Safe to call repeatedly.
void adsb_start();
void adsb_stop();
bool adsb_is_running();

// Thread-safe snapshot for the LVGL task. Copies up to `max` aircraft (already
// sorted nearest-first) into `out`; returns how many were copied.
int  adsb_get(AdsbAircraft *out, int max);

// Number of aircraft in the last snapshot.
int  adsb_count();

AdsbStatus  adsb_status();
const char *adsb_status_text();

// Milliseconds since the last successful fetch (0 = never yet).
uint32_t adsb_age_ms();

// Monotonic counter bumped once per successful fetch (incl. empty results).
// The screen compares it to its own last-drawn value to know when to repaint.
uint32_t adsb_seq();

// Radar centre actually used for the last query (last known GPS fix).
bool adsb_center(float *lat, float *lon);

// Search radius, in nautical miles, and a cycler for the RANGE button.
int  adsb_range_nm();
void adsb_cycle_range();
