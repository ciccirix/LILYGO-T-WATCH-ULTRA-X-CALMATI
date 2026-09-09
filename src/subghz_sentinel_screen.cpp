#include "subghz_sentinel_screen.h"
#include <LilyGoLib.h>          // provides the global RadioLib `radio` (SX1262)
#include "meshtastic.h"         // meshtastic_set_active() to release the radio
#include <string.h>
#include <stdio.h>

void tools_screen_show();

#define SENTINEL_FREQ_MHZ 868.0   // EU band this module is tuned for

static lv_obj_t *s_scr, *rssi_label, *status_label, *floor_label, *bar_fill;
static bool s_pending = false;
static bool s_radio_ok = false;

// Calibration: average the first WARMUP samples into a real baseline; detect
// only a SUSTAINED elevation above THAT (so a constant getRSSI offset can't
// trigger a phantom alarm — the baseline just absorbs it).
#define SENT_WARMUP 24        // ~3.6 s at 150 ms
static float  s_base = 0;     // calibrated baseline (dBm)
static int    s_warm = 0;     // warmup samples collected
static double s_warm_sum = 0;
static int    s_hi_run = 0;
static uint32_t s_dbg_ms = 0;

static int rssi_to_pct(float r) {   // map -120..-40 dBm -> 0..100
    int p = (int)((r + 120.0f) * 100.0f / 80.0f);
    return p < 2 ? 2 : (p > 100 ? 100 : p);
}

static void sentinel_begin()
{
    meshtastic_set_active(false);
    int st = radio.setFrequency(SENTINEL_FREQ_MHZ);
    radio.startReceive();
    s_radio_ok = (st == 0 /* RADIOLIB_ERR_NONE */);
    s_warm = 0; s_warm_sum = 0; s_base = 0; s_hi_run = 0;
}

static void on_refresh(lv_timer_t *)
{
    if (lv_scr_act() != s_scr) return;
    if (s_pending) { s_pending = false; sentinel_begin(); }
    if (!s_radio_ok) { lv_label_set_text(status_label, "radio non pronta"); return; }

    float r = radio.getRSSI(false);         // instantaneous channel RSSI

    // Debug: print the raw value ~1/s so we can calibrate against reality.
    if (millis() - s_dbg_ms > 1000) {
        s_dbg_ms = millis();
        Serial.printf("[SENT] rssi=%.1f base=%.1f warm=%d hi=%d\n", r, s_base, s_warm, s_hi_run);
    }

    const char *st; lv_color_t col;

    if (s_warm < SENT_WARMUP) {             // still calibrating
        s_warm_sum += r; s_warm++;
        s_base = (float)(s_warm_sum / s_warm);
        st = "calibrazione..."; col = lv_color_make(0x00,0xcc,0xff);
        s_hi_run = 0;
    } else {
        float delta = r - s_base;           // how far ABOVE the calibrated floor
        // Loud only if clearly above the baseline AND not just sensor jitter.
        if (delta > 15.0f) s_hi_run++; else s_hi_run = 0;

        if (s_hi_run > 20)      { st = "JAMMING!"; col = lv_color_make(0xff,0x33,0x33); }
        else if (delta > 8.0f)  { st = "RUMORE";   col = lv_color_make(0xff,0xbb,0x00); }
        else                    { st = "QUIETE";   col = lv_color_make(0x33,0xcc,0x55); }
    }

    lv_label_set_text_fmt(rssi_label, "%d dBm", (int)r);
    lv_label_set_text_fmt(floor_label, "fondo %d dBm", (int)s_base);
    lv_label_set_text(status_label, st);
    lv_obj_set_style_text_color(status_label, col, LV_PART_MAIN);
    lv_obj_set_width(bar_fill, lv_pct(rssi_to_pct(r)));
    lv_obj_set_style_bg_color(bar_fill, col, LV_PART_MAIN);
}

void subghz_sentinel_screen_stop()
{
    if (s_radio_ok) { radio.standby(); s_radio_ok = false; }
    // leave Meshtastic paused (radio in standby); user re-enables it if needed.
}

static void on_gesture(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_event_get_indev(e)) == LV_DIR_TOP) {
        subghz_sentinel_screen_stop();
        tools_screen_show();
    }
}

void subghz_sentinel_screen_create()
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_color(title, lv_color_make(0x00,0xff,0xaa), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_label_set_text(title, "Sentinel");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *sub = lv_label_create(s_scr);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_make(0x66,0x99,0x88), LV_PART_MAIN);
    lv_label_set_text(sub, "jamming SubGHz 868 MHz - pausa la mesh");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 78);

    status_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x33,0xcc,0x55), LV_PART_MAIN);
    lv_label_set_text(status_label, "...");
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -50);

    rssi_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(rssi_label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(rssi_label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(rssi_label, "-- dBm");
    lv_obj_align(rssi_label, LV_ALIGN_CENTER, 0, 10);

    floor_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(floor_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(floor_label, lv_color_make(0x88,0x88,0x88), LV_PART_MAIN);
    lv_label_set_text(floor_label, "fondo -- dBm");
    lv_obj_align(floor_label, LV_ALIGN_CENTER, 0, 48);

    lv_obj_t *bar_bg = lv_obj_create(s_scr);
    lv_obj_set_size(bar_bg, 360, 24);
    lv_obj_align(bar_bg, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_style_bg_color(bar_bg, lv_color_make(0x22,0x22,0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_bg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_bg, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_bg, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar_bg, 0, LV_PART_MAIN);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);
    bar_fill = lv_obj_create(bar_bg);
    lv_obj_set_size(bar_fill, lv_pct(2), 24);
    lv_obj_align(bar_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(bar_fill, lv_color_make(0x33,0xcc,0x55), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_fill, 12, LV_PART_MAIN);
    lv_obj_clear_flag(bar_fill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(s_scr, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_refresh, 150, NULL);
}

void subghz_sentinel_screen_show()
{
    lv_scr_load(s_scr);
    s_pending = true;
}

bool subghz_sentinel_screen_is_active() { return lv_scr_act() == s_scr; }
