#include "ble_pair_screen.h"
#include "ble_pair_audit.h"
#include <LilyGoLib.h>
#include <string.h>
#include <stdio.h>

// Defined in tools_screen.cpp
void tools_screen_show();

static lv_obj_t *screen;
static lv_obj_t *status_label;
static lv_obj_t *list_box;
static lv_obj_t *btn_scan, *btn_scan_lbl;
static lv_obj_t *btn_clear, *btn_clear_lbl;

static int  s_last_shown = -1;
static bool s_pending_start = false;   // start BLE from the timer, after the screen shows

static lv_color_t risk_bg(BpaRisk r) {
    switch (r) {
    case BPA_RISK_HIGH: return lv_color_make(0x3a, 0x10, 0x10);
    case BPA_RISK_MED:  return lv_color_make(0x33, 0x21, 0x0a);
    default:            return lv_color_make(0x10, 0x28, 0x10);
    }
}
static lv_color_t risk_border(BpaRisk r) {
    switch (r) {
    case BPA_RISK_HIGH: return lv_color_make(0xcc, 0x33, 0x33);
    case BPA_RISK_MED:  return lv_color_make(0xcc, 0x88, 0x00);
    default:            return lv_color_make(0x33, 0xcc, 0x33);
    }
}
static lv_color_t risk_fg(BpaRisk r) {
    switch (r) {
    case BPA_RISK_HIGH: return lv_color_make(0xff, 0x55, 0x55);
    case BPA_RISK_MED:  return lv_color_make(0xff, 0xcc, 0x00);
    default:            return lv_color_make(0x00, 0xff, 0x00);
    }
}

static void build_list()
{
    lv_obj_clean(list_box);
    int n = bpa_count();
    if (n == 0) {
        lv_obj_t *l = lv_label_create(list_box);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_make(0x00, 0x66, 0x00), LV_PART_MAIN);
        lv_label_set_text(l, bpa_is_running() ? "Listening..." : "Tap SCAN");
        return;
    }
    for (int i = 0; i < n; i++) {
        BpaDevice d;
        if (!bpa_get(i, &d)) continue;

        lv_obj_t *card = lv_obj_create(list_box);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, risk_bg(d.risk), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card, risk_border(d.risk), LV_PART_MAIN);
        lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(card, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *l1 = lv_label_create(card);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(l1, risk_fg(d.risk), LV_PART_MAIN);
        lv_label_set_text(l1, d.name[0] ? d.name : "(no name)");

        char line[64];
        snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X  %d dBm",
                 d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5], (int)d.rssi);
        lv_obj_t *l2 = lv_label_create(card);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l2, lv_color_make(0xaa, 0xaa, 0xaa), LV_PART_MAIN);
        lv_label_set_text(l2, line);

        char tags[64];
        bpa_tags(&d, tags, sizeof(tags));
        lv_obj_t *l3 = lv_label_create(card);
        lv_obj_set_style_text_font(l3, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l3, risk_border(d.risk), LV_PART_MAIN);
        lv_label_set_text(l3, tags);
    }
}

static void update_status()
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s  -  %d devices",
             bpa_is_running() ? "Listening" : "Stopped", bpa_count());
    lv_label_set_text(status_label, buf);
    lv_obj_set_style_text_color(status_label,
        bpa_is_running() ? lv_color_make(0x00, 0x99, 0x00) : lv_color_make(0x88, 0x88, 0x88),
        LV_PART_MAIN);
}

static void update_buttons()
{
    lv_label_set_text(btn_scan_lbl, bpa_is_running() ? "STOP" : "SCAN");
    lv_obj_set_style_bg_color(btn_scan,
        bpa_is_running() ? lv_color_make(0x88, 0x22, 0x22) : lv_color_make(0x00, 0x88, 0xcc),
        LV_PART_MAIN);
}

static void on_scan_btn(lv_event_t *)
{
    if (bpa_is_running()) bpa_stop();
    else                  bpa_start();
    s_last_shown = -1;
    update_status();
    update_buttons();
}

static void on_clear_btn(lv_event_t *)
{
    bpa_clear();
    s_last_shown = -1;
    build_list();
    update_status();
}

static void on_refresh(lv_timer_t *)
{
    if (lv_screen_active() != screen) return;
    if (s_pending_start) {           // screen is up now — safe to bring BLE up
        s_pending_start = false;
        bpa_start();
        update_status();
        update_buttons();
        return;
    }
    int n = bpa_count();
    // Rebuild when the population changes (new device) — avoids constant
    // relayout/scroll-reset while nothing new is heard.
    if (n != s_last_shown) {
        s_last_shown = n;
        build_list();
        update_status();
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        ble_pair_screen_stop();
        tools_screen_show();
    }
}

static lv_obj_t *make_button(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                             lv_color_t bg, lv_obj_t **label_out)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(l);
    if (label_out) *label_out = l;
    return b;
}

void ble_pair_screen_create()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "BLE Audit");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *warn = lv_label_create(screen);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(warn, lv_color_make(0x66, 0x88, 0x66), LV_PART_MAIN);
    lv_label_set_text(warn, "Passive - never connects or pairs");
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 58);

    status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(status_label, "Tap SCAN");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 82);

    lv_obj_t *btn_row = lv_obj_create(screen);
    lv_obj_set_size(btn_row, 404, 50);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(btn_row, LV_ALIGN_TOP_MID, 0, 108);

    btn_scan = make_button(btn_row, 196, 48, lv_color_make(0x00, 0x88, 0xCC), &btn_scan_lbl);
    lv_obj_align(btn_scan, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_event_cb(btn_scan, on_scan_btn, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn_scan_lbl, "SCAN");

    btn_clear = make_button(btn_row, 196, 48, lv_color_make(0x44, 0x44, 0x44), &btn_clear_lbl);
    lv_obj_align(btn_clear, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_event_cb(btn_clear, on_clear_btn, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn_clear_lbl, "CLEAR");

    list_box = lv_obj_create(screen);
    lv_obj_set_size(list_box, 404, 322);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 168);
    lv_obj_set_style_bg_color(list_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(list_box, lv_color_make(0x00, 0x33, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list_box, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(list_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);

    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_refresh, 500, NULL);
}

void ble_pair_screen_show()
{
    s_last_shown = -1;
    build_list();
    update_status();
    update_buttons();
    lv_scr_load(screen);         // switch to the screen FIRST — doing the heavy
    s_pending_start = true;      // bpa_start() before this blocked the change
    Serial.println("[BPA] show: screen loaded");
}

void ble_pair_screen_stop()
{
    bpa_stop();
}

bool ble_pair_screen_is_active()
{
    return lv_screen_active() == screen;
}
