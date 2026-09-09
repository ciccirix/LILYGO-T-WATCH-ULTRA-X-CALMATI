#pragma once

// Microphone recorder screen. Built once at boot; drives mic_rec.cpp. Recording
// keeps running if you swipe away (an indicator returns when you come back).
void mic_screen_create();
void mic_screen_show();
bool mic_screen_is_active();
