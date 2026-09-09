#pragma once
#include <stdint.h>
#include "esp_gap_ble_api.h"

// Active characterisation of a BLE serial module: connect, read the Device
// Information Service (manufacturer / model / firmware / serial), check for the
// transparent-serial service (0xFFE0 / 0xFFF0), count services, and produce a
// human verdict. Reduces the name-only false positives of the skimmer detector.
//
// NOTE: only BLE / dual-mode modules are reachable. A pure Bluetooth-Classic
// HC-05 skimmer (SPP) does not advertise on BLE and cannot be probed here (the
// ESP32-S3 has no Classic BT).

void        skimmer_probe_start(const uint8_t bda[6], esp_ble_addr_type_t addr_type);
bool        skimmer_probe_busy();
bool        skimmer_probe_done();
const char *skimmer_probe_result();   // multi-line result (valid once done)
