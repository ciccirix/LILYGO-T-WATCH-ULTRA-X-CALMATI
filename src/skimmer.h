#pragma once
#include <stdint.h>
#include <stdbool.h>

// Card-skimmer detector. Card skimmers (the kind you find slapped under a
// gas-pump fascia or on ATM card readers) are almost universally built
// around cheap HC-series Bluetooth modules so the operator can sit in the
// parking lot and skim track data over a BT serial link. The
// ESP32Marauder project surfaces them by matching the device name —
// "HC-03" / "HC-05" / "HC-06" are the names the bare modules ship with,
// and the people building these skimmers tend not to bother changing them
// (https://github.com/justcallmekoko/ESP32Marauder/wiki/detect-card-skimmers).
//
// We do the same name-prefix match passively via the shared BLE scan
// manager. HC-0x modules are dual-mode (BR/EDR + BLE) on the modern clones,
// so the BLE advertisement carries the same name; pure-Classic-only
// variants aren't visible to this detector. False positives are mostly
// hobby projects that left the default name in place — usually obvious
// from context (drone telemetry, robotics kit, etc.).

bool skimmer_start();         // returns true if scan started (BT init OK)
void skimmer_stop();
bool skimmer_is_running();
int  skimmer_get_count();     // total skimmer adverts accepted since start
void skimmer_reset_count();   // zero the count + dedup table (per-session reset)

// Inspect one BLE advertisement for an HC-0x name. Returns true on a match
// (and dedups against recent hits). Used by both the standalone scanner and
// the wardriver — both call this from the BT task, so the dedup state
// needs no locking.
bool skimmer_check(const uint8_t *mac6, int8_t rssi, uint8_t addr_type,
                   const uint8_t *adv, int adv_len);

// Shared match used by both the detector and the interactive screen: true if
// the advertisement looks like a BT/BLE serial module (name prefix, or the
// advertised serial service 0xFFE0/0xFFF0). name_out receives the advertised
// name, or "" when only the service UUID matched (a renamed module).
bool skimmer_ad_match(const uint8_t *adv, int adv_len, char *name_out, int name_sz);

// Drains queued detections and writes them to the SD card. Call from loop().
void skimmer_bg_tick();
