#include "mifare_screen.h"
#include "mifare.h"
#include <LilyGoLib.h>
#include <string.h>
#include <stdio.h>

// Defined in tools_screen.cpp
void tools_screen_show();

static lv_obj_t *screen;
static lv_obj_t *status_label;
static lv_obj_t *info_label;
static lv_obj_t *btn_rescan, *btn_rescan_lbl;
static lv_obj_t *btn_dict, *btn_dict_lbl;

static bool s_shown_card = false;
static bool s_dict_was_running = false;

static void render_card()
{
    MifareCard c;
    if (!mifare_get_card(&c)) return;

    char uid[36] = {0};
    int p = 0;
    for (int i = 0; i < c.uidlen && p < (int)sizeof(uid) - 3; i++)
        p += snprintf(uid + p, sizeof(uid) - p, "%02X%s", c.uid[i], i < c.uidlen - 1 ? ":" : "");

    char buf[256];
    if (c.is_classic)
        snprintf(buf, sizeof(buf),
                 "Type: %s\nUID: %s\nSAK: 0x%02X   ATQA: %02X%02X\nSectors: %d\n\n"
                 "Tap DICT to try the key dictionary\n(real crypto1 auth; experimental).",
                 c.type, uid, c.sak, c.atqa[0], c.atqa[1], c.sectors);
    else
        snprintf(buf, sizeof(buf),
                 "Type: %s\nUID: %s\nSAK: 0x%02X   ATQA: %02X%02X\n\n"
                 "(not a Mifare Classic - no\nsectors to crack)",
                 c.type, uid, c.sak, c.atqa[0], c.atqa[1]);
    lv_label_set_text(info_label, buf);
    lv_obj_set_style_text_color(info_label,
        c.is_classic ? lv_color_make(0x00, 0xFF, 0x00) : lv_color_make(0xcc, 0xcc, 0x00),
        LV_PART_MAIN);
}

static void render_results()
{
    char buf[512];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "CRACKED %d / %d sectors\n\n",
                  mifare_dict_found(), mifare_dict_total());
    for (int s = 0; s < mifare_dict_total() && n < (int)sizeof(buf) - 40; s++) {
        uint8_t kt, key[6];
        if (mifare_dict_key(s, &kt, key)) {
            n += snprintf(buf + n, sizeof(buf) - n,
                          "S%02d %c %02X%02X%02X%02X%02X%02X\n", s, kt ? 'B' : 'A',
                          key[0], key[1], key[2], key[3], key[4], key[5]);
        }
    }
    if (mifare_dict_found() == 0)
        n += snprintf(buf + n, sizeof(buf) - n, "(no keys from the dictionary)");
    lv_label_set_text(info_label, buf);
    lv_obj_set_style_text_color(info_label, lv_color_make(0x00, 0xFF, 0x88), LV_PART_MAIN);
}

static void update_status()
{
    if (mifare_dict_running()) {
        char b[64];
        snprintf(b, sizeof(b), "Cracking %d%%  found %d", mifare_dict_progress(), mifare_dict_found());
        lv_label_set_text(status_label, b);
        lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0x44, 0x44), LV_PART_MAIN);
    } else if (mifare_have_card()) {
        lv_label_set_text(status_label, "Card read");
        lv_obj_set_style_text_color(status_label, lv_color_make(0x00, 0x99, 0x00), LV_PART_MAIN);
    } else {
        lv_label_set_text(status_label, "Hold a card near the watch");
        lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
    }
}

static void update_buttons()
{
    MifareCard c;
    bool classic = mifare_get_card(&c) && c.is_classic;
    if (mifare_dict_running()) {
        lv_obj_clear_flag(btn_dict, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(btn_dict_lbl, "STOP");
        lv_obj_set_style_bg_color(btn_dict, lv_color_make(0x88, 0x22, 0x22), LV_PART_MAIN);
    } else if (classic) {
        lv_obj_clear_flag(btn_dict, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(btn_dict_lbl, "DICT");
        lv_obj_set_style_bg_color(btn_dict, lv_color_make(0xCC, 0x33, 0x33), LV_PART_MAIN);
    } else {
        lv_obj_add_flag(btn_dict, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_rescan_btn(lv_event_t *)
{
    if (mifare_dict_running()) return;
    mifare_forget_card();
    s_shown_card = false;
    lv_label_set_text(info_label, "");
    update_status();
    update_buttons();
}

static void on_dict_btn(lv_event_t *)
{
    if (mifare_dict_running()) {
        mifare_dict_stop();
    } else {
        mifare_dict_start();
    }
    update_status();
    update_buttons();
}

static void on_refresh(lv_timer_t *)
{
    if (lv_screen_active() != screen) return;

    bool running = mifare_dict_running();
    if (running) {
        update_status();                 // live % / found
        s_dict_was_running = true;
        return;
    }
    if (s_dict_was_running) {             // just finished -> show the recovered keys
        s_dict_was_running = false;
        render_results();
        update_status();
        update_buttons();
        return;
    }

    if (mifare_have_card() && !s_shown_card) {
        s_shown_card = true;
        render_card();
        update_status();
        update_buttons();
    } else if (!mifare_have_card() && s_shown_card) {
        s_shown_card = false;
        update_status();
        update_buttons();
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        mifare_screen_stop();
        tools_screen_show();
    }
}

static lv_obj_t *make_btn(lv_obj_t *parent, lv_color_t bg, lv_obj_t **label_out)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, 196, 52);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, bg, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(l);
    if (label_out) *label_out = l;
    return b;
}

void mifare_screen_create()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "Mifare");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
    lv_label_set_text(status_label, "Hold a card near the watch");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 66);

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, 404, 292);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 98);
    lv_obj_set_style_bg_color(panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_make(0x00, 0x33, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 12, LV_PART_MAIN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);

    info_label = lv_label_create(panel);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(info_label, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
    lv_label_set_long_mode(info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info_label, lv_pct(100));
    lv_label_set_text(info_label, "");

    btn_rescan = make_btn(screen, lv_color_make(0x00, 0x88, 0xCC), &btn_rescan_lbl);
    lv_obj_align(btn_rescan, LV_ALIGN_BOTTOM_LEFT, 6, -14);
    lv_obj_add_event_cb(btn_rescan, on_rescan_btn, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn_rescan_lbl, "RESCAN");

    btn_dict = make_btn(screen, lv_color_make(0xCC, 0x33, 0x33), &btn_dict_lbl);
    lv_obj_align(btn_dict, LV_ALIGN_BOTTOM_RIGHT, -6, -14);
    lv_obj_add_event_cb(btn_dict, on_dict_btn, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn_dict_lbl, "DICT");
    lv_obj_add_flag(btn_dict, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_refresh, 300, NULL);
}

void mifare_screen_show()
{
    s_shown_card = false;
    s_dict_was_running = false;
    lv_label_set_text(info_label, "");
    mifare_reader_start();
    update_status();
    update_buttons();
    lv_scr_load(screen);
}

void mifare_screen_stop()
{
    mifare_dict_stop();
    mifare_reader_stop();
}

bool mifare_screen_is_active()
{
    return lv_screen_active() == screen;
}

void mifare_screen_worker()
{
    if (!mifare_screen_is_active()) return;
    mifare_reader_worker();
}
