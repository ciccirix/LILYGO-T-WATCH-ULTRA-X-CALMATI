#pragma once
#include <lvgl.h>

void lora_screen_create();
void lora_screen_show();
bool lora_screen_is_active();
bool lora_screen_is_powered();
void lora_screen_force_on();   // accende la radio LoRa (usato dalla tile UDDA)
