#pragma once
#include <lvgl.h>

void nfc_credit_screen_create();
void nfc_credit_screen_show();
bool nfc_credit_screen_is_active();
void nfc_credit_screen_worker(); // call from loop() — no-op when idle
