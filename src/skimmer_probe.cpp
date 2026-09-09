#include "skimmer_probe.h"
#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include "ble_scan_manager.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatt_common_api.h"

#define SP_APP_ID 0x13A9
#define DIS_SVC   0x180A
#define C_MANUF   0x2A29
#define C_MODEL   0x2A24
#define C_FW      0x2A26
#define C_SERIAL  0x2A25

static esp_gatt_if_t s_if = ESP_GATT_IF_NONE;
static bool s_reg = false;
static volatile bool s_busy = false, s_done = false;
static uint16_t s_conn = 0;

static char s_manuf[24], s_model[24], s_fw[24], s_serial[24];
static bool s_has_serial_svc = false;
static int  s_svc_count = 0;
static uint16_t s_dis_s = 0, s_dis_e = 0;
static uint16_t s_h_manuf = 0, s_h_model = 0, s_h_fw = 0, s_h_serial = 0;
static int s_read_step = 0;
static char s_result[220];

static void build_result()
{
    const char *verdict;
    // Recognisable vendor string in DIS → a real commercial product (benign).
    if (s_manuf[0])
        verdict = "prodotto commerciale (benigno)";
    else if (s_has_serial_svc)
        verdict = "modulo seriale grezzo - contestualizza";
    else
        verdict = "device BLE generico";

    snprintf(s_result, sizeof(s_result),
        "Marca: %s\nModello: %s\nFW: %s\nSeriale: %s\nServizio seriale: %s\nServizi: %d\nVerdetto: %s",
        s_manuf[0]  ? s_manuf  : "-",
        s_model[0]  ? s_model  : "-",
        s_fw[0]     ? s_fw     : "-",
        s_serial[0] ? s_serial : "-",
        s_has_serial_svc ? "FFE0/FFF0 SI" : "no",
        s_svc_count, verdict);
    s_done = true; s_busy = false;
}

static uint16_t handle_for(uint16_t uuid16)
{
    esp_bt_uuid_t u = {}; u.len = ESP_UUID_LEN_16; u.uuid.uuid16 = uuid16;
    esp_gattc_char_elem_t e; uint16_t c = 1;
    if (esp_ble_gattc_get_char_by_uuid(s_if, s_conn, s_dis_s, s_dis_e, u, &e, &c) == ESP_GATT_OK && c)
        return e.char_handle;
    return 0;
}

static void read_next()
{
    // Walk the four DIS chars in order; skip any the device doesn't expose.
    while (s_read_step < 4) {
        uint16_t h = 0;
        switch (s_read_step) {
            case 0: h = s_h_manuf;  break;
            case 1: h = s_h_model;  break;
            case 2: h = s_h_fw;     break;
            case 3: h = s_h_serial; break;
        }
        if (h) { esp_ble_gattc_read_char(s_if, s_conn, h, ESP_GATT_AUTH_REQ_NONE); return; }
        s_read_step++;
    }
    esp_ble_gattc_close(s_if, s_conn);   // done reading → build on close/disconnect
    build_result();
}

static void store_read(const uint8_t *v, int len)
{
    char tmp[24]; int n = len < 23 ? len : 23;
    for (int i = 0; i < n; i++) tmp[i] = (v[i] >= 0x20 && v[i] < 0x7f) ? (char)v[i] : '.';
    tmp[n] = '\0';
    switch (s_read_step) {
        case 0: strcpy(s_manuf, tmp);  break;
        case 1: strcpy(s_model, tmp);  break;
        case 2: strcpy(s_fw, tmp);     break;
        case 3: strcpy(s_serial, tmp); break;
    }
}

static void cb(esp_gattc_cb_event_t ev, esp_gatt_if_t gi, esp_ble_gattc_cb_param_t *p)
{
    switch (ev) {
    case ESP_GATTC_REG_EVT:
        if (p->reg.status == ESP_GATT_OK) { s_if = gi; s_reg = true; }
        break;
    case ESP_GATTC_OPEN_EVT:
        if (p->open.status != ESP_GATT_OK) { snprintf(s_result, sizeof(s_result), "connessione fallita\n(avvicina il device / non connettibile)"); s_done = true; s_busy = false; break; }
        s_conn = p->open.conn_id;
        s_dis_s = s_dis_e = 0; s_has_serial_svc = false; s_svc_count = 0;
        s_manuf[0]=s_model[0]=s_fw[0]=s_serial[0]='\0';
        esp_ble_gattc_send_mtu_req(gi, s_conn);
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        esp_ble_gattc_search_service(gi, p->cfg_mtu.conn_id, nullptr);
        break;
    case ESP_GATTC_SEARCH_RES_EVT: {
        s_svc_count++;
        const esp_bt_uuid_t &u = p->search_res.srvc_id.uuid;
        if (u.len == ESP_UUID_LEN_16) {
            if (u.uuid.uuid16 == DIS_SVC) { s_dis_s = p->search_res.start_handle; s_dis_e = p->search_res.end_handle; }
            if (u.uuid.uuid16 == 0xFFE0 || u.uuid.uuid16 == 0xFFF0) s_has_serial_svc = true;
        }
        break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (s_dis_s) {
            s_h_manuf  = handle_for(C_MANUF);
            s_h_model  = handle_for(C_MODEL);
            s_h_fw     = handle_for(C_FW);
            s_h_serial = handle_for(C_SERIAL);
            s_read_step = 0;
            read_next();
        } else {
            esp_ble_gattc_close(gi, p->search_cmpl.conn_id);
            build_result();
        }
        break;
    case ESP_GATTC_READ_CHAR_EVT:
        if (p->read.status == ESP_GATT_OK) store_read(p->read.value, p->read.value_len);
        s_read_step++;
        read_next();
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        if (!s_done) build_result();
        break;
    default: break;
    }
}

static void ensure_reg()
{
    if (s_reg) return;
    esp_ble_gattc_register_callback(cb);
    esp_ble_gattc_app_register(SP_APP_ID);
    esp_ble_gatt_set_local_mtu(500);
    for (int i = 0; i < 50 && !s_reg; i++) delay(10);
}

void skimmer_probe_start(const uint8_t bda[6], esp_ble_addr_type_t addr_type)
{
    if (s_busy) return;
    ensure_reg();
    if (!s_reg) { snprintf(s_result, sizeof(s_result), "GATT client non pronto"); s_done = true; return; }
    s_busy = true; s_done = false;
    esp_ble_gap_stop_scanning();
    delay(60);
    esp_bd_addr_t a; memcpy(a, bda, 6);
    if (esp_ble_gattc_open(s_if, a, addr_type, true) != ESP_OK) {
        snprintf(s_result, sizeof(s_result), "apertura fallita");
        s_done = true; s_busy = false;
    }
}

bool        skimmer_probe_busy()   { return s_busy; }
bool        skimmer_probe_done()   { return s_done; }
const char *skimmer_probe_result() { return s_result; }
