#include "gatt_explorer_screen.h"
#include <LilyGoLib.h>
#include "ble_scan_manager.h"
#include <string.h>
#include <stdio.h>
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_gatt_common_api.h"

void tools_screen_show();

#define GX_APP_ID 0x13A8

// ── scanned devices (connectable) ─────────────────────────────────────────────
struct GxDev { esp_bd_addr_t bda; esp_ble_addr_type_t type; int8_t rssi; char name[22]; };
#define GX_MAX 16
static GxDev s_dev[GX_MAX];
static volatile int s_dev_n = 0;

// ── enumeration output (filled on BT task, rendered on main) ──────────────────
#define GX_LINES 48
static char s_line[GX_LINES][52];
static volatile int  s_line_n = 0;
static volatile bool s_enum_done = false;
static volatile bool s_connecting = false;

static esp_gatt_if_t s_if = ESP_GATT_IF_NONE;
static bool s_reg = false;
static uint16_t s_conn = 0;

static bool s_pending_scan = false;
enum { VIEW_LIST, VIEW_ENUM };
static int s_view = VIEW_LIST;

static lv_obj_t *s_scr, *title_label, *info_label, *list_box;

static void addline(const char *s) {
    if (s_line_n >= GX_LINES) return;
    strncpy(s_line[s_line_n], s, sizeof(s_line[0]) - 1);
    s_line[s_line_n][sizeof(s_line[0]) - 1] = '\0';
    s_line_n++;
}
static void uuid_str(const esp_bt_uuid_t &u, char *out, int sz) {
    if (u.len == ESP_UUID_LEN_16)      snprintf(out, sz, "0x%04X", u.uuid.uuid16);
    else if (u.len == ESP_UUID_LEN_32) snprintf(out, sz, "0x%08lX", (unsigned long)u.uuid.uuid32);
    else {
        char *p = out; int rem = sz;
        for (int i = 15; i >= 0 && rem > 2; i--) { int w = snprintf(p, rem, "%02x", u.uuid.uuid128[i]); p += w; rem -= w; }
    }
}

// ── GATT client callback (BT task) ────────────────────────────────────────────
static void gx_cb(esp_gattc_cb_event_t ev, esp_gatt_if_t gi, esp_ble_gattc_cb_param_t *p)
{
    switch (ev) {
    case ESP_GATTC_REG_EVT:
        if (p->reg.status == ESP_GATT_OK) { s_if = gi; s_reg = true; }
        break;
    case ESP_GATTC_OPEN_EVT:
        if (p->open.status != ESP_GATT_OK) { addline("connessione fallita"); s_enum_done = true; break; }
        s_conn = p->open.conn_id;
        esp_ble_gattc_send_mtu_req(gi, s_conn);
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        esp_ble_gattc_search_service(gi, p->cfg_mtu.conn_id, nullptr);
        break;
    case ESP_GATTC_SEARCH_RES_EVT: {
        char u[40], line[52];
        uuid_str(p->search_res.srvc_id.uuid, u, sizeof(u));
        snprintf(line, sizeof(line), "SVC %s", u);
        addline(line);
        // list characteristics of this service
        uint16_t sh = p->search_res.start_handle, eh = p->search_res.end_handle;
        uint16_t count = 0;
        if (esp_ble_gattc_get_attr_count(gi, p->search_res.conn_id, ESP_GATT_DB_CHARACTERISTIC,
                                         sh, eh, 0, &count) == ESP_GATT_OK && count) {
            esp_gattc_char_elem_t elems[8];
            uint16_t got = count > 8 ? 8 : count;
            if (esp_ble_gattc_get_all_char(gi, p->search_res.conn_id, sh, eh, elems, &got, 0) == ESP_GATT_OK) {
                for (int i = 0; i < got; i++) {
                    char cu[40], cl[52];
                    uuid_str(elems[i].uuid, cu, sizeof(cu));
                    uint8_t pr = elems[i].properties;
                    snprintf(cl, sizeof(cl), " %s %c%c%c", cu,
                             (pr & ESP_GATT_CHAR_PROP_BIT_READ)   ? 'R' : '-',
                             (pr & ESP_GATT_CHAR_PROP_BIT_WRITE)  ? 'W' : '-',
                             (pr & ESP_GATT_CHAR_PROP_BIT_NOTIFY) ? 'N' : '-');
                    addline(cl);
                }
            }
        }
        break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT:
        esp_ble_gattc_close(gi, p->search_cmpl.conn_id);
        s_enum_done = true;
        break;
    case ESP_GATTC_DISCONNECT_EVT:
        s_enum_done = true;
        break;
    default: break;
    }
}

static void gx_begin() {
    if (s_reg) return;
    esp_ble_gattc_register_callback(gx_cb);
    esp_ble_gattc_app_register(GX_APP_ID);
    esp_ble_gatt_set_local_mtu(500);
    for (int i = 0; i < 50 && !s_reg; i++) delay(10);
}

// ── scan consumer: collect connectable devices ────────────────────────────────
static void gx_scan_cb(esp_ble_gap_cb_param_t *param)
{
    auto &r = param->scan_rst;
    bool conn = (r.ble_evt_type == ESP_BLE_EVT_CONN_ADV || r.ble_evt_type == ESP_BLE_EVT_CONN_DIR_ADV);
    if (!conn) return;
    // extract name from adv (0x08/0x09)
    char name[22]; name[0] = '\0';
    int total = (int)r.adv_data_len + (int)r.scan_rsp_len; if (total > 62) total = 62;
    for (int pos = 0; pos + 1 < total; ) {
        uint8_t seg = r.ble_adv[pos]; if (seg == 0 || pos + 1 + seg > total) break;
        uint8_t t = r.ble_adv[pos + 1];
        if (t == 0x08 || t == 0x09) {
            int n = seg - 1; if (n > (int)sizeof(name) - 1) n = sizeof(name) - 1;
            for (int i = 0; i < n; i++) { uint8_t c = r.ble_adv[pos + 2 + i]; name[i] = (c >= 0x20 && c < 0x7f) ? c : '.'; }
            name[n] = '\0';
        }
        pos += 1 + seg;
    }
    for (int i = 0; i < s_dev_n; i++)
        if (memcmp(s_dev[i].bda, r.bda, 6) == 0) { if (r.rssi > s_dev[i].rssi) s_dev[i].rssi = r.rssi;
            if (name[0] && s_dev[i].name[0] == '\0') strncpy(s_dev[i].name, name, sizeof(s_dev[i].name)-1); return; }
    if (s_dev_n >= GX_MAX) return;
    GxDev &d = s_dev[s_dev_n];
    memcpy(d.bda, r.bda, 6); d.type = r.ble_addr_type; d.rssi = (int8_t)r.rssi;
    strncpy(d.name, name[0] ? name : "(senza nome)", sizeof(d.name)-1); d.name[sizeof(d.name)-1] = '\0';
    s_dev_n++;
}

// ── connect + enumerate a chosen device (from a click) ────────────────────────
static void start_enum(const GxDev &dev)
{
    esp_ble_gap_stop_scanning();
    delay(40);
    s_line_n = 0; s_enum_done = false; s_connecting = true;
    s_view = VIEW_ENUM;
    lv_label_set_text(title_label, "GATT");
    lv_label_set_text(info_label, "connessione + enumerazione...");
    gx_begin();
    esp_bd_addr_t a; memcpy(a, dev.bda, 6);
    esp_ble_gattc_open(s_if, a, dev.type, true);
}

static void on_dev_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_dev_n) return;
    start_enum(s_dev[idx]);
}

// ── rendering ─────────────────────────────────────────────────────────────────
static void build_device_list()
{
    lv_obj_clean(list_box);
    uint32_t nsel = s_dev_n;
    char info[24]; snprintf(info, sizeof(info), "%d device - tocca", (int)nsel);
    lv_label_set_text(info_label, info);
    if (s_dev_n == 0) {
        lv_obj_t *l = lv_label_create(list_box);
        lv_obj_set_style_text_color(l, lv_color_make(0x00,0x66,0x66), LV_PART_MAIN);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_label_set_text(l, "Scansione BLE...");
        return;
    }
    static int idxbuf[GX_MAX];
    for (int i = 0; i < s_dev_n; i++) {
        idxbuf[i] = i;
        GxDev &d = s_dev[i];
        lv_obj_t *card = lv_obj_create(list_box);
        lv_obj_set_width(card, lv_pct(100)); lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_make(0x14,0x0c,0x14), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, lv_color_make(0xaa,0x55,0xff), LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_layout(card, LV_LAYOUT_FLEX); lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_event_cb(card, on_dev_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)idxbuf[i]);

        lv_obj_t *l1 = lv_label_create(card);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(l1, lv_color_make(0xcc,0x99,0xff), LV_PART_MAIN);
        lv_label_set_text_fmt(l1, "%s  %d dBm", d.name, d.rssi);
        lv_obj_t *l2 = lv_label_create(card);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l2, lv_color_make(0xaa,0xaa,0xaa), LV_PART_MAIN);
        lv_label_set_text_fmt(l2, "%02X:%02X:%02X:%02X:%02X:%02X",
                              d.bda[0],d.bda[1],d.bda[2],d.bda[3],d.bda[4],d.bda[5]);
    }
}

static void build_enum_list()
{
    lv_obj_clean(list_box);
    lv_label_set_text(info_label, s_enum_done ? "enumerazione completa (swipe-su = indietro)"
                                              : "connessione + enumerazione...");
    int n = s_line_n;
    for (int i = 0; i < n; i++) {
        bool svc = strncmp(s_line[i], "SVC", 3) == 0;
        lv_obj_t *l = lv_label_create(list_box);
        lv_obj_set_style_text_font(l, svc ? &lv_font_montserrat_16 : &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, svc ? lv_color_make(0xaa,0x55,0xff) : lv_color_make(0xdd,0xdd,0xdd), LV_PART_MAIN);
        lv_label_set_text(l, s_line[i]);
    }
}

static void on_refresh(lv_timer_t *)
{
    if (lv_scr_act() != s_scr) return;
    if (s_view == VIEW_LIST) {
        if (s_pending_scan) { s_pending_scan = false; if (ble_stack_acquire()) ble_scan_add(gx_scan_cb); }
        build_device_list();
    } else {
        build_enum_list();
    }
}

void gatt_explorer_screen_stop()
{
    ble_scan_remove(gx_scan_cb);
    ble_stack_release();
}

static void on_gesture(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_event_get_indev(e)) != LV_DIR_TOP) return;
    if (s_view == VIEW_ENUM) {          // back to the device list, resume scan
        s_view = VIEW_LIST;
        lv_label_set_text(title_label, "GATT");
        if (ble_stack_acquire()) ble_scan_add(gx_scan_cb);
        return;
    }
    gatt_explorer_screen_stop();
    tools_screen_show();
}

void gatt_explorer_screen_create()
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    title_label = lv_label_create(s_scr);
    lv_obj_set_style_text_color(title_label, lv_color_make(0xcc,0x99,0xff), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_label_set_text(title_label, "GATT");
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 10);

    info_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(info_label, lv_color_make(0x88,0x88,0x88), LV_PART_MAIN);
    lv_label_set_text(info_label, "Scansione BLE...");
    lv_obj_align(info_label, LV_ALIGN_TOP_MID, 0, 62);

    list_box = lv_obj_create(s_scr);
    lv_obj_set_size(list_box, 410, 402);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_color(list_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(list_box, lv_color_make(0x22,0x11,0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_box, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list_box, 12, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(list_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);

    lv_obj_add_event_cb(s_scr, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_refresh, 600, NULL);
}

void gatt_explorer_screen_show()
{
    s_dev_n = 0; s_view = VIEW_LIST;
    lv_scr_load(s_scr);
    s_pending_scan = true;
}

bool gatt_explorer_screen_is_active() { return lv_scr_act() == s_scr; }
