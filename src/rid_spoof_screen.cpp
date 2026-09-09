#include "rid_spoof_screen.h"
#include <LilyGoLib.h>
#include "ble_scan_manager.h"
#include <string.h>
#include <stdio.h>
#include "esp_gap_ble_api.h"
#include "esp_system.h"

void tools_screen_show();

static lv_obj_t *s_scr, *status_label, *id_label, *btn, *btn_lbl;
static bool s_pending_stack = false, s_have_stack = false, s_spoofing = false;
static char s_fake_id[21] = "1337-PHANTOM-0001";

// Build the Open Drone ID BLE advert for a Basic-ID message and broadcast it.
//   AD: [len][0x16][0xFA 0xFF][0x0D][ODID msg 25B]
//   msg: [0x02 = BasicID/F3411][0x21 = UAType2(multi)|IDType1(serial)][20B id][2B rsv]
static void spoof_start()
{
    esp_ble_gap_stop_advertising();
    esp_bd_addr_t a; for (int i = 0; i < 6; i++) a[i] = (uint8_t)esp_random();
    a[0] |= 0xC0;
    esp_ble_gap_set_rand_addr(a);

    uint8_t msg[25]; memset(msg, 0, sizeof(msg));
    msg[0] = 0x02;                       // Basic ID, protocol version 2
    msg[1] = 0x21;                       // UA type 2 (multirotor), ID type 1 (serial)
    int n = (int)strlen(s_fake_id); if (n > 20) n = 20;
    memcpy(msg + 2, s_fake_id, n);

    uint8_t raw[31]; int p = 0;
    raw[p++] = 1 + 3 + 25;               // len = type + (uuid2+appcode1) + msg25
    raw[p++] = 0x16;                     // Service Data - 16-bit UUID
    raw[p++] = 0xFA; raw[p++] = 0xFF;    // UUID 0xFFFA (ASTM)
    raw[p++] = 0x0D;                     // ODID AD application code
    memcpy(raw + p, msg, 25); p += 25;
    esp_ble_gap_config_adv_data_raw(raw, p);

    esp_ble_adv_params_t ap = {};
    ap.adv_int_min = 0x50; ap.adv_int_max = 0x50;
    ap.adv_type = ADV_TYPE_NONCONN_IND;
    ap.own_addr_type = BLE_ADDR_TYPE_RANDOM;
    ap.channel_map = ADV_CHNL_ALL;
    ap.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    esp_ble_gap_start_advertising(&ap);
    s_spoofing = true;
}

static void set_btn(bool on)
{
    lv_label_set_text(btn_lbl, on ? "STOP" : "TRASMETTI");
    lv_obj_set_style_bg_color(btn, on ? lv_color_make(0x99,0x22,0x22)
                                      : lv_color_make(0x00,0x88,0xcc), LV_PART_MAIN);
    lv_label_set_text(status_label, on ? "IN TRASMISSIONE - drone fantasma live"
                                       : "pronto");
}

static void on_btn(lv_event_t *)
{
    if (!s_have_stack) return;
    if (s_spoofing) { s_spoofing = false; esp_ble_gap_stop_advertising(); }
    else            { spoof_start(); }
    set_btn(s_spoofing);
}

static void on_refresh(lv_timer_t *)
{
    if (lv_scr_act() != s_scr) return;
    if (s_pending_stack) { s_pending_stack = false; s_have_stack = ble_stack_acquire(); }
}

void rid_spoof_screen_stop()
{
    s_spoofing = false;
    esp_ble_gap_stop_advertising();
    if (s_have_stack) { ble_stack_release(); s_have_stack = false; }
}

static void on_gesture(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_event_get_indev(e)) == LV_DIR_TOP) {
        rid_spoof_screen_stop();
        tools_screen_show();
    }
}

void rid_spoof_screen_create()
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_color(title, lv_color_make(0x33,0xcc,0xff), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_label_set_text(title, "RID Spoof");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *sub = lv_label_create(s_scr);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_make(0x66,0x88,0x99), LV_PART_MAIN);
    lv_label_set_text(sub, "drone fantasma - Remote ID ASTM F3411 (BLE)");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 78);

    id_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(id_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(id_label, lv_color_make(0xaa,0xdd,0xff), LV_PART_MAIN);
    lv_label_set_text_fmt(id_label, "ID: %s", s_fake_id);
    lv_obj_align(id_label, LV_ALIGN_CENTER, 0, -20);

    status_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x88,0x88,0x88), LV_PART_MAIN);
    lv_label_set_text(status_label, "pronto");
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, 20);

    btn = lv_obj_create(s_scr);
    lv_obj_set_size(btn, 280, 70);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x00,0x88,0xcc), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_btn, LV_EVENT_CLICKED, NULL);
    btn_lbl = lv_label_create(btn);
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(btn_lbl, "TRASMETTI");
    lv_obj_center(btn_lbl);

    lv_obj_add_event_cb(s_scr, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_refresh, 200, NULL);
}

void rid_spoof_screen_show()
{
    s_spoofing = false;
    set_btn(false);
    lv_scr_load(s_scr);
    s_pending_stack = true;
}

bool rid_spoof_screen_is_active() { return lv_scr_act() == s_scr; }
