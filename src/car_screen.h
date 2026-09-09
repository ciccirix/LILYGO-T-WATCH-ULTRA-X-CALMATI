#pragma once

// "Auto" tile — find-my-car. Save the current GPS fix as your parking spot, then
// get a big arrow + distance back to it (arrow oriented by GPS heading while you
// walk; north-up when stationary, since this build has no active magnetometer).
// The saved spot also shows as a marker on the map. Swipe up = back to Tools.
void car_screen_show();
bool car_screen_is_active();
