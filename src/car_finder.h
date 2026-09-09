#pragma once
#include <stdbool.h>

// "Where did I park" waypoint. One position, persisted to /car.txt on the SD as
// "lat,lon" so it survives reboot/sleep — you park, the watch can sleep for
// hours, and the spot is still there. Shared by the Auto tile (car_screen) and
// the map (car marker).

bool car_finder_save(double lat, double lon);   // set + persist the car position
bool car_finder_clear();                         // forget it (removes /car.txt)
bool car_finder_has();                           // is a position saved?
bool car_finder_get(double *lat, double *lon);   // fetch saved position

// Great-circle helpers, WGS84 sphere approximation.
double geo_distance_m(double lat1, double lon1, double lat2, double lon2);
double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2); // 0=N, CW
