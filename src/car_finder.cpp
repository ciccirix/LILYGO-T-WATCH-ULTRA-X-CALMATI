#include "car_finder.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <math.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *kCarPath = "/car.txt";

// RAM cache of the persisted waypoint. s_loaded stays false until the SD is
// ready AND we've actually read (or confirmed absent) the file, so a save/get
// issued before the card mounts retries on the next call rather than caching a
// bogus "nothing saved".
static bool   s_loaded = false;
static bool   s_has    = false;
static double s_lat    = 0;
static double s_lon    = 0;

static void load_once()
{
    if (s_loaded) return;
    if (!instance.isCardReady()) return;    // try again once the card is up
    s_loaded = true;

    if (!SD.exists(kCarPath)) { s_has = false; return; }
    File f = SD.open(kCarPath, FILE_READ);
    if (!f) { s_loaded = false; return; }   // transient — retry next call
    char buf[64]; size_t n = 0;
    while (f.available() && n < sizeof(buf) - 1) {
        int c = f.read();
        if (c < 0 || c == '\n') break;
        if (c == '\r') continue;
        buf[n++] = (char)c;
    }
    buf[n] = '\0';
    f.close();
    double la, lo;
    if (sscanf(buf, "%lf,%lf", &la, &lo) == 2) { s_lat = la; s_lon = lo; s_has = true; }
}

bool car_finder_save(double lat, double lon)
{
    s_lat = lat; s_lon = lon; s_has = true; s_loaded = true;   // cache immediately
    if (!instance.isCardReady()) return false;                 // in RAM only, not persisted
    File f = SD.open(kCarPath, FILE_WRITE);                     // FILE_WRITE truncates → single line
    if (!f) return false;
    f.printf("%.7f,%.7f\n", lat, lon);
    f.close();
    return true;
}

bool car_finder_clear()
{
    s_has = false; s_lat = 0; s_lon = 0; s_loaded = true;
    if (instance.isCardReady() && SD.exists(kCarPath)) SD.remove(kCarPath);
    return true;
}

bool car_finder_has() { load_once(); return s_has; }

bool car_finder_get(double *lat, double *lon)
{
    load_once();
    if (!s_has) return false;
    if (lat) *lat = s_lat;
    if (lon) *lon = s_lon;
    return true;
}

double geo_distance_m(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371000.0;
    double p1 = lat1 * M_PI / 180.0, p2 = lat2 * M_PI / 180.0;
    double dp = (lat2 - lat1) * M_PI / 180.0;
    double dl = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 2.0 * R * asin(fmin(1.0, sqrt(a)));
}

double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * M_PI / 180.0, p2 = lat2 * M_PI / 180.0;
    double dl = (lon2 - lon1) * M_PI / 180.0;
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = atan2(y, x) * 180.0 / M_PI;
    if (b < 0) b += 360.0;
    return b;
}
