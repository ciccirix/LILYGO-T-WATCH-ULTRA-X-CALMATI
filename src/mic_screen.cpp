#include "mic_screen.h"
#include "mic_rec.h"
#include "tools_screen.h"     // tools_screen_show() for the back-gesture
#include <lvgl.h>
#include <stdio.h>

// Big 96px timer font, generated in lv_font_montserrat_clock_96.c (also used by
// the clock face).
extern "C" const lv_font_t lv_font_montserrat_clock_96;

static lv_obj_t *s_screen   = nullptr;
static lv_obj_t *s_time     = nullptr;   // MM:SS
static lv_obj_t *s_meter    = nullptr;   // VU bar (foreground)
static lv_obj_t *s_status   = nullptr;
static lv_obj_t *s_recbtn   = nullptr;
static lv_obj_t *s_reclbl   = nullptr;
static lv_timer_t *s_timer  = nullptr;
static bool  s_active = false;

#define METER_W  320

static void paint_button()
{
    bool rec = mic_rec_is_recording();
    lv_color_t c = rec ? lv_color_make(0xFF, 0x33, 0x33)
                       : lv_color_make(0x00, 0xCC, 0x00);
    lv_obj_set_style_border_color(s_recbtn, c, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_reclbl, c, LV_PART_MAIN);
    lv_label_set_text(s_reclbl, rec ? LV_SYMBOL_STOP "  STOP"
                                    : LV_SYMBOL_AUDIO "  REC");
}

static void refresh()
{
    uint32_t ms = mic_rec_elapsed_ms();
    unsigned s = ms / 1000;
    lv_label_set_text_fmt(s_time, "%02u:%02u", s / 60, s % 60);
    lv_obj_set_style_text_color(s_time,
        mic_rec_is_recording() ? lv_color_make(0xFF, 0x33, 0x33)
                               : lv_color_make(0x00, 0xCC, 0x00),
        LV_PART_MAIN);

    int lvl = mic_rec_level();                 // 0..100
    int w = METER_W * lvl / 100;
    if (w < 2) w = 2;
    lv_obj_set_width(s_meter, w);
    // Green low, amber loud, red clipping — a quick input-gain read.
    lv_color_t mc = lvl > 85 ? lv_color_make(0xFF, 0x33, 0x33)
                  : lvl > 60 ? lv_color_make(0xFF, 0xCC, 0x00)
                             : lv_color_make(0x00, 0xCC, 0x66);
    lv_obj_set_style_bg_color(s_meter, mc, LV_PART_MAIN);

    size_t kb = mic_rec_bytes() / 1024;
    const char *path = mic_rec_last_path();
    const char *name = path[0] ? path : "";
    // Show just the basename to keep it short.
    const char *slash = name;
    for (const char *p = name; *p; p++) if (*p == '/') slash = p + 1;
    lv_label_set_text_fmt(s_status, "%s%s%s  ·  %u KB",
        mic_rec_status_text(),
        slash[0] ? "  ·  " : "", slash, (unsigned)kb);

    paint_button();
}

static void on_timer(lv_timer_t *)
{
    if (s_active) refresh();
}

static void on_rec(lv_event_t *)
{
    mic_rec_toggle();
    refresh();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        s_active = false;
        // Deliberately DO NOT stop recording — it keeps running in the
        // background (mic_rec has its own hard-cap auto-stop).
        tools_screen_show();
    }
}

void mic_screen_create()
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
    lv_label_set_text(title, LV_SYMBOL_AUDIO "  MIC RECORDER");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    // Big elapsed timer.
    s_time = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_clock_96, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_time, lv_color_make(0x00, 0xCC, 0x00), LV_PART_MAIN);
    lv_label_set_text(s_time, "00:00");
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 120);

    // VU meter: dim track with a coloured fill.
    lv_obj_t *track = lv_obj_create(s_screen);
    lv_obj_set_size(track, METER_W, 26);
    lv_obj_set_style_radius(track, 13, LV_PART_MAIN);
    lv_obj_set_style_bg_color(track, lv_color_make(0x11, 0x22, 0x11), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(track, lv_color_make(0x00, 0x55, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(track, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(track, 0, LV_PART_MAIN);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(track, LV_ALIGN_CENTER, 0, 40);

    s_meter = lv_obj_create(track);
    lv_obj_set_size(s_meter, 2, 26);
    lv_obj_set_style_radius(s_meter, 13, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_meter, lv_color_make(0x00, 0xCC, 0x66), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_meter, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_meter, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_meter, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_meter, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_meter, LV_ALIGN_LEFT_MID, 0, 0);

    // Status / filename / size.
    s_status = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status, lv_color_make(0x00, 0xAA, 0x55), LV_PART_MAIN);
    lv_label_set_text(s_status, "idle");
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 90);

    // REC / STOP toggle (centre-bottom, clear of the corners).
    s_recbtn = lv_obj_create(s_screen);
    lv_obj_set_size(s_recbtn, 200, 60);
    lv_obj_set_style_radius(s_recbtn, 30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_recbtn, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_recbtn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_recbtn, 2, LV_PART_MAIN);
    lv_obj_clear_flag(s_recbtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_recbtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_recbtn, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_add_event_cb(s_recbtn, on_rec, LV_EVENT_CLICKED, NULL);
    s_reclbl = lv_label_create(s_recbtn);
    lv_obj_set_style_text_font(s_reclbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(s_reclbl);
    lv_obj_add_flag(s_reclbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    paint_button();
    s_timer = lv_timer_create(on_timer, 60, nullptr);
    lv_timer_pause(s_timer);
}

void mic_screen_show()
{
    if (!s_screen) mic_screen_create();
    s_active = true;
    if (s_timer) lv_timer_resume(s_timer);
    refresh();
    lv_scr_load(s_screen);
}

bool mic_screen_is_active() { return s_active; }
