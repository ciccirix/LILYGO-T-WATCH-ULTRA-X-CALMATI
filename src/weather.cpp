#include "weather.h"
#include "wifi_creds.h"
#include "gps_screen.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---- module state ---------------------------------------------------------

static WeatherSample s_sample   = {};
static uint32_t      s_saved_at_millis = 0;  // millis() when we last set s_sample
static uint32_t      s_saved_at_epoch  = 0;  // wall-clock secs when we saved (for cross-boot age)
static const char   *s_status   = "";
static volatile bool s_fetching = false;
static TaskHandle_t  s_task     = nullptr;

// NVS: single blob, so we serialise the whole sample plus timestamps.
static const char *NVS_NS        = "weather";
static const char *NVS_KEY       = "sample";

// Package written to NVS. Explicit sizes (int32_t / float32) so a future
// firmware can still parse an older blob without alignment surprises.
struct __attribute__((packed)) WeatherBlob {
    uint32_t magic;         // 0x5757 = "WW"
    uint32_t saved_epoch;   // wall-clock secs when saved (0 if RTC was unknown)
    float    temp_c;
    int16_t  humidity_pct;
    int16_t  pressure_hpa;
    uint8_t  valid;
    uint8_t  reserved[3];
};

// ---- persistence -----------------------------------------------------------

static void nvs_save()
{
    Preferences p;
    if (!p.begin(NVS_NS, false)) return;
    WeatherBlob b = {};
    b.magic        = 0x00005757;
    b.saved_epoch  = s_saved_at_epoch;
    b.temp_c       = s_sample.temp_c;
    b.humidity_pct = (int16_t)s_sample.humidity_pct;
    b.pressure_hpa = (int16_t)s_sample.pressure_hpa;
    b.valid        = s_sample.valid ? 1 : 0;
    p.putBytes(NVS_KEY, &b, sizeof(b));
    p.end();
}

void weather_init()
{
    Preferences p;
    if (!p.begin(NVS_NS, true)) return;
    WeatherBlob b = {};
    size_t n = p.getBytes(NVS_KEY, &b, sizeof(b));
    p.end();
    if (n != sizeof(b) || b.magic != 0x00005757) return;
    s_sample.valid        = (b.valid != 0);
    s_sample.temp_c       = b.temp_c;
    s_sample.humidity_pct = b.humidity_pct;
    s_sample.pressure_hpa = b.pressure_hpa;
    s_sample.age_ms       = 0;   // recomputed on read
    s_saved_at_millis     = millis();
    s_saved_at_epoch      = b.saved_epoch;
    if (s_sample.valid) s_status = "loaded";
}

// ---- JSON parse (tiny, targeted) ------------------------------------------
//
// Open-Meteo's response for our query is small and predictable:
//   {"latitude":..., "current":{"temperature_2m":22.4,
//    "relative_humidity_2m":65, "surface_pressure":1013.2}, ...}
// We only care about three numbers under "current". Hand-parsing avoids
// pulling in ArduinoJson for 3 fields.

static bool find_number(const String &body, const char *key, float *out)
{
    int k = body.indexOf(key);
    if (k < 0) return false;
    // Skip to the ':' after the key, then read the number.
    int colon = body.indexOf(':', k);
    if (colon < 0) return false;
    int i = colon + 1;
    while (i < (int)body.length() && (body[i] == ' ' || body[i] == '\t')) i++;
    int start = i;
    while (i < (int)body.length() &&
           (isdigit(body[i]) || body[i] == '.' || body[i] == '-'))
        i++;
    if (i == start) return false;
    *out = body.substring(start, i).toFloat();
    return true;
}

// ---- fetch task -----------------------------------------------------------
//
// Runs on core 0 (WiFi core), so we don't have to hop cores on every http
// call. Bounded lifetime: turns WiFi on, does one HTTP GET with a 6 s cap,
// turns WiFi off, exits.

struct FetchArgs {
    float lat, lon;
};

static void fetch_task(void *arg)
{
    FetchArgs *a = (FetchArgs *)arg;
    float lat = a->lat, lon = a->lon;
    delete a;

    char ssid[33] = {0}, pass[65] = {0};
    if (!wifi_creds_get(ssid, sizeof(ssid), pass, sizeof(pass)) || !ssid[0]) {
        s_status = "no wifi creds";
        s_fetching = false;
        s_task = nullptr;
        vTaskDelete(NULL);
        return;
    }

    s_status = "wifi up...";
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 6000) {
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    if (WiFi.status() != WL_CONNECTED) {
        s_status = "wifi timeout";
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
        s_fetching = false;
        s_task = nullptr;
        vTaskDelete(NULL);
        return;
    }

    s_status = "fetching...";
    char url[192];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,surface_pressure",
             lat, lon);

    HTTPClient http;
    http.setTimeout(8000);
    http.setConnectTimeout(4000);
    http.begin(url);
    int code = http.GET();
    if (code != 200) {
        char buf[24]; snprintf(buf, sizeof(buf), "http %d", code);
        static char status_buf[24];
        strncpy(status_buf, buf, sizeof(status_buf) - 1);
        s_status = status_buf;
        http.end();
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
        s_fetching = false;
        s_task = nullptr;
        vTaskDelete(NULL);
        return;
    }

    String body = http.getString();
    http.end();

    // Isolate the "current" block so we don't accidentally read the top-
    // level "latitude"/"longitude" numbers into ours.
    int cur = body.indexOf("\"current\"");
    if (cur < 0) cur = 0;
    String cbody = body.substring(cur);

    float t = 0, h = 0, p = 0;
    bool ok = find_number(cbody, "temperature_2m",       &t) &&
              find_number(cbody, "relative_humidity_2m", &h) &&
              find_number(cbody, "surface_pressure",     &p);

    if (ok) {
        s_sample.valid        = true;
        s_sample.temp_c       = t;
        s_sample.humidity_pct = (int)(h + 0.5f);
        s_sample.pressure_hpa = (int)(p + 0.5f);
        s_saved_at_millis     = millis();
        time_t now = time(nullptr);
        s_saved_at_epoch      = (now > 1600000000L) ? (uint32_t)now : 0;
        nvs_save();
        s_status = "ok";
    } else {
        s_status = "json parse";
    }

    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    s_fetching = false;
    s_task = nullptr;
    vTaskDelete(NULL);
}

// ---- public API -----------------------------------------------------------

bool weather_is_fetching() { return s_fetching; }

bool weather_fetch_async()
{
    if (s_fetching) return false;
    // Need a GPS fix so we can send lat/lon. The clock face taps the
    // widget; if the user has no fix yet, that's a clean "—" state.
    if (!gps_screen_has_lock() ||
        !instance.gps.location.isValid()) {
        s_status = "no gps";
        return false;
    }

    FetchArgs *a = new FetchArgs;
    a->lat = (float)instance.gps.location.lat();
    a->lon = (float)instance.gps.location.lng();

    s_fetching = true;
    s_status = "starting...";
    BaseType_t ok = xTaskCreatePinnedToCore(fetch_task, "weather",
                                            8192, a, 4, &s_task, 0);
    if (ok != pdPASS) {
        s_fetching = false;
        s_status = "task spawn failed";
        delete a;
        return false;
    }
    return true;
}

WeatherSample weather_get()
{
    WeatherSample r = s_sample;
    r.age_ms = s_saved_at_millis ? (millis() - s_saved_at_millis) : 0;
    return r;
}

const char *weather_last_status_text()
{
    return s_status ? s_status : "";
}
