#include "drone_screen.h"
#include <LilyGoLib.h>
#include "ble_scan_manager.h"
#include "threat_radar.h"
#include <string.h>
#include <stdio.h>
#include "esp_gap_ble_api.h"

void tools_screen_show();

// ── Open Drone ID (ASTM F3411) BLE decode ─────────────────────────────────────
// Legacy BT4 advertising carries an AD structure:
//   [len][0x16 = Service Data 16-bit UUID][UUID lo=0xFA][UUID hi=0xFF]
//   [0x0D = ODID AD application code][ODID message...]
// The message header byte = (msgType<<4)|protoVer. msgType 0x0 = Basic ID:
//   byte1 = (UA-type<<4)|(ID-type), bytes2..21 = 20-byte ASCII UAS ID.
static const char *ua_type(uint8_t t) {
    switch (t) {
    case 0:  return "None";
    case 1:  return "Aereo";
    case 2:  return "Multirotore";
    case 3:  return "Elicottero";
    case 4:  return "Gyroplane";
    case 5:  return "VTOL";
    case 6:  return "Ornithopter";
    case 7:  return "Aliante";
    case 8:  return "Kite";
    case 9:  return "Free Balloon";
    case 10: return "Captive Balloon";
    case 11: return "Airship";
    case 12: return "Paracadute";
    case 15: return "Altro";
    default: return "?";
    }
}

struct Drone {
    uint8_t  mac[6];
    int8_t   rssi;
    char     id[24];
    uint8_t  uatype;
    bool     have_id;
    uint32_t last_ms;
};
#define DR_MAX 10
static Drone s_dr[DR_MAX];
static volatile int s_dr_n = 0;

static bool s_pending_scan = false;
static lv_obj_t *s_scr, *count_label, *list_box;

// Parse one advertisement; if it's Open Drone ID, fill id/uatype. Returns true.
static bool odid_parse(const uint8_t *adv, int len, char *id_out, int id_sz, uint8_t *uatype)
{
    id_out[0] = '\0'; *uatype = 0xFF;
    for (int pos = 0; pos + 1 < len; ) {
        uint8_t seg = adv[pos];
        if (seg == 0 || pos + 1 + seg > len) break;
        uint8_t type = adv[pos + 1];
        const uint8_t *d = adv + pos + 2;
        int dlen = seg - 1;
        // Service Data - 16-bit UUID, UUID 0xFFFA, app code 0x0D
        if (type == 0x16 && dlen >= 3 && d[0] == 0xFA && d[1] == 0xFF && d[2] == 0x0D) {
            const uint8_t *msg = d + 3;
            int mlen = dlen - 3;
            if (mlen >= 1) {
                uint8_t msgType = (msg[0] >> 4) & 0x0F;
                if (msgType == 0x0 && mlen >= 22) {          // Basic ID
                    *uatype = (msg[1] >> 4) & 0x0F;
                    int n = 20 < id_sz - 1 ? 20 : id_sz - 1;
                    int w = 0;
                    for (int i = 0; i < n; i++) {
                        uint8_t c = msg[2 + i];
                        if (c == 0) break;
                        id_out[w++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
                    }
                    id_out[w] = '\0';
                }
            }
            return true;   // it IS a drone Remote-ID broadcast either way
        }
        pos += 1 + seg;
    }
    return false;
}

static void dr_scan_cb(esp_ble_gap_cb_param_t *param)
{
    auto &r = param->scan_rst;
    int total = (int)r.adv_data_len + (int)r.scan_rsp_len;
    if (total > 62) total = 62;
    char id[24]; uint8_t ut;
    if (!odid_parse(r.ble_adv, total, id, sizeof(id), &ut)) return;

    uint32_t now = millis();
    for (int i = 0; i < s_dr_n; i++)
        if (memcmp(s_dr[i].mac, r.bda, 6) == 0) {
            s_dr[i].rssi = (int8_t)r.rssi; s_dr[i].last_ms = now;
            if (id[0] && !s_dr[i].have_id) { strncpy(s_dr[i].id, id, sizeof(s_dr[i].id)-1); s_dr[i].have_id = true; s_dr[i].uatype = ut; }
            return;
        }
    if (s_dr_n >= DR_MAX) return;
    Drone &d = s_dr[s_dr_n];
    memcpy(d.mac, r.bda, 6);
    d.rssi = (int8_t)r.rssi; d.last_ms = now;
    d.have_id = id[0] != '\0';
    d.uatype = ut;
    strncpy(d.id, id[0] ? id : "(Remote ID)", sizeof(d.id)-1);
    d.id[sizeof(d.id)-1] = '\0';
    s_dr_n++;
    threatradar_observe(r.bda, (int8_t)r.rssi, TR_CAT_AIRTAG);   // reuse a category for the co-move engine
}

static void build_list()
{
    lv_obj_clean(list_box);
    uint32_t now = millis();
    Drone list[DR_MAX]; int n = 0;
    for (int i = 0; i < s_dr_n; i++) if (now - s_dr[i].last_ms < 15000) list[n++] = s_dr[i];
    for (int i = 0; i < n; i++)
        for (int j = i+1; j < n; j++)
            if (list[j].rssi > list[i].rssi) { Drone t=list[i]; list[i]=list[j]; list[j]=t; }

    char cbuf[24]; snprintf(cbuf, sizeof(cbuf), "%d droni", n);
    lv_label_set_text(count_label, cbuf);

    if (n == 0) {
        lv_obj_t *l = lv_label_create(list_box);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_make(0x00,0x66,0x66), LV_PART_MAIN);
        lv_label_set_text(l, "Scansione Remote ID...\nnessun drone in trasmissione");
        return;
    }
    for (int i = 0; i < n; i++) {
        Drone &d = list[i];
        lv_obj_t *card = lv_obj_create(list_box);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_make(0x0a,0x10,0x18), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, lv_color_make(0x00,0xaa,0xff), LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(card, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *l1 = lv_label_create(card);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(l1, lv_color_make(0x33,0xcc,0xff), LV_PART_MAIN);
        lv_label_set_text_fmt(l1, LV_SYMBOL_GPS " %s", d.id);

        lv_obj_t *l2 = lv_label_create(card);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l2, lv_color_make(0xaa,0xaa,0xaa), LV_PART_MAIN);
        lv_label_set_text_fmt(l2, "%s   %d dBm", d.uatype != 0xFF ? ua_type(d.uatype) : "tipo ?", d.rssi);
    }
}

static void on_refresh(lv_timer_t *)
{
    if (lv_scr_act() != s_scr) return;
    if (s_pending_scan) { s_pending_scan = false; if (ble_stack_acquire()) ble_scan_add(dr_scan_cb); }
    build_list();
}

void drone_screen_stop()
{
    ble_scan_remove(dr_scan_cb);
    ble_stack_release();
}

static void on_gesture(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_event_get_indev(e)) == LV_DIR_TOP) {
        drone_screen_stop();
        tools_screen_show();
    }
}

void drone_screen_create()
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_color(title, lv_color_make(0x33,0xcc,0xff), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_label_set_text(title, "Drone ID");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *sub = lv_label_create(s_scr);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_make(0x66,0x88,0x99), LV_PART_MAIN);
    lv_label_set_text(sub, "Remote ID ASTM F3411 (BLE) - passivo");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 60);

    count_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(count_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(count_label, lv_color_make(0x88,0x88,0x88), LV_PART_MAIN);
    lv_label_set_text(count_label, "0 droni");
    lv_obj_align(count_label, LV_ALIGN_TOP_MID, 0, 84);

    list_box = lv_obj_create(s_scr);
    lv_obj_set_size(list_box, 404, 388);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_style_bg_color(list_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(list_box, lv_color_make(0x00,0x33,0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list_box, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(list_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);

    lv_obj_add_event_cb(s_scr, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_refresh, 700, NULL);
}

void drone_screen_show()
{
    s_dr_n = 0;
    lv_scr_load(s_scr);
    s_pending_scan = true;
}

bool drone_screen_is_active() { return lv_scr_act() == s_scr; }
