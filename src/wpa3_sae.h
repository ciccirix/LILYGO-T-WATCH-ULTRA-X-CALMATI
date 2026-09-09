#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// WPA3 SAE Overflow — ported from C5Lab/projectZero (MIT).
//
// SAE (Simultaneous Authentication of Equals, RFC 7664) is the WPA3 handshake.
// Each Commit frame arriving at an AP forces the AP to allocate a Password
// Element and run one elliptic-curve multiplication over group 19 (P-256) —
// that's the CPU-expensive step at the heart of the mesh mitigation the
// standard already worries about. We generate a fresh valid random scalar,
// compute the point k·G with mbedTLS, and inject an 802.11 Auth-SAE-Commit
// frame with a spoofed source MAC per burst so the AP can't rate-limit us by
// STA identity. At enough frames/sec the AP's SAE stack (or the AP altogether)
// stops answering real clients — a WPA3 DoS.
//
// This is a management-frame attack on a specific BSSID, using the same
// esp_wifi_80211_tx primitive the deauther relies on. Deauth itself doesn't
// work on WPA3 because PMF is mandatory; SAE overflow strikes before the
// association exists, so PMF is irrelevant. Intended for authorised testing
// of your own WPA3 network — the first-boot disclaimer covers this.
//
// Frame injection runs on a task pinned to core 0 (WiFi core), same pattern
// as the deauther.
// ---------------------------------------------------------------------------

bool     wpa3_sae_start(const uint8_t bssid[6], uint8_t channel);
void     wpa3_sae_stop();
bool     wpa3_sae_is_running();
uint32_t wpa3_sae_frames_sent();
