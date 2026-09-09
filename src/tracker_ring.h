#pragma once
#include <stdint.h>
#include "esp_gap_ble_api.h"

// "Fai Suonare" — make a nearby tracker/keyfinder play its sound, to physically
// locate a hidden one that may be following you (anti-stalking). Ported from the
// user's ESP32Marauder TrackerRing (NimBLE) to this firmware's Bluedroid stack.
//
// Sound protocol (reverse-engineered, confirmed working in the Marauder build):
//   Apple AirTag / Find My : svc 7dfc9000-…  chr 7dfc9001-…  write 1 byte 0xAF
//   cheap keyfinder (iTag…): Immediate Alert 0x1802 (or Link Loss 0x1803),
//                            chr Alert Level 0x2A06, write 1 byte 0x02
//
// NOTE: only a *separated* AirTag (away from its owner's Apple devices ~15 min,
// in lost mode) is connectable and will ring for a non-owner. One sitting next
// to its paired iPhone will not.

// Result codes shared by fire()/run().
enum {
    TR_RING_FAIL      = -1,  // connect failed (move closer / not connectable)
    TR_RING_NO_SOUND  =  0,  // connected but no known sound service
    TR_RING_APPLE     =  1,  // wrote 0xAF to the Apple sound characteristic
    TR_RING_KEYFINDER =  3,  // wrote 0x02 to Immediate-Alert / Link-Loss
};

// Register the Bluedroid GATT client app. Safe to call repeatedly; the first
// call after Bluedroid is up performs the registration. Called lazily by run().
void tracker_ring_begin();

// Ring a specific device by address. Brings the BT stack up, stops scanning,
// connects, writes the sound opcode, disconnects. Blocks up to ~8 s. Returns a
// TR_RING_* code.
int tracker_ring_fire(const uint8_t bda[6], esp_ble_addr_type_t addr_type);

// Scan ~6 s for the nearest connectable / Find-My tracker, then ring the best
// candidate. Blocks ~14 s total. Returns a TR_RING_* code (or TR_RING_FAIL if
// no candidate was found). Logs progress as [RING] over serial.
int tracker_ring_run();
