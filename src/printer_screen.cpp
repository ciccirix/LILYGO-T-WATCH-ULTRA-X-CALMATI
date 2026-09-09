#include "printer_screen.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include "wifi_creds.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void tools_screen_show();

// PCL/PJL job that prints a CALMATI banner and ejects the page. Wrapped in the
// Universal Exit Language + PJL so PCL printers render it; plain-text printers
// mostly strip the control bits.
static const char CALMATI_JOB[] =
  "\x1b%-12345X@PJL JOB NAME=\"CALMATI\"\r\n@PJL ENTER LANGUAGE=PCL\r\n"
  "\r\n\r\n        C A L M A T I\r\n\r\n"
  "  CALMATI CALMATI CALMATI CALMATI\r\n"
  "  CALMATI CALMATI CALMATI CALMATI\r\n"
  "  CALMATI CALMATI CALMATI CALMATI\r\n\r\n"
  "\x0c" "\x1b%-12345X@PJL EOJ\r\n\x1b%-12345X";

#define PR_MAX 24
static uint32_t s_found[PR_MAX];
static volatile int  s_found_n = 0;
static volatile int  s_pct = 0;
static volatile bool s_run = false;
static volatile bool s_scanning = false;
static TaskHandle_t  s_task = nullptr;

enum { ST_JOIN, ST_SCAN, ST_RESULTS };
static int  s_state = ST_JOIN;
static bool s_pending = false;
static bool s_built = false;

static lv_obj_t *s_scr, *status_label, *list_box, *btn, *btn_lbl;

static void scan_task(void *)
{
    s_scanning = true; s_found_n = 0; s_pct = 0;
    uint32_t self = (uint32_t)WiFi.localIP();            // network order
    uint32_t base = self & 0x00FFFFFF;                   // keep first 3 octets (LE layout)
    for (int i = 1; i <= 254 && s_run; i++) {
        IPAddress ip(base | ((uint32_t)i << 24));
        WiFiClient c;
        if (c.connect(ip, 9100, 250)) {                  // JetDirect raw open
            c.stop();
            if (s_found_n < PR_MAX) s_found[s_found_n++] = (uint32_t)ip;
        }
        s_pct = i * 100 / 254;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_scanning = false;
    s_task = nullptr;
    vTaskDelete(nullptr);
}

static void print_all()
{
    lv_label_set_text(status_label, "invio CALMATI...");
    lv_refr_now(NULL);
    int ok = 0;
    for (int i = 0; i < s_found_n; i++) {
        IPAddress ip(s_found[i]);
        WiFiClient c;
        if (c.connect(ip, 9100, 1500)) {
            c.write((const uint8_t *)CALMATI_JOB, sizeof(CALMATI_JOB) - 1);
            c.flush(); delay(150); c.stop(); ok++;
        }
        delay(50);
    }
    char b[48]; snprintf(b, sizeof(b), "CALMATI inviato a %d/%d", ok, s_found_n);
    lv_label_set_text(status_label, b);
}

static void on_print(lv_event_t *)
{
    if (s_found_n > 0) print_all();
}

static void build_results()
{
    lv_obj_clean(list_box);
    for (int i = 0; i < s_found_n; i++) {
        IPAddress ip(s_found[i]);
        lv_obj_t *l = lv_label_create(list_box);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_make(0x00,0xff,0x88), LV_PART_MAIN);
        lv_label_set_text_fmt(l, LV_SYMBOL_OK "  %d.%d.%d.%d : 9100", ip[0], ip[1], ip[2], ip[3]);
    }
    if (s_found_n == 0) {
        lv_obj_t *l = lv_label_create(list_box);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_make(0x88,0x88,0x88), LV_PART_MAIN);
        lv_label_set_text(l, "nessuna stampante (porta 9100) trovata");
    }
    if (s_found_n > 0) lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
    else               lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
}

static void on_refresh(lv_timer_t *)
{
    if (lv_scr_act() != s_scr) return;
    if (s_pending) {
        s_pending = false; s_state = ST_JOIN; s_built = false;
        if (WiFi.status() != WL_CONNECTED) { WiFi.mode(WIFI_STA); wifi_creds_autoconnect(); }
    }
    if (s_state == ST_JOIN) {
        if (WiFi.status() == WL_CONNECTED) {
            s_state = ST_SCAN; s_run = true;
            xTaskCreatePinnedToCore(scan_task, "prscan", 4096, nullptr, 1, &s_task, 0);
        } else {
            lv_label_set_text(status_label, "connessione WiFi...");
        }
    } else if (s_state == ST_SCAN) {
        char b[40]; snprintf(b, sizeof(b), "scan rete %d%%  (trovate %d)", s_pct, s_found_n);
        lv_label_set_text(status_label, b);
        if (!s_scanning) { s_state = ST_RESULTS; }
    } else if (s_state == ST_RESULTS && !s_built) {
        s_built = true;
        char b[40]; snprintf(b, sizeof(b), "%d stampanti - tocca STAMPA", s_found_n);
        lv_label_set_text(status_label, b);
        build_results();
    }
}

void printer_screen_stop()
{
    s_run = false;
    for (int i = 0; i < 40 && s_scanning; i++) delay(10);
}

static void on_gesture(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_event_get_indev(e)) == LV_DIR_TOP) {
        printer_screen_stop();
        tools_screen_show();
    }
}

void printer_screen_create()
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_color(title, lv_color_make(0x00,0xff,0x88), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_label_set_text(title, "Printer");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *sub = lv_label_create(s_scr);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_make(0x66,0x88,0x77), LV_PART_MAIN);
    lv_label_set_text(sub, "stampanti di rete (TCP 9100) - solo rete tua");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 72);

    status_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x88,0x88,0x88), LV_PART_MAIN);
    lv_label_set_text(status_label, "...");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 96);

    list_box = lv_obj_create(s_scr);
    lv_obj_set_size(list_box, 410, 300);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 124);
    lv_obj_set_style_bg_color(list_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(list_box, lv_color_make(0x00,0x33,0x22), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list_box, 10, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(list_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);

    btn = lv_obj_create(s_scr);
    lv_obj_set_size(btn, 300, 64);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x00,0x88,0x44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(btn, on_print, LV_EVENT_CLICKED, NULL);
    btn_lbl = lv_label_create(btn);
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(btn_lbl, "STAMPA CALMATI");
    lv_obj_center(btn_lbl);

    lv_obj_add_event_cb(s_scr, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_refresh, 400, NULL);
}

void printer_screen_show()
{
    s_found_n = 0; s_built = false;
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(list_box);
    lv_scr_load(s_scr);
    s_pending = true;
}

bool printer_screen_is_active() { return lv_scr_act() == s_scr; }
