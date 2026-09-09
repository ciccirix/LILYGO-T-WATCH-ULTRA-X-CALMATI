#include "badusb_screen.h"
#include "badusb.h"
#include <LilyGoLib.h>
#include <string.h>
#include <stdio.h>

// Defined in tools_screen.cpp
void tools_screen_show();

struct Payload { const char *name; const char *desc; const char *script; };

// Benign demo payloads. Bring your own for real engagements (STRING/DELAY/GUI…).
static const Payload PAYLOADS[] = {
    { "Type test line", "types one line where focused",
      "REM benign demo\nSTRING BadUSB from T-Watch Ultra\nENTER\n" },
    { "Win: Notepad hello", "Win+R notepad, type a line",
      "GUI r\nDELAY 500\nSTRING notepad\nENTER\nDELAY 900\n"
      "STRING Hello from the T-Watch Ultra BadUSB.\nENTER\n" },
    { "Win: Lock screen", "Win+L (harmless)",
      "REM harmless demo\nGUI l\n" },
    { "Mac: TextEdit note", "Spotlight -> TextEdit, type",
      "GUI SPACE\nDELAY 500\nSTRING TextEdit\nENTER\nDELAY 1200\n"
      "STRING Hello from the T-Watch Ultra.\n" },
};
static const int N_PAYLOADS = sizeof(PAYLOADS) / sizeof(PAYLOADS[0]);

static lv_obj_t *screen;
static lv_obj_t *status_label;
static lv_obj_t *list_box;
static lv_obj_t *btn_run, *btn_run_lbl;

static int  s_sel = -1;
static bool s_was_running = false;

static void on_row_clicked(lv_event_t *e);

static void build_list()
{
    lv_obj_clean(list_box);
    for (int i = 0; i < N_PAYLOADS; i++) {
        bool sel = (i == s_sel);
        lv_obj_t *card = lv_obj_create(list_box);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card,
            sel ? lv_color_make(0x10, 0x28, 0x10) : lv_color_make(0x16, 0x16, 0x16), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(card,
            sel ? lv_color_make(0x33, 0xcc, 0x33) : lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
        lv_obj_set_style_border_width(card, sel ? 2 : 1, LV_PART_MAIN);
        lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_layout(card, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *l1 = lv_label_create(card);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(l1, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
        lv_label_set_text(l1, PAYLOADS[i].name);

        lv_obj_t *l2 = lv_label_create(card);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l2, lv_color_make(0x00, 0x99, 0x00), LV_PART_MAIN);
        lv_label_set_text(l2, PAYLOADS[i].desc);
    }
}

static void update_status()
{
    char buf[64];
    if (badusb_is_running()) {
        snprintf(buf, sizeof(buf), "Injecting... %d%%", badusb_progress());
        lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0x44, 0x44), LV_PART_MAIN);
    } else if (s_sel >= 0) {
        snprintf(buf, sizeof(buf), "Ready: %s", PAYLOADS[s_sel].name);
        lv_obj_set_style_text_color(status_label, lv_color_make(0x00, 0x99, 0x00), LV_PART_MAIN);
    } else {
        snprintf(buf, sizeof(buf), "Pick a payload, plug into target");
        lv_obj_set_style_text_color(status_label, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    }
    lv_label_set_text(status_label, buf);
}

static void update_buttons()
{
    if (badusb_is_running()) {
        lv_label_set_text(btn_run_lbl, "STOP");
        lv_obj_set_style_bg_color(btn_run, lv_color_make(0x88, 0x22, 0x22), LV_PART_MAIN);
        lv_obj_clear_flag(btn_run, LV_OBJ_FLAG_HIDDEN);
    } else if (s_sel >= 0) {
        lv_label_set_text(btn_run_lbl, "RUN");
        lv_obj_set_style_bg_color(btn_run, lv_color_make(0xCC, 0x33, 0x33), LV_PART_MAIN);
        lv_obj_clear_flag(btn_run, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(btn_run, LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_row_clicked(lv_event_t *e)
{
    if (badusb_is_running()) return;
    s_sel = (int)(intptr_t)lv_event_get_user_data(e);
    build_list();
    update_status();
    update_buttons();
}

static void on_run_btn(lv_event_t *)
{
    if (badusb_is_running()) {
        badusb_stop();
    } else if (s_sel >= 0) {
        badusb_run(PAYLOADS[s_sel].script);
    }
    update_status();
    update_buttons();
}

static void on_refresh(lv_timer_t *)
{
    if (lv_screen_active() != screen) return;
    bool r = badusb_is_running();
    if (r || s_was_running != r) {   // keep %/buttons fresh while (and just after) running
        update_status();
        update_buttons();
    }
    s_was_running = r;
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        badusb_screen_stop();
        tools_screen_show();
    }
}

void badusb_screen_create()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "BadUSB");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *warn = lv_label_create(screen);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(warn, lv_color_make(0x88, 0x55, 0x55), LV_PART_MAIN);
    lv_label_set_text(warn, "Types keystrokes into the plugged-in PC - authorised only");
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 58);

    status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(status_label, "Pick a payload, plug into target");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 82);

    btn_run = lv_obj_create(screen);
    lv_obj_set_size(btn_run, 200, 48);
    lv_obj_align(btn_run, LV_ALIGN_TOP_MID, 0, 108);
    lv_obj_set_style_radius(btn_run, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_run, lv_color_make(0xCC, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_run, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn_run, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_run, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_run, on_run_btn, LV_EVENT_CLICKED, NULL);
    btn_run_lbl = lv_label_create(btn_run);
    lv_obj_set_style_text_color(btn_run_lbl, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(btn_run_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(btn_run_lbl, "RUN");
    lv_obj_center(btn_run_lbl);
    lv_obj_add_flag(btn_run, LV_OBJ_FLAG_HIDDEN);

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
    lv_timer_create(on_refresh, 300, NULL);
}

void badusb_screen_show()
{
    badusb_begin();          // bring up the composite USB device (idle keyboard)
    build_list();
    update_status();
    update_buttons();
    lv_scr_load(screen);
}

void badusb_screen_stop()
{
    badusb_stop();
}

bool badusb_screen_is_active()
{
    return lv_screen_active() == screen;
}
