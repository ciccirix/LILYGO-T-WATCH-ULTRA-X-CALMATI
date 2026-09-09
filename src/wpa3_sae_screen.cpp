#include "wpa3_sae_screen.h"
#include "wpa3_sae.h"
#include "tools_screen.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include <lvgl.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// UI for the SAE-overflow WPA3 stress tool. Same shape as the deauther screen:
// SCAN → tap an AP → FLOOD button. The scan list is filtered down to WPA3-SAE
// networks (auth mode WPA3-PSK or WPA2/WPA3-PSK mixed), because sending SAE
// Commit frames at a WPA2-only or Open AP does nothing useful and would just
// hide the WPA3 targets in a crowded scan.
// ---------------------------------------------------------------------------

enum SState {
    SST_IDLE,
    SST_SCANNING,
    SST_LIST,
    SST_RUNNING,
};

struct SAp {
    char    ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int32_t rssi;
};

static lv_obj_t *screen;
static lv_obj_t *status_label;
static lv_obj_t *list_box;
static lv_obj_t *btn_scan, *btn_scan_lbl;
static lv_obj_t *btn_atk,  *btn_atk_lbl;
static lv_obj_t *live_lbl;    // frame counter while running

static SState s_state = SST_IDLE;
static SAp    s_aps[24];
static int    s_ap_count = 0;
static int    s_target   = -1;

// ─── AP list ─────────────────────────────────────────────────────────────────
static void on_ap_clicked(lv_event_t *e);

static void show_aps()
{
    lv_obj_clean(list_box);
    if (s_ap_count == 0) {
        lv_obj_t *l = lv_label_create(list_box);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_make(0x66, 0x66, 0x99), LV_PART_MAIN);
        lv_label_set_text(l, "Nessuna rete WPA3 - tocca SCAN");
        lv_obj_add_flag(l, LV_OBJ_FLAG_FLOATING);
        lv_obj_center(l);
        return;
    }
    for (int i = 0; i < s_ap_count; i++) {
        bool sel = (i == s_target);
        lv_obj_t *card = lv_obj_create(list_box);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card,
            sel ? lv_color_make(0x10, 0x1a, 0x2a) : lv_color_make(0x16, 0x16, 0x16),
            LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card,
            sel ? lv_color_make(0x44, 0xaa, 0xff) : lv_color_make(0x33, 0x33, 0x33),
            LV_PART_MAIN);
        lv_obj_set_style_border_width(card, sel ? 2 : 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(card, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_ap_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *l1 = lv_label_create(card);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(l1, lv_color_make(0x66, 0xCC, 0xFF), LV_PART_MAIN);
        lv_label_set_text(l1, s_aps[i].ssid[0] ? s_aps[i].ssid : "(hidden)");

        char det[80];
        snprintf(det, sizeof(det), "ch %d  \xC2\xB7  %d dBm  \xC2\xB7  %02X:%02X:%02X",
                 s_aps[i].channel, (int)s_aps[i].rssi,
                 s_aps[i].bssid[3], s_aps[i].bssid[4], s_aps[i].bssid[5]);
        lv_obj_t *l2 = lv_label_create(card);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l2, lv_color_make(0x55, 0x88, 0x99), LV_PART_MAIN);
        lv_label_set_text(l2, det);
    }
}

// ─── status / buttons ────────────────────────────────────────────────────────
static void update_status()
{
    char buf[96];
    const char *txt = buf;
    lv_color_t  col = lv_color_make(0x88, 0x88, 0x99);
    switch (s_state) {
    case SST_IDLE:
        txt = "Tocca SCAN per trovare AP WPA3";
        break;
    case SST_SCANNING:
        txt = "Scansione...";
        col = lv_color_make(0xFF, 0xCC, 0x00);
        break;
    case SST_LIST:
        if (s_target >= 0)
            snprintf(buf, sizeof(buf), "Bersaglio: %s (ch %d)",
                     s_aps[s_target].ssid[0] ? s_aps[s_target].ssid : "(hidden)",
                     s_aps[s_target].channel);
        else
            snprintf(buf, sizeof(buf), "%d reti WPA3 - toccane una", s_ap_count);
        break;
    case SST_RUNNING:
        txt = "SAE Overflow attivo - swipe su per fermare";
        col = lv_color_make(0x44, 0xAA, 0xFF);
        break;
    }
    lv_label_set_text(status_label, txt);
    lv_obj_set_style_text_color(status_label, col, LV_PART_MAIN);
}

static void update_buttons()
{
    if (s_state == SST_RUNNING) {
        lv_obj_add_flag(btn_scan, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(btn_atk_lbl, "STOP");
        lv_obj_set_style_bg_color(btn_atk, lv_color_make(0x88, 0x22, 0x22), LV_PART_MAIN);
        lv_obj_align(btn_atk, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    lv_obj_clear_flag(btn_scan, LV_OBJ_FLAG_HIDDEN);
    const char *sl = (s_state == SST_SCANNING) ? "SCAN..." :
                     (s_target >= 0)           ? "RISCAN"  : "SCAN";
    lv_label_set_text(btn_scan_lbl, sl);
    lv_obj_set_style_bg_color(btn_scan, lv_color_make(0x00, 0x66, 0x99), LV_PART_MAIN);

    if (s_target >= 0 && s_state != SST_SCANNING) {
        lv_obj_clear_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(btn_atk_lbl, "FLOOD");
        lv_obj_set_style_bg_color(btn_atk, lv_color_make(0x33, 0x88, 0xCC), LV_PART_MAIN);
        lv_obj_align(btn_scan, LV_ALIGN_LEFT_MID,  6, 0);
        lv_obj_align(btn_atk,  LV_ALIGN_RIGHT_MID, -6, 0);
    } else {
        lv_obj_add_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(btn_scan, LV_ALIGN_CENTER, 0, 0);
    }
}

static void update_view()
{
    bool running = (s_state == SST_RUNNING);
    if (running) {
        lv_obj_add_flag(list_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(live_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(list_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(live_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

// ─── flow ────────────────────────────────────────────────────────────────────
static void start_scan()
{
    WiFi.mode(WIFI_STA);
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    s_state = SST_SCANNING;
}

static void on_ap_clicked(lv_event_t *e)
{
    if (s_state == SST_RUNNING) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_ap_count) return;
    s_target = idx;
    show_aps();
    update_status();
    update_buttons();
}

static void on_scan_btn(lv_event_t *)
{
    if (s_state == SST_RUNNING) return;
    start_scan();
    update_status();
    update_buttons();
}

static void on_attack_btn(lv_event_t *)
{
    if (s_state == SST_RUNNING) {
        wpa3_sae_stop();
        s_state = (s_ap_count > 0) ? SST_LIST : SST_IDLE;
    } else if (s_target >= 0) {
        if (wpa3_sae_start(s_aps[s_target].bssid, s_aps[s_target].channel))
            s_state = SST_RUNNING;
    }
    update_view();
    update_status();
    update_buttons();
}

static void on_refresh(lv_timer_t *)
{
    if (lv_screen_active() != screen) return;

    if (s_state == SST_SCANNING) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
            s_ap_count = 0;
            for (int i = 0; i < n && s_ap_count < 24; i++) {
                // Filter to WPA3-SAE-capable APs. The Arduino-ESP32 API only
                // exposes the coarse enum here (WPA3-PSK includes both pure
                // WPA3 and the WPA2/WPA3 transitional mode) — good enough,
                // because injecting SAE Commit at a transitional AP still
                // exercises its SAE stack.
                wifi_auth_mode_t am = WiFi.encryptionType(i);
                if (am != WIFI_AUTH_WPA3_PSK &&
                    am != WIFI_AUTH_WPA2_WPA3_PSK) continue;

                SAp &a = s_aps[s_ap_count++];
                strncpy(a.ssid, WiFi.SSID(i).c_str(), 32);
                a.ssid[32] = '\0';
                a.rssi    = WiFi.RSSI(i);
                a.channel = WiFi.channel(i);
                const uint8_t *b = WiFi.BSSID(i);
                if (b) memcpy(a.bssid, b, 6);
            }
            WiFi.scanDelete();
            if (s_target >= s_ap_count) s_target = -1;
            s_state = SST_LIST;
            show_aps();
        } else if (n == WIFI_SCAN_FAILED) {
            s_state = (s_ap_count > 0) ? SST_LIST : SST_IDLE;
        }
        update_status();
        update_buttons();
    } else if (s_state == SST_RUNNING) {
        lv_label_set_text_fmt(live_lbl,
            "SAE Commit frames\n\n%lu\n\ntap ch %d  \xC2\xB7  %02X:%02X:%02X",
            (unsigned long)wpa3_sae_frames_sent(),
            s_aps[s_target].channel,
            s_aps[s_target].bssid[3], s_aps[s_target].bssid[4], s_aps[s_target].bssid[5]);
    }
}

// ─── events / layout ─────────────────────────────────────────────────────────
static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        wpa3_sae_screen_stop();
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

void wpa3_sae_screen_create()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_make(0x44, 0xAA, 0xFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(title, "WPA3 SAE");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *warn = lv_label_create(screen);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(warn, lv_color_make(0x55, 0x66, 0x88), LV_PART_MAIN);
    lv_label_set_text(warn, "Solo su reti che sei autorizzato a testare");
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 50);

    status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x66, 0x99, 0xCC), LV_PART_MAIN);
    lv_label_set_text(status_label, "Tocca SCAN per trovare AP WPA3");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 74);

    lv_obj_t *btn_row = lv_obj_create(screen);
    lv_obj_set_size(btn_row, 404, 50);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(btn_row, LV_ALIGN_TOP_MID, 0, 100);

    btn_scan = make_button(btn_row, 196, 48, lv_color_make(0x00, 0x66, 0x99), &btn_scan_lbl);
    lv_obj_align(btn_scan, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_scan, on_scan_btn, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn_scan_lbl, "SCAN");

    btn_atk = make_button(btn_row, 196, 48, lv_color_make(0x33, 0x88, 0xCC), &btn_atk_lbl);
    lv_obj_align(btn_atk, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_event_cb(btn_atk, on_attack_btn, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn_atk_lbl, "FLOOD");
    lv_obj_add_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);

    list_box = lv_obj_create(screen);
    lv_obj_set_size(list_box, 404, 322);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_set_style_bg_color(list_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(list_box, lv_color_make(0x22, 0x33, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list_box, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(list_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);

    // Live "frames sent" panel while running — same footprint as the list.
    live_lbl = lv_label_create(screen);
    lv_obj_set_style_text_font(live_lbl, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(live_lbl, lv_color_make(0x66, 0xCC, 0xFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(live_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(live_lbl, "SAE Commit\n\n0");
    lv_obj_align(live_lbl, LV_ALIGN_CENTER, 0, 40);
    lv_obj_add_flag(live_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_refresh, 400, NULL);
}

void wpa3_sae_screen_show()
{
    if (!screen) wpa3_sae_screen_create();
    if (s_ap_count > 0) { s_state = SST_LIST; show_aps(); }
    else                { s_state = SST_IDLE; lv_obj_clean(list_box); }
    update_view();
    update_status();
    update_buttons();
    lv_scr_load(screen);
}

void wpa3_sae_screen_stop()
{
    wpa3_sae_stop();
    s_state = (s_ap_count > 0) ? SST_LIST : SST_IDLE;
    update_view();
}

bool wpa3_sae_screen_is_active() { return screen && lv_screen_active() == screen; }
