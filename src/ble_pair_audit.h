#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// BLE Pair Auditor ("Whisper") — passive.
//
// Rides the shared ble_scan_manager and, for every advertiser it hears,
// distills the pairing/privacy posture that is observable WITHOUT connecting:
//
//   * address privacy : public / static-random (both trackable) vs RPA (good)
//   * connectable?    : ADV_IND / ADV_DIRECT_IND = open to a pairing attempt
//   * discoverable    : Limited flag = device is IN PAIRING MODE right now
//   * BR/EDR          : dual-mode = larger attack surface
//   * services        : advertises HID (0x1812) = pairable input device
//                       (keystroke-injection target if it accepts Just Works)
//
// It never connects and never initiates pairing, so it is safe to run near
// devices you don't own. (A future v2 could add an opt-in active probe that
// reads the peer's IO-capabilities / MITM flag, but that initiates pairing and
// must be gated + coordinated with phone_link's use of the GAP-forward slot.)
// ---------------------------------------------------------------------------

#define BPA_MAX_DEVICES 48

// Risk buckets (drive list colour + sort order).
enum BpaRisk { BPA_RISK_LOW = 0, BPA_RISK_MED = 1, BPA_RISK_HIGH = 2 };

// Address privacy classes.
enum BpaAddr { BPA_ADDR_PUBLIC, BPA_ADDR_RAND_STATIC, BPA_ADDR_RPA, BPA_ADDR_NRPA };

struct BpaDevice {
    uint8_t  mac[6];
    int8_t   rssi;
    char     name[24];
    BpaAddr  addr;
    bool     connectable;
    bool     limited_disc;   // in pairing mode right now
    bool     general_disc;
    bool     bredr;          // dual-mode (BR/EDR supported)
    bool     hid;            // advertises HID service 0x1812
    BpaRisk  risk;
};

bool bpa_start();      // attach scan consumer (brings the BLE stack up)
void bpa_stop();       // detach
bool bpa_is_running();
void bpa_clear();      // wipe the collected table

int  bpa_count();
// Copy entry i (already sorted High->Low risk, then RSSI). false if out of range.
bool bpa_get(int i, BpaDevice *out);

// One-line tag summary for the UI, e.g. "PAIRING - HID - public". Returns `out`.
const char *bpa_tags(const BpaDevice *d, char *out, int out_sz);
