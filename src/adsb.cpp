#include "adsb.h"
#include "wifi_creds.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// adsb.fi OpenData v2 — free, no key. Returns aircraft within `dist` nm of the
// point. We ask for a modest radius and cap what we keep so a busy sky (London,
// a hub) can't blow past the heap.  API etiquette: <=1 req/s; we poll every 5s.
//   https://github.com/adsbfi/opendata
// ---------------------------------------------------------------------------
#define ADSB_HOST_FMT "https://opendata.adsb.fi/api/v2/lat/%.5f/lon/%.5f/dist/%d"

#define ADSB_MAX      48        // aircraft retained per snapshot
#define ADSB_POLL_MS  5000      // between successful fetches
#define ADSB_WIFI_TMO 15000     // give WiFi this long to associate

static const int RANGES[] = { 20, 40, 80, 150 };
static volatile int s_range_idx = 1;   // default 40 nm

// ---- shared state (guarded by s_mtx) ---------------------------------------
static SemaphoreHandle_t s_mtx = nullptr;
static AdsbAircraft s_ac[ADSB_MAX];
static int          s_n        = 0;
static volatile AdsbStatus s_status = ADSB_IDLE;
static volatile uint32_t   s_last_ok = 0;
static volatile uint32_t   s_seq     = 0;
static float        s_clat = 0, s_clon = 0;
static bool         s_have_center = false;

static volatile bool s_run = false;
static TaskHandle_t  s_task = nullptr;
static wifi_mode_t   s_prev_mode = WIFI_MODE_NULL;

// ---- small helpers ---------------------------------------------------------

// Great-circle distance (nm) and initial bearing (deg, 0=N CW) from a->b.
static void geo(float lat0, float lon0, float lat1, float lon1,
                float *dist_nm, float *bearing)
{
    const double R = 3440.065;                 // Earth radius in nm
    double p0 = lat0 * DEG_TO_RAD, p1 = lat1 * DEG_TO_RAD;
    double dp = (lat1 - lat0) * DEG_TO_RAD;
    double dl = (lon1 - lon0) * DEG_TO_RAD;
    double a  = sin(dp / 2) * sin(dp / 2) +
                cos(p0) * cos(p1) * sin(dl / 2) * sin(dl / 2);
    *dist_nm  = (float)(2 * R * atan2(sqrt(a), sqrt(1 - a)));
    double y  = sin(dl) * cos(p1);
    double x  = cos(p0) * sin(p1) - sin(p0) * cos(p1) * cos(dl);
    double b  = atan2(y, x) * RAD_TO_DEG;
    *bearing  = (float)fmod(b + 360.0, 360.0);
}

// Find "key": and read the number after it inside [obj, end). Returns false if
// the key is absent or the value is null. Handles ints and floats.
static bool num_field(const char *obj, const char *end,
                      const char *key, double *out)
{
    char pat[24];
    int pn = snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = obj;
    while ((p = strstr(p, pat)) && p < end) {
        const char *v = p + pn;
        while (v < end && (*v == ' ' || *v == '\t')) v++;
        if (v < end && *v == '"') { p = v; continue; }        // string value: skip
        if (v + 4 <= end && strncmp(v, "null", 4) == 0) return false;
        *out = atof(v);
        return true;
    }
    return false;
}

// Copy the string value of "key":"..." into dst (trimmed). Returns false if
// absent. dst is always NUL-terminated.
static bool str_field(const char *obj, const char *end,
                      const char *key, char *dst, size_t dsz)
{
    dst[0] = '\0';
    char pat[24];
    int pn = snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(obj, pat);
    if (!p || p >= end) return false;
    p += pn;
    size_t i = 0;
    while (p < end && *p != '"' && i + 1 < dsz) dst[i++] = *p++;
    dst[i] = '\0';
    // trim trailing spaces (adsb.fi pads callsigns to 8 chars)
    while (i > 0 && dst[i - 1] == ' ') dst[--i] = '\0';
    return true;
}

// Parse the JSON body into a local array, then publish under the mutex.
static void parse_and_store(const char *body, float clat, float clon)
{
    const char *ac = strstr(body, "\"ac\":");
    if (ac) ac = strchr(ac, '[');
    if (!ac) { // some responses use "aircraft"
        ac = strstr(body, "\"aircraft\":");
        if (ac) ac = strchr(ac, '[');
    }
    if (!ac) return;

    AdsbAircraft tmp[ADSB_MAX];
    int n = 0;
    const char *p = ac + 1;                         // just past '['

    // Walk objects by brace matching. Cheap and robust to unknown fields.
    while (n < ADSB_MAX) {
        const char *o = strchr(p, '{');
        if (!o) break;
        int depth = 0;
        const char *e = o;
        for (; *e; e++) {
            if (*e == '{') depth++;
            else if (*e == '}') { depth--; if (depth == 0) { e++; break; } }
        }
        if (depth != 0) break;                      // truncated
        p = e;                                      // advance for next iteration

        AdsbAircraft a = {};
        double d;
        bool has_lat = num_field(o, e, "lat", &d); if (has_lat) a.lat = (float)d;
        bool has_lon = num_field(o, e, "lon", &d); if (has_lon) a.lon = (float)d;
        if (!has_lat || !has_lon) continue;         // no position: not plottable

        str_field(o, e, "flight", a.flight, sizeof(a.flight));
        str_field(o, e, "hex",    a.hex,    sizeof(a.hex));

        // Altitude: numeric ft, or the string "ground".
        char altbuf[12];
        if (str_field(o, e, "alt_baro", altbuf, sizeof(altbuf)) &&
            strncmp(altbuf, "ground", 6) == 0) {
            a.on_ground = true;
            a.alt_ft = 0;
        } else if (num_field(o, e, "alt_baro", &d)) {
            a.alt_ft = (int32_t)d;
        } else {
            a.alt_ft = -1;
        }
        a.gs_kt     = num_field(o, e, "gs",    &d) ? (float)d : -1.f;
        a.track_deg = num_field(o, e, "track", &d) ? (float)d : -1.f;

        geo(clat, clon, a.lat, a.lon, &a.dist_nm, &a.bearing_deg);
        tmp[n++] = a;
    }

    // Sort nearest-first (insertion sort; n is small).
    for (int i = 1; i < n; i++) {
        AdsbAircraft key = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j].dist_nm > key.dist_nm) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = key;
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    memcpy(s_ac, tmp, sizeof(AdsbAircraft) * n);
    s_n = n;
    s_seq++;
    xSemaphoreGive(s_mtx);
}

// Read a fresh GPS fix, falling back to the last one we ever saw so the radar
// keeps a centre through brief signal drops. Returns false only if we have
// never had a lock this session.
static bool current_center(float *lat, float *lon)
{
    if (instance.gps.location.isValid() && instance.gps.location.age() < 60000) {
        s_clat = (float)instance.gps.location.lat();
        s_clon = (float)instance.gps.location.lng();
        s_have_center = true;
    }
    if (!s_have_center) return false;
    *lat = s_clat; *lon = s_clon;
    return true;
}

static bool do_fetch(float clat, float clon)
{
    char url[128];
    snprintf(url, sizeof(url), ADSB_HOST_FMT, clat, clon, RANGES[s_range_idx]);

    WiFiClientSecure cli;
    cli.setInsecure();                 // public read-only data; no cert pinning
    HTTPClient http;
    http.setTimeout(9000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (!http.begin(cli, url)) return false;

    bool ok = false;
    int code = http.GET();
    if (code == 200) {
        String body = http.getString();      // lives in PSRAM on the S3
        parse_and_store(body.c_str(), clat, clon);
        ok = true;
    }
    http.end();
    return ok;
}

static void adsb_task(void *)
{
    // Bring WiFi up in station mode using the stored credentials.
    char ssid[33] = {0}, pass[65] = {0};
    if (!wifi_creds_get(ssid, sizeof(ssid), pass, sizeof(pass))) {
        s_status = ADSB_ERR_WIFI;
        s_run = false;
        s_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    s_prev_mode = WiFi.getMode();
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    s_status = ADSB_WIFI;

    uint32_t wifi_since = millis();
    while (s_run) {
        if (WiFi.status() != WL_CONNECTED) {
            s_status = ADSB_WIFI;
            if (millis() - wifi_since > ADSB_WIFI_TMO) {
                s_status = ADSB_ERR_WIFI;
                WiFi.disconnect();
                WiFi.begin(ssid, pass);       // retry association
                wifi_since = millis();
            }
            vTaskDelay(pdMS_TO_TICKS(400));
            continue;
        }

        float clat, clon;
        if (!current_center(&clat, &clon)) {
            s_status = ADSB_NOGPS;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        s_status = ADSB_FETCH;
        if (do_fetch(clat, clon)) {
            s_status = ADSB_OK;
            s_last_ok = millis();
        } else {
            s_status = ADSB_ERR_HTTP;
        }

        // Poll interval, but stay responsive to stop().
        for (int i = 0; i < ADSB_POLL_MS / 100 && s_run; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Restore the WiFi mode we found (scanners want it off/promiscuous).
    if (s_prev_mode == WIFI_MODE_NULL || s_prev_mode == WIFI_OFF) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    } else {
        WiFi.mode(s_prev_mode);
    }
    s_status = ADSB_IDLE;
    s_task = nullptr;
    vTaskDelete(nullptr);
}

// ---- public API ------------------------------------------------------------

void adsb_start()
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (s_run || s_task) return;
    s_run = true;
    s_status = ADSB_WIFI;
    xTaskCreatePinnedToCore(adsb_task, "adsb", 8192, nullptr, 1, &s_task, 0);
}

void adsb_stop()
{
    s_run = false;    // task restores WiFi and self-deletes
}

bool adsb_is_running() { return s_run; }

int adsb_get(AdsbAircraft *out, int max)
{
    if (!s_mtx) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int n = s_n < max ? s_n : max;
    memcpy(out, s_ac, sizeof(AdsbAircraft) * n);
    xSemaphoreGive(s_mtx);
    return n;
}

int adsb_count()
{
    if (!s_mtx) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int n = s_n;
    xSemaphoreGive(s_mtx);
    return n;
}

AdsbStatus adsb_status() { return s_status; }

const char *adsb_status_text()
{
    switch (s_status) {
        case ADSB_WIFI:     return "WiFi...";
        case ADSB_NOGPS:    return "waiting GPS fix";
        case ADSB_FETCH:    return "fetching";
        case ADSB_OK:       return "live";
        case ADSB_ERR_WIFI: return "no WiFi (check /wifi.txt)";
        case ADSB_ERR_HTTP: return "API error";
        default:            return "idle";
    }
}

uint32_t adsb_age_ms() { return s_last_ok ? (millis() - s_last_ok) : 0; }

uint32_t adsb_seq() { return s_seq; }

bool adsb_center(float *lat, float *lon)
{
    if (!s_have_center) return false;
    *lat = s_clat; *lon = s_clon;
    return true;
}

int  adsb_range_nm()   { return RANGES[s_range_idx]; }

void adsb_cycle_range()
{
    s_range_idx = (s_range_idx + 1) % (int)(sizeof(RANGES) / sizeof(RANGES[0]));
}
