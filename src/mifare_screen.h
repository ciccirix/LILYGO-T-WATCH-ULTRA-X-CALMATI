#pragma once
#include <lvgl.h>

// Mifare reader UI: hold a card, shows UID / SAK / ATQA / type / sectors.
// Back = swipe down. Worker must be pumped from loop().
void mifare_screen_create();
void mifare_screen_show();
void mifare_screen_stop();
bool mifare_screen_is_active();
void mifare_screen_worker();   // call from loop()
