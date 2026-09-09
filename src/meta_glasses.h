#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_gap_ble_api.h"

// ---------------------------------------------------------------------------
// Meta / smart-glasses detector — ported from the ESP32-Marauder C5 "RAYBAN"
// sniffer (filters originally from NullPxl). Meta Ray-Ban glasses randomise
// their BLE MAC, so an OUI match is unreliable; the robust signal is the BLE
// COMPANY ID (manufacturer data) / 16-bit SERVICE UUID / SERVICE DATA UUID.
// We flag an advert whose identifier is a known Meta/Luxottica one AND is not
// one of the common blocked IDs (Apple/Samsung/MS/phone), which keeps false
// positives down.
// ---------------------------------------------------------------------------

// Classify one BLE advertisement. Returns true on a Meta match (also folds it
// into the live list, keeping the raw advert + address type for capture mode).
// addr_type is the GAP address type (BLE_ADDR_TYPE_*). When out_id is given, it
// receives the winning Meta identifier on a match.
bool meta_glasses_check(const uint8_t *mac6, int8_t rssi, uint8_t addr_type,
                        const uint8_t *adv, int adv_len, uint16_t *out_id = nullptr);

// Standalone tile: start/stop a dedicated BLE scan feeding the detector.
bool meta_glasses_start();
void meta_glasses_stop();
bool meta_glasses_is_running();

// Live list for the screen.
struct MetaHit {
    uint8_t  mac[6];
    int8_t   rssi;
    uint16_t id;        // the Meta identifier that matched
    uint16_t hits;
    uint32_t last_ms;
};
int  meta_glasses_count();                 // distinct glasses seen this session
int  meta_glasses_get(MetaHit *out, int max);
void meta_glasses_reset();

// ── raw-advert capture (optician / research mode) ───────────────────────────
// Wayfarer / Headliner / Oakley Meta all advertise the same company id 0x0D53,
// so id alone can't tell the models apart. Capture mode turns the tile into a
// field tool: stand in front of a KNOWN pair, tap it in the live list, pick the
// model, and its FULL raw advert — MAC + address type (to see whether the MAC
// is even usable or randomised), advertised name, and every AD structure — is
// saved TAGGED with that model to Serial and /meta/adv_capture.txt. Diff two
// labelled captures to find a per-model discriminator (a name or service byte).
void meta_glasses_set_capture(bool on);      // toggles the screen's tap behaviour
bool meta_glasses_capture_enabled();
// Save the last raw advert of the device with this MAC, tagged `label`, to SD +
// Serial. Call from the LVGL/main task (never the BLE task). Returns false if
// the MAC isn't in the live list or the SD write failed.
bool meta_glasses_save_labeled(const uint8_t mac6[6], const char *label);
int  meta_glasses_capture_total();           // running count saved this session
