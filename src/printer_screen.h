#pragma once
#include <lvgl.h>

// "Printer" — trova le stampanti di rete (JetDirect raw, TCP 9100) sulla /24 e
// ci manda un job che stampa "CALMATI". Richiede WiFi (wifi_creds). Solo rete tua.
void printer_screen_create();
void printer_screen_show();
void printer_screen_stop();
bool printer_screen_is_active();
