#include "tracker_ring.h"
#include "ble_scan_manager.h"
#include <Arduino.h>
#include <string.h>
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatt_common_api.h"
#include "esp_bt_defs.h"

// ─────────────────────────────────────────────────────────────────────────────
// Sound-service UUIDs. Bluedroid stores 128-bit UUIDs little-endian, so the
// Apple ones are the byte-reverse of 7dfc9000-7d1c-4951-86aa-8d9728f8d66c.
// ─────────────────────────────────────────────────────────────────────────────
static const uint8_t APPLE_SND_SVC_LE[16] = {
    0x6c,0xd6,0xf8,0x28,0x97,0x8d,0xaa,0x86,0x51,0x49,0x1c,0x7d,0x00,0x90,0xfc,0x7d };
static const uint8_t APPLE_SND_CHR_LE[16] = {
    0x6c,0xd6,0xf8,0x28,0x97,0x8d,0xaa,0x86,0x51,0x49,0x1c,0x7d,0x01,0x90,0xfc,0x7d };
#define UUID16_IAS   0x1802   // Immediate Alert Service
#define UUID16_LLS   0x1803   // Link Loss Service
#define UUID16_ALERT 0x2A06   // Alert Level characteristic

#define TR_GATTC_APP_ID 0x13A7

// ─────────────────────────────────────────────────────────────────────────────
// GATT-client state. Written by the BT task (gattc_cb) and read by the caller
// task via the volatile flags below; the caller only inspects them once s_done
// is set, so no locking is needed.
// ─────────────────────────────────────────────────────────────────────────────
static esp_gatt_if_t   s_gattc_if   = ESP_GATT_IF_NONE;
static volatile bool   s_reg_done   = false;

static volatile bool   s_busy       = false;   // a ring op is in flight
static volatile bool   s_done       = false;   // op finished (success or fail)
static volatile int    s_result     = TR_RING_FAIL;
static uint16_t        s_conn_id    = 0;

// Service handle ranges discovered for the three sound services (0 = not found).
static uint16_t s_apple_s = 0, s_apple_e = 0;
static uint16_t s_ias_s   = 0, s_ias_e   = 0;
static uint16_t s_lls_s   = 0, s_lls_e   = 0;
// Which write we attempted, so WRITE_CHAR_EVT knows the success code to report.
static int      s_attempt = TR_RING_NO_SOUND;

static bool uuid_is(const esp_bt_uuid_t &u, const uint8_t le[16]) {
    return u.len == ESP_UUID_LEN_128 && memcmp(u.uuid.uuid128, le, 16) == 0;
}
static bool uuid_is16(const esp_bt_uuid_t &u, uint16_t v) {
    return u.len == ESP_UUID_LEN_16 && u.uuid.uuid16 == v;
}

// Find the Alert-Level (0x2A06) char in a service range and write `val`.
static bool write_char16(uint16_t s, uint16_t e, uint16_t chr_uuid,
                         uint8_t val, uint16_t conn) {
    if (!s) return false;
    esp_bt_uuid_t cu = {}; cu.len = ESP_UUID_LEN_16; cu.uuid.uuid16 = chr_uuid;
    esp_gattc_char_elem_t elem; uint16_t count = 1;
    if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, conn, s, e, cu, &elem, &count) != ESP_GATT_OK
        || count == 0) return false;
    return esp_ble_gattc_write_char(s_gattc_if, conn, elem.char_handle, 1, &val,
                                    ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE) == ESP_OK;
}
static bool write_char128(uint16_t s, uint16_t e, const uint8_t chr_le[16],
                          uint8_t val, uint16_t conn) {
    if (!s) return false;
    esp_bt_uuid_t cu = {}; cu.len = ESP_UUID_LEN_128; memcpy(cu.uuid.uuid128, chr_le, 16);
    esp_gattc_char_elem_t elem; uint16_t count = 1;
    if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, conn, s, e, cu, &elem, &count) != ESP_GATT_OK
        || count == 0) return false;
    return esp_ble_gattc_write_char(s_gattc_if, conn, elem.char_handle, 1, &val,
                                    ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE) == ESP_OK;
}

static void finish(int result) { s_result = result; s_done = true; }

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                     esp_ble_gattc_cb_param_t *param) {
    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (param->reg.status == ESP_GATT_OK) { s_gattc_if = gattc_if; s_reg_done = true; }
        Serial.printf("[RING] gattc reg status=%d if=%d\n", param->reg.status, gattc_if);
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status != ESP_GATT_OK) {
            Serial.printf("[RING] open FAIL status=%d\n", param->open.status);
            finish(TR_RING_FAIL);
            break;
        }
        s_conn_id = param->open.conn_id;
        s_apple_s = s_ias_s = s_lls_s = 0;
        // Negotiate MTU FIRST, then discover on CFG_MTU_EVT. The AirTag only
        // exposes its full GATT table (incl. the 7dfc9000 sound service) after
        // the MTU exchange — searching immediately on OPEN returns just 0x1800
        // (Generic Access). Mirrors the working airhorn-riscv ESP-IDF flow.
        Serial.printf("[RING] connected conn=%d, requesting MTU...\n", s_conn_id);
        esp_ble_gattc_send_mtu_req(gattc_if, s_conn_id);
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        Serial.printf("[RING] MTU=%d, discovering services...\n", param->cfg_mtu.mtu);
        esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, nullptr);
        break;

    case ESP_GATTC_SEARCH_RES_EVT: {
        const esp_bt_uuid_t &u = param->search_res.srvc_id.uuid;
        // Dump EVERY service so we can see exactly what the tag exposes.
        if (u.len == ESP_UUID_LEN_16) {
            Serial.printf("[RING]  svc 0x%04X  [%u..%u]\n", u.uuid.uuid16,
                          param->search_res.start_handle, param->search_res.end_handle);
        } else {
            Serial.print("[RING]  svc 128:");
            for (int i = 15; i >= 0; i--) Serial.printf("%02x", u.uuid.uuid128[i]);
            Serial.printf("  [%u..%u]\n",
                          param->search_res.start_handle, param->search_res.end_handle);
        }
        if (uuid_is(u, APPLE_SND_SVC_LE)) {
            s_apple_s = param->search_res.start_handle; s_apple_e = param->search_res.end_handle;
            Serial.println("[RING]  found Apple sound service");
        } else if (uuid_is16(u, UUID16_IAS)) {
            s_ias_s = param->search_res.start_handle; s_ias_e = param->search_res.end_handle;
            Serial.println("[RING]  found Immediate Alert service");
        } else if (uuid_is16(u, UUID16_LLS)) {
            s_lls_s = param->search_res.start_handle; s_lls_e = param->search_res.end_handle;
            Serial.println("[RING]  found Link Loss service");
        }
        break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        uint16_t conn = param->search_cmpl.conn_id;
        Serial.printf("[RING] search complete status=%d  apple=%d ias=%d lls=%d\n",
                      param->search_cmpl.status, s_apple_s != 0, s_ias_s != 0, s_lls_s != 0);
        // Prefer the keyfinder Immediate-Alert path (rings NOW while connected),
        // then Apple (0xAF), then Link-Loss (rings on disconnect below).
        if (write_char16(s_ias_s, s_ias_e, UUID16_ALERT, 0x02, conn)) {
            s_attempt = TR_RING_KEYFINDER;
            Serial.println("[RING]  IAS 0x02 sent");
        } else if (write_char128(s_apple_s, s_apple_e, APPLE_SND_CHR_LE, 0xAF, conn)) {
            s_attempt = TR_RING_APPLE;
            Serial.println("[RING]  Apple 0xAF sent");
        } else if (write_char16(s_lls_s, s_lls_e, UUID16_ALERT, 0x02, conn)) {
            s_attempt = TR_RING_KEYFINDER;   // beeps when we disconnect
            Serial.println("[RING]  LLS 0x02 sent (rings on disconnect)");
        } else {
            Serial.println("[RING]  no sound service on this device");
            esp_ble_gattc_close(s_gattc_if, conn);
            finish(TR_RING_NO_SOUND);
        }
        break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT:
        Serial.printf("[RING]  write status=%d\n", param->write.status);
        // Report success on the attempted type regardless of a late error — the
        // tag often ACKs and beeps, then drops the link (Apple auth-terminates).
        esp_ble_gattc_close(s_gattc_if, param->write.conn_id);
        finish(s_attempt);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        Serial.printf("[RING] disconnected reason=%d\n", param->disconnect.reason);
        if (!s_done) finish(s_attempt != TR_RING_NO_SOUND ? s_attempt : TR_RING_FAIL);
        break;

    default:
        break;
    }
}

void tracker_ring_begin() {
    if (s_reg_done) return;
    esp_ble_gattc_register_callback(gattc_cb);
    esp_ble_gattc_app_register(TR_GATTC_APP_ID);
    esp_ble_gatt_set_local_mtu(500);   // ask for a large MTU (airhorn does 500)
    // Wait briefly for REG_EVT (runs on the BT task).
    for (int i = 0; i < 50 && !s_reg_done; i++) delay(10);
}

// Core: assumes the BT stack is up and scanning is stopped. Opens, rings, waits.
static int do_ring(const uint8_t bda[6], esp_ble_addr_type_t addr_type) {
    if (!s_reg_done) { Serial.println("[RING] gattc not registered"); return TR_RING_FAIL; }
    if (s_busy)      return TR_RING_FAIL;

    s_busy = true; s_done = false; s_result = TR_RING_FAIL;
    s_attempt = TR_RING_NO_SOUND;

    esp_bd_addr_t addr; memcpy(addr, bda, 6);
    Serial.printf("[RING] opening %02X:%02X:%02X:%02X:%02X:%02X type=%d\n",
                  bda[0], bda[1], bda[2], bda[3], bda[4], bda[5], addr_type);
    if (esp_ble_gattc_open(s_gattc_if, addr, addr_type, true) != ESP_OK) {
        s_busy = false; return TR_RING_FAIL;
    }
    for (int i = 0; i < 400 && !s_done; i++) delay(20);   // up to 8 s
    if (!s_done) { esp_ble_gattc_close(s_gattc_if, s_conn_id); s_result = TR_RING_FAIL; }
    delay(200);   // let CLOSE/DISCONNECT settle
    s_busy = false;
    return s_result;
}

int tracker_ring_fire(const uint8_t bda[6], esp_ble_addr_type_t addr_type) {
    if (!ble_stack_acquire()) return TR_RING_FAIL;
    tracker_ring_begin();
    esp_ble_gap_stop_scanning();
    delay(60);
    int r = do_ring(bda, addr_type);
    ble_stack_release();
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Candidate collection for run(): a temporary scan consumer records nearby
// connectable / Find-My devices so we can ring the strongest.
// ─────────────────────────────────────────────────────────────────────────────
struct RingCand { esp_bd_addr_t bda; esp_ble_addr_type_t type; int8_t rssi; bool findmy; };
#define RING_CAND_MAX 16
static RingCand s_cand[RING_CAND_MAX];
static volatile int s_cand_n = 0;

static void ring_collect(esp_ble_gap_cb_param_t *p) {
    auto &r = p->scan_rst;
    int total = (int)r.adv_data_len + (int)r.scan_rsp_len;
    if (total > 62) total = 62;
    bool findmy = false;
    for (int pos = 0; pos + 1 < total; ) {
        uint8_t l = r.ble_adv[pos];
        if (l == 0 || pos + 1 + l > total) break;
        uint8_t t = r.ble_adv[pos + 1];
        const uint8_t *d = r.ble_adv + pos + 2;
        int dl = l - 1;
        if (t == 0xFF && dl >= 3 && d[0] == 0x4C && d[1] == 0x00 && d[2] == 0x12) findmy = true;
        pos += 1 + l;
    }
    bool conn = (r.ble_evt_type == ESP_BLE_EVT_CONN_ADV ||
                 r.ble_evt_type == ESP_BLE_EVT_CONN_DIR_ADV);
    if (!findmy && !conn) return;

    for (int i = 0; i < s_cand_n; i++)
        if (memcmp(s_cand[i].bda, r.bda, 6) == 0) {
            if (r.rssi > s_cand[i].rssi) s_cand[i].rssi = r.rssi;
            if (findmy) s_cand[i].findmy = true;
            return;
        }
    if (s_cand_n < RING_CAND_MAX) {
        memcpy(s_cand[s_cand_n].bda, r.bda, 6);
        s_cand[s_cand_n].type   = r.ble_addr_type;
        s_cand[s_cand_n].rssi   = r.rssi;
        s_cand[s_cand_n].findmy = findmy;
        s_cand_n++;
    }
}

int tracker_ring_run() {
    if (!ble_stack_acquire()) { Serial.println("[RING] stack up failed"); return TR_RING_FAIL; }
    tracker_ring_begin();

    s_cand_n = 0;
    Serial.println("[RING] scanning 6s for trackers...");
    if (!ble_scan_add(ring_collect)) { ble_stack_release(); return TR_RING_FAIL; }
    for (int i = 0; i < 120; i++) delay(50);   // 6 s
    ble_scan_remove(ring_collect);             // stack stays up (we hold it)
    esp_ble_gap_stop_scanning();
    delay(60);

    // Pick the best: Find-My first, then strongest RSSI.
    int best = -1;
    for (int i = 0; i < s_cand_n; i++) {
        if (best < 0) { best = i; continue; }
        bool bf = s_cand[best].findmy, cf = s_cand[i].findmy;
        if (cf != bf) { if (cf) best = i; }
        else if (s_cand[i].rssi > s_cand[best].rssi) best = i;
    }
    Serial.printf("[RING] candidates=%d\n", s_cand_n);
    if (best < 0) { Serial.println("[RING] no tracker found"); ble_stack_release(); return TR_RING_FAIL; }

    Serial.printf("[RING] ringing %02X:%02X:%02X:%02X:%02X:%02X rssi=%d findmy=%d\n",
                  s_cand[best].bda[0], s_cand[best].bda[1], s_cand[best].bda[2],
                  s_cand[best].bda[3], s_cand[best].bda[4], s_cand[best].bda[5],
                  s_cand[best].rssi, s_cand[best].findmy);
    int r = do_ring(s_cand[best].bda, s_cand[best].type);
    ble_stack_release();
    return r;
}
