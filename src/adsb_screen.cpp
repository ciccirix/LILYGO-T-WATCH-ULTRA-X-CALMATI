#include "adsb_screen.h"
#include "adsb.h"
#include "tools_screen.h"     // tools_screen_show() for the back-gesture
#include <LilyGoLib.h>        // instance.vibrator()
#include <lvgl.h>
#include <math.h>
#include <stdio.h>

// ---- geometry (410x502 AMOLED) ---------------------------------------------
#define SCR_W   410
#define SCR_H   502
#define CX      205
#define CY      258          // radar centre, nudged down to clear the title
#define RR      182          // outer ring radius (px)
#define N_RINGS 4
#define MAX_BLIP 24          // cap plotted aircraft to keep the object count sane

static lv_obj_t *s_screen   = nullptr;
static lv_obj_t *s_rings[N_RINGS];
static lv_obj_t *s_ring_lbl[N_RINGS];
static lv_obj_t *s_beam     = nullptr;
static lv_obj_t *s_center   = nullptr;
static lv_obj_t *s_blips    = nullptr;   // container, rebuilt on new data
static lv_obj_t *s_status   = nullptr;
static lv_obj_t *s_range_lbl = nullptr;
static lv_timer_t *s_timer  = nullptr;
static bool  s_active = false;

static lv_point_precise_t s_beam_pts[2];
static float s_sweep = 0;                // beam angle (deg)
static uint32_t s_last_seq = 0;          // adsb_seq() we last painted from
static int   s_prev_count = 0;

// Altitude -> colour band (matches the "cool = high" convention pilots expect).
static lv_color_t alt_color(const AdsbAircraft *a)
{
    if (a->on_ground || a->alt_ft < 0) return lv_color_make(0x88, 0x88, 0x88);
    if (a->alt_ft < 10000)  return lv_color_make(0x00, 0xFF, 0x66);
    if (a->alt_ft < 20000)  return lv_color_make(0xCC, 0xFF, 0x00);
    if (a->alt_ft < 33000)  return lv_color_make(0xFF, 0x99, 0x00);
    return lv_color_make(0x33, 0xCC, 0xFF);
}

static void set_range_labels()
{
    int range = adsb_range_nm();
    for (int i = 0; i < N_RINGS; i++) {
        int nm = range * (i + 1) / N_RINGS;
        lv_label_set_text_fmt(s_ring_lbl[i], "%d", nm);
        // Sit each label just inside the top of its ring.
        int r = RR * (i + 1) / N_RINGS;
        lv_obj_set_pos(s_ring_lbl[i], CX + 4, CY - r + 1);
    }
    lv_label_set_text_fmt(s_range_lbl, LV_SYMBOL_GPS "  %d nm", range);
}

// Rebuild the aircraft dots from a fresh snapshot.
static void draw_blips()
{
    lv_obj_clean(s_blips);

    AdsbAircraft ac[MAX_BLIP];
    int n = adsb_get(ac, MAX_BLIP);
    int range = adsb_range_nm();

    for (int i = 0; i < n; i++) {
        float r_px = (ac[i].dist_nm / (float)range) * RR;
        if (r_px > RR) continue;                       // outside the outer ring
        float ang = ac[i].bearing_deg * (float)DEG_TO_RAD;
        int x = CX + (int)lroundf(r_px * sinf(ang));
        int y = CY - (int)lroundf(r_px * cosf(ang));
        lv_color_t col = alt_color(&ac[i]);

        lv_obj_t *dot = lv_obj_create(s_blips);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, 5, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, col, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(dot, x - 5, y - 5);

        // Label the nearest handful only, so a busy sky stays legible.
        if (i < 8 && ac[i].flight[0]) {
            lv_obj_t *lbl = lv_label_create(s_blips);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
            lv_obj_set_style_text_color(lbl, col, LV_PART_MAIN);
            lv_label_set_text(lbl, ac[i].flight);
            lv_obj_set_pos(lbl, x + 8, y - 7);
        }
    }
}

static void refresh_status()
{
    int n = adsb_count();
    lv_color_t c;
    switch (adsb_status()) {
        case ADSB_OK:       c = lv_color_make(0x00, 0xCC, 0x66); break;
        case ADSB_FETCH:
        case ADSB_WIFI:     c = lv_color_make(0x33, 0xBB, 0xFF); break;
        case ADSB_NOGPS:    c = lv_color_make(0xFF, 0xCC, 0x00); break;
        default:            c = lv_color_make(0xFF, 0x44, 0x44); break;
    }
    lv_obj_set_style_text_color(s_status, c, LV_PART_MAIN);

    if (adsb_status() == ADSB_OK) {
        uint32_t age = adsb_age_ms() / 1000;
        lv_label_set_text_fmt(s_status, "%d aircraft  ·  %s  ·  %us ago",
                              n, adsb_status_text(), (unsigned)age);
    } else {
        lv_label_set_text_fmt(s_status, "%s", adsb_status_text());
    }
}

static void on_timer(lv_timer_t *)
{
    if (!s_active) return;

    // Rotating sweep beam (advance 3 deg/tick @ 60ms -> slower, ~one turn / 7 s).
    s_sweep = fmodf(s_sweep + 3.0f, 360.0f);
    float ar = s_sweep * (float)DEG_TO_RAD;
    s_beam_pts[0].x = CX;
    s_beam_pts[0].y = CY;
    s_beam_pts[1].x = CX + (int)lroundf(RR * sinf(ar));
    s_beam_pts[1].y = CY - (int)lroundf(RR * cosf(ar));
    lv_line_set_points(s_beam, s_beam_pts, 2);

    // Repaint aircraft only when a new fetch landed.
    uint32_t seq = adsb_seq();
    if (seq != s_last_seq) {
        s_last_seq = seq;
        draw_blips();
        int n = adsb_count();
        if (n > 0 && s_prev_count == 0) instance.vibrator();   // first contact
        s_prev_count = n;
    }
    refresh_status();
}

static void on_range(lv_event_t *)
{
    adsb_cycle_range();
    set_range_labels();
    draw_blips();           // re-scale immediately with what we already have
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        s_active = false;
        adsb_stop();
        tools_screen_show();
    }
}

static lv_obj_t *make_ring(int r)
{
    lv_obj_t *o = lv_obj_create(s_screen);
    lv_obj_set_size(o, r * 2, r * 2);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, lv_color_make(0x00, 0x66, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, CX - r, CY - r);
    return o;
}

static lv_obj_t *make_crosshair(bool vertical)
{
    lv_obj_t *o = lv_obj_create(s_screen);
    if (vertical) { lv_obj_set_size(o, 1, RR * 2); lv_obj_set_pos(o, CX, CY - RR); }
    else          { lv_obj_set_size(o, RR * 2, 1); lv_obj_set_pos(o, CX - RR, CY); }
    lv_obj_set_style_bg_color(o, lv_color_make(0x00, 0x44, 0x00), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void adsb_screen_create()
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
    lv_label_set_text(title, LV_SYMBOL_GPS "  ADS-B RADAR");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    // Range rings (outer first so labels/crosshair land on top).
    for (int i = N_RINGS - 1; i >= 0; i--)
        s_rings[i] = make_ring(RR * (i + 1) / N_RINGS);
    make_crosshair(true);
    make_crosshair(false);

    for (int i = 0; i < N_RINGS; i++) {
        s_ring_lbl[i] = lv_label_create(s_screen);
        lv_obj_set_style_text_font(s_ring_lbl[i], &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_ring_lbl[i], lv_color_make(0x00, 0x77, 0x00), LV_PART_MAIN);
    }

    // Rotating sweep beam.
    s_beam_pts[0].x = CX; s_beam_pts[0].y = CY;
    s_beam_pts[1].x = CX; s_beam_pts[1].y = CY - RR;
    s_beam = lv_line_create(s_screen);
    lv_line_set_points(s_beam, s_beam_pts, 2);
    lv_obj_set_style_line_width(s_beam, 2, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_beam, lv_color_make(0x00, 0xFF, 0x44), LV_PART_MAIN);
    lv_obj_set_style_line_opa(s_beam, LV_OPA_70, LV_PART_MAIN);

    // Aircraft container on top of the grid.
    s_blips = lv_obj_create(s_screen);
    lv_obj_set_size(s_blips, SCR_W, SCR_H);
    lv_obj_set_pos(s_blips, 0, 0);
    lv_obj_set_style_bg_opa(s_blips, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_blips, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_blips, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_blips, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_blips, LV_OBJ_FLAG_CLICKABLE);

    // Own position marker.
    s_center = lv_obj_create(s_screen);
    lv_obj_set_size(s_center, 12, 12);
    lv_obj_set_style_radius(s_center, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_center, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_center, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_center, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_center, 2, LV_PART_MAIN);
    lv_obj_clear_flag(s_center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_center, CX - 6, CY - 6);

    // Status line above the button.
    s_status = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status, lv_color_make(0x00, 0xCC, 0x66), LV_PART_MAIN);
    lv_label_set_text(s_status, "starting...");
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -80);

    // RANGE button (centre-bottom, away from the untouchable corners).
    lv_obj_t *rbtn = lv_obj_create(s_screen);
    lv_obj_set_size(rbtn, 180, 46);
    lv_obj_set_style_radius(rbtn, 23, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rbtn, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rbtn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(rbtn, lv_color_make(0x00, 0x66, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(rbtn, 1, LV_PART_MAIN);
    lv_obj_clear_flag(rbtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(rbtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(rbtn, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_obj_add_event_cb(rbtn, on_range, LV_EVENT_CLICKED, NULL);
    s_range_lbl = lv_label_create(rbtn);
    lv_obj_set_style_text_font(s_range_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_range_lbl, lv_color_make(0x00, 0xCC, 0x00), LV_PART_MAIN);
    lv_obj_center(s_range_lbl);
    lv_obj_add_flag(s_range_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    set_range_labels();

    s_timer = lv_timer_create(on_timer, 60, NULL);
    lv_timer_pause(s_timer);
}

void adsb_screen_show()
{
    if (!s_screen) adsb_screen_create();
    s_active = true;
    s_prev_count = 0;
    s_last_seq = adsb_seq();
    lv_obj_clean(s_blips);
    set_range_labels();
    adsb_start();
    if (s_timer) lv_timer_resume(s_timer);
    refresh_status();
    lv_scr_load(s_screen);
}

bool adsb_screen_is_active() { return s_active; }
