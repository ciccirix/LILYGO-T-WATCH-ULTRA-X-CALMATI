#include "webpanel_screen.h"
#include "webpanel.h"
#include "tools_screen.h"
#include <lvgl.h>
#include <WiFi.h>
#include <stdio.h>

static lv_obj_t *s_screen  = nullptr;
static lv_obj_t *s_toggle  = nullptr;
static lv_obj_t *s_tlbl    = nullptr;
static lv_obj_t *s_info    = nullptr;
static lv_timer_t *s_timer = nullptr;
static bool s_active = false;

static void refresh()
{
    bool on = webpanel_is_running();
    lv_color_t c = on ? lv_color_make(0x00, 0xCC, 0x66) : lv_color_make(0x66, 0x66, 0x66);
    lv_obj_set_style_border_color(s_toggle, c, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_tlbl, c, LV_PART_MAIN);
    lv_label_set_text(s_tlbl, on ? LV_SYMBOL_WIFI "  AP ON" : LV_SYMBOL_POWER "  AP OFF");

    if (on) {
        lv_label_set_text_fmt(s_info,
            "SSID:  %s\n"
            "pass:  1337panel\n"
            "open:  http://192.168.4.1\n\n"
            "clients: %d\n\n"
            "Dashboard: status, radar &\n"
            "mic control, command box,\n"
            "download recordings.",
            webpanel_ssid(), (int)WiFi.softAPgetStationNum());
    } else {
        lv_label_set_text(s_info,
            "Web control panel.\n\n"
            "Turn on to broadcast a WiFi\n"
            "AP; join it and browse to\n"
            "192.168.4.1 for a live\n"
            "dashboard + command console.\n\n"
            "(Takes over WiFi while on.)");
    }
}

static void on_timer(lv_timer_t *) { if (s_active) refresh(); }

static void on_toggle(lv_event_t *)
{
    if (webpanel_is_running()) webpanel_stop();
    else                       webpanel_start();
    refresh();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        s_active = false;
        // Leave the AP running if it's on — it keeps serving in the background.
        tools_screen_show();
    }
}

void webpanel_screen_create()
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  WEB PANEL");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    s_info = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_info, lv_color_make(0x00, 0xCC, 0x66), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(s_info, LV_ALIGN_CENTER, 0, -10);

    s_toggle = lv_obj_create(s_screen);
    lv_obj_set_size(s_toggle, 200, 60);
    lv_obj_set_style_radius(s_toggle, 30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_toggle, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_toggle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_toggle, 2, LV_PART_MAIN);
    lv_obj_clear_flag(s_toggle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_toggle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_toggle, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_add_event_cb(s_toggle, on_toggle, LV_EVENT_CLICKED, NULL);
    s_tlbl = lv_label_create(s_toggle);
    lv_obj_set_style_text_font(s_tlbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(s_tlbl);
    lv_obj_add_flag(s_tlbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    refresh();
    s_timer = lv_timer_create(on_timer, 1000, nullptr);
    lv_timer_pause(s_timer);
}

void webpanel_screen_show()
{
    if (!s_screen) webpanel_screen_create();
    s_active = true;
    if (s_timer) lv_timer_resume(s_timer);
    refresh();
    lv_scr_load(s_screen);
}

bool webpanel_screen_is_active() { return s_active; }
