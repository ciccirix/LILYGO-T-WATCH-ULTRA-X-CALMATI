#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Weather — on-demand ambient data.
//
// The T-Watch Ultra has no ambient sensor (only die-temps for the CPU and
// the AXP2101 PMIC — see docs/esp-sr-plan.md notes on hardware). Rather
// than fake numbers on the clock face, we fetch REAL weather from
// Open-Meteo (free, no API key, HTTPS) on demand: the user taps the meteo
// widget on the clock face and we bring WiFi up for ~5 s, hit the JSON
// endpoint, save to NVS, and drop the radio again. Cache is timestamped
// so the UI can always say "22 °C, 15 min ago" instead of implying live.
//
// If we've never fetched (or GPS has no fix / no WiFi credentials), we
// show "—" — never a placeholder value. See [[no-fake-metrics]].
// ---------------------------------------------------------------------------

// One snapshot of ambient measurements. Any single field can be missing —
// the caller checks `valid` before reading.
struct WeatherSample {
    bool     valid;                 // false = we've never had a real fetch
    float    temp_c;                // ambient temperature in Celsius
    int      humidity_pct;          // relative humidity 0..100
    int      pressure_hpa;          // surface pressure in hPa
    uint32_t age_ms;                // how long since the fetch, at read time
};

// Load the last-known sample from NVS. Called once at boot from setup().
// After this, weather_get() returns whatever we had persisted.
void weather_init();

// Async fetch — non-blocking. Spawns a task on core 0 that turns WiFi on
// briefly, calls Open-Meteo with the current GPS fix, updates NVS, and
// tears the radio back down. Returns false if a fetch is already running
// or if the prereqs aren't met (no GPS fix, no WiFi credentials on file).
bool weather_fetch_async();

// True while a fetch is in progress. UI can show a spinner.
bool weather_is_fetching();

// Latest snapshot. Age is computed against millis() at the moment of the
// call, so the widget can render "1m ago" without holding a timer itself.
WeatherSample weather_get();

// Human-readable last outcome — "ok", "fetching...", "no gps", "no wifi
// creds", "http XX", "json parse". For status commands / debugging.
const char *weather_last_status_text();
