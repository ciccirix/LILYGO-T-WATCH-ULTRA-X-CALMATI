#pragma once
#include <lvgl.h>

// BadUSB UI: pick a payload, RUN types it over USB HID into the connected PC.
// Back = swipe down. Authorised machines only.
void badusb_screen_create();
void badusb_screen_show();
void badusb_screen_stop();
bool badusb_screen_is_active();
