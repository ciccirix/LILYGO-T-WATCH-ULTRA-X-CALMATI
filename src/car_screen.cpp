#include "car_screen.h"
#include "car_finder.h"
#include "gps_screen.h"
#include "tools_screen.h"
#include <lvgl.h>
#include <LilyGoLib.h>
#include <math.h>
#include <stdio.h>

// Opens the Meshtastic map centred on a point (added in map_screen.cpp).
void map_screen_focus(double lat, double lon);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static lv_obj_t *s_screen   = nullptr;
static lv_obj_t *s_status   = nullptr;   // top line: GPS / mode
static lv_obj_t *s_arrow    = nullptr;   // rotating direction arrow (lv_line)
static lv_obj_t *s_dist     = nullptr;   // big distance number
static lv_obj_t *s_hint     = nullptr;   // "cammina per orientare" / coords
static lv_obj_t *s_save_btn = nullptr;   // SALVA / AGGIORNA
static lv_obj_t *s_save_lbl = nullptr;
static lv_obj_t *s_map_btn  = nullptr;   // MAPPA (only when saved)
static lv_obj_t *s_clr_btn  = nullptr;   // CANCELLA (only when saved)
static lv_timer_t *s_timer  = nullptr;
static bool      s_active   = false;

// Arrow outline (points "up" = -y), centred at the origin; rotated per refresh.
#define ARROW_CX 205
#define ARROW_CY 196
#define ARROW_N  8
static const lv_point_precise_t arrow_base[ARROW_N] = {
    {0, -72}, {32, -20}, {13, -20}, {13, 60}, {-13, 60}, {-13, -20}, {-32, -20}, {0, -72}
};
static lv_point_precise_t s_arrow_pts[ARROW_N];

static void set_arrow_angle(double deg)
{
    double a = deg * M_PI / 180.0, ca = cos(a), sa = sin(a);
    for (int i = 0; i < ARROW_N; i++) {
        double px = (double)arrow_base[i].x, py = (double)arrow_base[i].y;
        s_arrow_pts[i].x = (lv_value_precise_t)(ARROW_CX + px * ca - py * sa);
        s_arrow_pts[i].y = (lv_value_precise_t)(ARROW_CY + px * sa + py * ca);
    }
    lv_line_set_points(s_arrow, s_arrow_pts, ARROW_N);
}

static void show(lv_obj_t *o, bool v)
{
    if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void refresh()
{
    bool lock = gps_screen_has_lock() && instance.gps.location.isValid();
    double clat, clon;
    bool has = car_finder_get(&clat, &clon);

    if (!has) {
        // Nothing parked yet — just the SAVE prompt.
        show(s_arrow, false); show(s_dist, false); show(s_map_btn, false); show(s_clr_btn, false);
        show(s_hint, true);
        lv_label_set_text(s_save_lbl, LV_SYMBOL_GPS "  SALVA POSIZIONE AUTO");
        lv_label_set_text(s_hint, "Parcheggi? Premi per fissare\nqui la posizione dell'auto.");
        lv_label_set_text(s_status, lock ? "GPS pronto" : "Attendo il fix GPS...");
        lv_obj_set_style_text_color(s_status,
            lock ? lv_color_make(0x00, 0xCC, 0x66) : lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
        return;
    }

    // A car is saved — show recall UI.
    show(s_map_btn, true); show(s_clr_btn, true);
    lv_label_set_text(s_save_lbl, LV_SYMBOL_GPS "  AGGIORNA");

    char hint[64];
    snprintf(hint, sizeof(hint), "auto: %.5f, %.5f", clat, clon);
    lv_label_set_text(s_hint, hint);
    show(s_hint, true);

    if (!lock) {
        show(s_arrow, false); show(s_dist, true);
        lv_label_set_text(s_dist, "-- m");
        lv_label_set_text(s_status, "Auto salvata · attendo il tuo fix GPS");
        lv_obj_set_style_text_color(s_status, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
        return;
    }

    double ulat = instance.gps.location.lat();
    double ulon = instance.gps.location.lng();
    double dist = geo_distance_m(ulat, ulon, clat, clon);
    double brg  = geo_bearing_deg(ulat, ulon, clat, clon);

    char db[24];
    if (dist < 1000.0) snprintf(db, sizeof(db), "%d m", (int)(dist + 0.5));
    else               snprintf(db, sizeof(db), "%.2f km", dist / 1000.0);
    lv_label_set_text(s_dist, db);
    show(s_dist, true);

    // Orient the arrow. With no compass, we can only know which way you FACE
    // from GPS course-over-ground, which is valid only while moving. Standing
    // still → north-up (top of screen = north) and the arrow shows the absolute
    // bearing, with a note to start walking.
    bool moving = instance.gps.speed.isValid() && instance.gps.speed.kmph() > 3.0
                  && instance.gps.course.isValid();
    double ang = moving ? (brg - instance.gps.course.deg()) : brg;
    set_arrow_angle(ang);
    show(s_arrow, true);

    if (moving) {
        lv_label_set_text(s_status, "Segui la freccia");
    } else {
        lv_label_set_text(s_status, LV_SYMBOL_UP " Nord in alto · cammina per orientare");
    }
    lv_obj_set_style_text_color(s_status, lv_color_make(0x00, 0xCC, 0x66), LV_PART_MAIN);
}

static void on_save(lv_event_t *)
{
    if (!(gps_screen_has_lock() && instance.gps.location.isValid())) {
        lv_label_set_text(s_status, "Nessun fix GPS: non salvo");
        lv_obj_set_style_text_color(s_status, lv_color_make(0xFF, 0x66, 0x00), LV_PART_MAIN);
        return;
    }
    car_finder_save(instance.gps.location.lat(), instance.gps.location.lng());
    refresh();
}

static void on_clear(lv_event_t *)
{
    car_finder_clear();
    refresh();
}

static void on_map(lv_event_t *)
{
    double clat, clon;
    if (car_finder_get(&clat, &clon)) map_screen_focus(clat, clon);
}

static void on_timer(lv_timer_t *) { if (s_active) refresh(); }

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        s_active = false;
        tools_screen_show();
    }
}

static lv_obj_t *make_btn(const char *txt, lv_align_t al, int ox, int oy, int w,
                          lv_color_t border, lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *b = lv_obj_create(s_screen);
    lv_obj_set_size(b, w, 46);
    lv_obj_set_style_radius(b, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, border, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(b, al, ox, oy);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, border, LV_PART_MAIN);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (lbl_out) *lbl_out = l;
    return b;
}

static void car_screen_create()
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xCC, 0x66), LV_PART_MAIN);
    lv_label_set_text(title, LV_SYMBOL_GPS "  AUTO");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    s_status = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_status, "Attendo il fix GPS...");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 56);

    // Direction arrow (neon outline). Hidden until a car + fix are available.
    s_arrow = lv_line_create(s_screen);
    lv_obj_set_style_line_width(s_arrow, 6, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_arrow, lv_color_make(0x00, 0xFF, 0x88), LV_PART_MAIN);
    lv_obj_set_style_line_rounded(s_arrow, true, LV_PART_MAIN);
    set_arrow_angle(0);
    lv_obj_add_flag(s_arrow, LV_OBJ_FLAG_HIDDEN);

    // Big distance readout under the arrow.
    s_dist = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_dist, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_dist, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(s_dist, "-- m");
    lv_obj_align(s_dist, LV_ALIGN_TOP_MID, 0, 296);
    lv_obj_add_flag(s_dist, LV_OBJ_FLAG_HIDDEN);

    // Hint / coords line.
    s_hint = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_hint, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_hint, "");
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 356);

    // Buttons (kept in the central band, off the rounded corners).
    s_map_btn = make_btn(LV_SYMBOL_DIRECTORY "  MAPPA", LV_ALIGN_BOTTOM_MID, -82, -84, 150,
                         lv_color_make(0x00, 0x88, 0xCC), on_map, nullptr);
    s_clr_btn = make_btn(LV_SYMBOL_TRASH "  CANCELLA", LV_ALIGN_BOTTOM_MID, 82, -84, 150,
                         lv_color_make(0xCC, 0x44, 0x44), on_clear, nullptr);
    s_save_btn = make_btn("", LV_ALIGN_BOTTOM_MID, 0, -26, 330,   // 12px higher + bigger
                          lv_color_make(0x00, 0xCC, 0x66), on_save, &s_save_lbl);
    lv_obj_set_height(s_save_btn, 54);
    lv_obj_set_style_text_font(s_save_lbl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_label_set_text(s_save_lbl, LV_SYMBOL_GPS "  SALVA POSIZIONE AUTO");

    s_timer = lv_timer_create(on_timer, 1000, NULL);
}

void car_screen_show()
{
    if (!s_screen) car_screen_create();
    s_active = true;
    gps_screen_power_on();   // start acquiring a fix without hunting for the switch
    refresh();
    lv_scr_load(s_screen);
}

bool car_screen_is_active() { return s_active; }
