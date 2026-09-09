#include "ble_pair_audit.h"
#include "ble_scan_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// AD (Advertising Data) type codes we care about (Core Spec / CSS).
#define AD_FLAGS        0x01
#define AD_16SRV_PART   0x02
#define AD_16SRV_CMPL   0x03
#define AD_16SRV_SOL    0x14
#define AD_NAME_SHORT   0x08
#define AD_NAME_CMPL    0x09
#define HID_SERVICE_UUID 0x1812

#define FLAG_LE_LIMITED 0x01
#define FLAG_LE_GENERAL 0x02
#define FLAG_BREDR_NOT  0x04

// Raw scan hit posted from the BLE callback to the main task via a queue — the
// SAME safe pattern airtag.cpp uses. No spinlock is taken in the callback (the
// previous portENTER_CRITICAL there could deadlock the two cores), and all the
// parsing / table work happens on the main task in bpa_drain().
struct RawHit {
    uint8_t bda[6];
    int8_t  rssi;
    uint8_t addr_type;
    uint8_t evt_type;
    uint8_t adv[62];
    uint8_t adv_len;
};

static BpaDevice     s_dev[BPA_MAX_DEVICES];   // owned by the main task
static int           s_dev_n = 0;
static BpaDevice     s_sorted[BPA_MAX_DEVICES];
static int           s_sorted_n = 0;
static QueueHandle_t s_queue    = nullptr;
static volatile bool s_running  = false;

// ---- helpers (main task only) ----------------------------------------------
static BpaAddr classify_addr(uint8_t addr_type, const uint8_t mac[6]) {
    if (addr_type == 0 /* PUBLIC */) return BPA_ADDR_PUBLIC;
    switch (mac[0] & 0xC0) {
        case 0xC0: return BPA_ADDR_RAND_STATIC;
        case 0x40: return BPA_ADDR_RPA;
        case 0x00: return BPA_ADDR_NRPA;
        default:   return BPA_ADDR_RAND_STATIC;
    }
}

static BpaRisk compute_risk(const BpaDevice *d) {
    if (d->connectable && (d->limited_disc || d->hid)) return BPA_RISK_HIGH;
    if (d->connectable || d->addr == BPA_ADDR_PUBLIC || d->addr == BPA_ADDR_RAND_STATIC)
        return BPA_RISK_MED;
    return BPA_RISK_LOW;
}

static void parse_into(BpaDevice *d, const uint8_t *buf, int len, bool connectable) {
    if (connectable) d->connectable = true;
    int pos = 0;
    while (pos + 1 < len) {
        int seg_len = buf[pos];
        if (seg_len == 0) break;
        if (pos + 1 + seg_len > len) break;
        uint8_t type = buf[pos + 1];
        const uint8_t *val = &buf[pos + 2];
        int vlen = seg_len - 1;
        switch (type) {
        case AD_FLAGS:
            if (vlen >= 1) {
                d->limited_disc = (val[0] & FLAG_LE_LIMITED) != 0;
                d->general_disc = (val[0] & FLAG_LE_GENERAL) != 0;
                d->bredr        = (val[0] & FLAG_BREDR_NOT) == 0;
            }
            break;
        case AD_NAME_SHORT:
        case AD_NAME_CMPL: {
            int n = vlen < (int)sizeof(d->name) - 1 ? vlen : (int)sizeof(d->name) - 1;
            if (n > 0 && d->name[0] == '\0') {
                for (int j = 0; j < n; j++) {   // sanitise -> printable ASCII (LVGL-safe)
                    uint8_t ch = val[j];
                    d->name[j] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : '.';
                }
                d->name[n] = '\0';
            }
            break;
        }
        case AD_16SRV_PART:
        case AD_16SRV_CMPL:
        case AD_16SRV_SOL:
            for (int i = 0; i + 1 < vlen; i += 2) {
                uint16_t uuid = (uint16_t)val[i] | ((uint16_t)val[i + 1] << 8);
                if (uuid == HID_SERVICE_UUID) d->hid = true;
            }
            break;
        default: break;
        }
        pos += 1 + seg_len;
    }
}

// Pull every queued hit into the device table. Main task only — no locks.
static void bpa_drain() {
    if (!s_queue) return;
    RawHit h;
    while (xQueueReceive(s_queue, &h, 0) == pdTRUE) {
        int idx = -1;
        for (int i = 0; i < s_dev_n; i++)
            if (memcmp(s_dev[i].mac, h.bda, 6) == 0) { idx = i; break; }
        if (idx < 0) {
            if (s_dev_n >= BPA_MAX_DEVICES) continue;
            idx = s_dev_n++;
            memset(&s_dev[idx], 0, sizeof(BpaDevice));
            memcpy(s_dev[idx].mac, h.bda, 6);
        }
        BpaDevice *d = &s_dev[idx];
        d->rssi = h.rssi;
        d->addr = classify_addr(h.addr_type, h.bda);
        bool connectable = (h.evt_type == ESP_BLE_EVT_CONN_ADV ||
                            h.evt_type == ESP_BLE_EVT_CONN_DIR_ADV);
        parse_into(d, h.adv, h.adv_len, connectable);
        d->risk = compute_risk(d);
    }
}

// ---- BLE scan consumer (BT task): copy raw, post to queue, NOTHING else -----
static void on_scan_result(esp_ble_gap_cb_param_t *param) {
    if (!s_running || !s_queue) return;
    auto &res = param->scan_rst;
    int total = (int)res.adv_data_len + (int)res.scan_rsp_len;
    if (total < 0) total = 0;
    if (total > 62) total = 62;

    RawHit h;
    memcpy(h.bda, res.bda, 6);
    h.rssi      = (int8_t)res.rssi;
    h.addr_type = (uint8_t)res.ble_addr_type;
    h.evt_type  = (uint8_t)res.ble_evt_type;
    h.adv_len   = (uint8_t)total;
    if (total) memcpy(h.adv, res.ble_adv, total);
    xQueueSend(s_queue, &h, 0);   // drop if full — fine, we just miss a sighting
}

// ---- public API -------------------------------------------------------------
bool bpa_start() {
    if (s_running) return true;
    if (!s_queue) {
        s_queue = xQueueCreate(24, sizeof(RawHit));
        if (!s_queue) return false;
    }
    // Only tear WiFi down if it is actually up (ARP MitM may have left STA on);
    // calling the teardown when WiFi is already off can itself stall.
    if (WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(60);
    }
    if (!ble_scan_add(on_scan_result)) return false;
    s_running = true;
    return true;
}

void bpa_stop() {
    if (!s_running) return;
    s_running = false;
    ble_scan_remove(on_scan_result);
    if (s_queue) xQueueReset(s_queue);
}

bool bpa_is_running() { return s_running; }

void bpa_clear() {
    s_dev_n = 0;
    if (s_queue) xQueueReset(s_queue);
}

static int sort_cmp(const void *a, const void *b) {
    const BpaDevice *x = (const BpaDevice *)a, *y = (const BpaDevice *)b;
    if (x->risk != y->risk) return (int)y->risk - (int)x->risk;
    return (int)y->rssi - (int)x->rssi;
}

int bpa_count() {
    bpa_drain();                       // fold queued hits in (main task)
    int n = s_dev_n;
    if (n > BPA_MAX_DEVICES) n = BPA_MAX_DEVICES;
    memcpy(s_sorted, s_dev, sizeof(BpaDevice) * n);
    if (n > 1) qsort(s_sorted, n, sizeof(BpaDevice), sort_cmp);
    s_sorted_n = n;
    return n;
}

bool bpa_get(int i, BpaDevice *out) {
    if (i < 0 || i >= s_sorted_n || !out) return false;
    *out = s_sorted[i];
    return true;
}

const char *bpa_tags(const BpaDevice *d, char *out, int out_sz) {
    out[0] = '\0';
    int used = 0;
    auto add = [&](const char *t) {
        if (out_sz - used <= 1) return;
        int w = snprintf(out + used, out_sz - used, "%s%s", used ? " - " : "", t);
        if (w > 0) used += w;
        if (used >= out_sz) used = out_sz - 1;
    };
    if (d->connectable && d->limited_disc) add("PAIRING");
    else if (d->connectable && d->general_disc) add("OPEN");
    else if (d->connectable) add("conn");
    if (d->hid)   add("HID");
    if (d->bredr) add("BR/EDR");
    switch (d->addr) {
        case BPA_ADDR_PUBLIC:      add("public"); break;
        case BPA_ADDR_RAND_STATIC: add("static"); break;
        case BPA_ADDR_RPA:         add("RPA");    break;
        case BPA_ADDR_NRPA:        add("NRPA");   break;
    }
    if (used == 0) add("beacon");
    return out;
}
