#include "calmati_remote_screen.h"
#include "calmati_targets_screen.h"
#include "calmati_result_screen.h"
#include "calmati_remote.h"
#include "tools_screen.h"
#include <lvgl.h>
#include <Arduino.h>

// Calmati brand logo, reused as a small header mark (src/calmati_icon.c).
extern "C" const lv_image_dsc_t calmati_icon;

static lv_obj_t *s_screen = nullptr;
static lv_obj_t *s_grid   = nullptr;   // scrollable control grid
static lv_obj_t *s_status = nullptr;
static lv_obj_t *s_dot    = nullptr;   // connection indicator
static lv_obj_t *s_chlbl  = nullptr;   // channel value label
static bool s_active = false;
static int  s_channel = 1;

// ---- palette -------------------------------------------------------------
// Strict two-colour scheme: GREEN + VIOLET on black. Everything that used to be
// red/cyan/amber is now violet, so the panel reads green (go/positive) + violet
// (everything else). Neutrals: ink / mute.
// All-white scheme: single accent (white) on black, neutral greys.
static lv_color_t C_VIOLET() { return lv_color_make(0xFF, 0xFF, 0xFF); }
static lv_color_t C_GREEN()  { return lv_color_make(0xFF, 0xFF, 0xFF); }
static lv_color_t C_RED()    { return lv_color_make(0xFF, 0xFF, 0xFF); }
static lv_color_t C_CYAN()   { return lv_color_make(0xFF, 0xFF, 0xFF); }
static lv_color_t C_AMBER()  { return lv_color_make(0xFF, 0xFF, 0xFF); }
static lv_color_t C_INK()    { return lv_color_make(0xEC, 0xF0, 0xF3); }
static lv_color_t C_MUTE()   { return lv_color_make(0x86, 0x8e, 0x94); }
// A dark tint of an accent, for filled backgrounds.
static lv_color_t tint(lv_color_t c) { return lv_color_darken(c, 210); }

static void set_status(const String &t, lv_color_t c) {
  if (s_status) { lv_label_set_text(s_status, t.c_str()); lv_obj_set_style_text_color(s_status, c, LV_PART_MAIN); }
}
static void set_dot(lv_color_t c) {
  if (s_dot) lv_obj_set_style_bg_color(s_dot, c, LV_PART_MAIN);
}

// ---- connection (deferred until after the screen is shown) ----------------
static void on_connect_timer(lv_timer_t *t) {
  set_status("connessione...", C_AMBER());
  bool ok = calmati_connect();
  if (ok) { set_status("connesso a CALMATI", C_GREEN()); set_dot(C_GREEN()); }
  else    { set_status("AP non trovato", C_RED());       set_dot(C_RED());   }
  lv_timer_del(t);
}

static void on_cmd(lv_event_t *e) {
  const char *cmd = (const char *)lv_event_get_user_data(e);
  set_status(String(cmd) + "...", C_CYAN());
  String out; bool ok = calmati_send(cmd, out);
  out.trim(); if (out.length() > 34) out = out.substring(0, 34);
  set_status(ok ? (String(cmd) + " OK") : (String("err: ") + out), ok ? C_GREEN() : C_RED());
  set_dot(calmati_is_connected() ? C_GREEN() : C_RED());
}

static void send_channel() {
  char b[20]; snprintf(b, sizeof(b), "channel -s %d", s_channel);
  String out; calmati_send(b, out);
  if (s_chlbl) lv_label_set_text_fmt(s_chlbl, "%d", s_channel);
  set_status(String("canale ") + s_channel, C_AMBER());
}
static void on_ch_up(lv_event_t *)   { if (s_channel < 14) s_channel++; send_channel(); }
static void on_ch_down(lv_event_t *) { if (s_channel > 1)  s_channel--; send_channel(); }

// Result views (read the dongle's live lists).
static void on_res_skim(lv_event_t *) { s_active = false; calmati_result_screen_show("SKIMMER",  "/skimmers", true);  }
static void on_res_ble(lv_event_t *)  { s_active = false; calmati_result_screen_show("BLE",      "/ble",      false); }
static void on_res_sta(lv_event_t *)  { s_active = false; calmati_result_screen_show("STAZIONI", "/stations", false); }

static void on_gesture(lv_event_t *e) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_event_get_indev(e));
  if (dir == LV_DIR_TOP) {
    if (!s_grid || lv_obj_get_scroll_bottom(s_grid) <= 4) { s_active = false; tools_screen_show(); }
  } else if (dir == LV_DIR_BOTTOM) {
    if (s_grid && lv_obj_get_scroll_top(s_grid) <= 4) { s_active = false; tools_screen_show(); }
  }
}

// A styled control button. `hero` makes it a large glowing primary control.
static lv_obj_t *make_btn(lv_obj_t *parent, const char *label, const char *cmd,
                          lv_color_t col, bool hero, lv_event_cb_t cb = on_cmd) {
  lv_obj_t *b = lv_obj_create(parent);
  lv_obj_set_size(b, hero ? 182 : 118, hero ? 84 : 62);
  lv_obj_set_style_radius(b, hero ? 20 : 16, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, tint(col), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(b, col, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, hero ? 3 : 2, LV_PART_MAIN);
  lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
  if (hero) {   // soft glow
    lv_obj_set_style_shadow_color(b, col, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(b, LV_OPA_40, LV_PART_MAIN);
  }
  lv_obj_set_style_bg_color(b, lv_color_darken(col, 150), LV_STATE_PRESSED);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)cmd);

  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, hero ? &lv_font_montserrat_28 : &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, col, LV_PART_MAIN);
  lv_label_set_text(l, label);
  lv_obj_center(l);
  lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
  return b;
}

// A full-width row that holds children laid out horizontally (breaks the flex).
static lv_obj_t *make_row(lv_obj_t *parent, int h) {
  lv_obj_t *r = lv_obj_create(parent);
  lv_obj_set_size(r, lv_pct(100), h);
  lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(r, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  return r;
}

static void build() {
  s_screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

  // Header: logo mark + wordmark.
  lv_obj_t *mark = lv_image_create(s_screen);
  lv_image_set_src(mark, &calmati_icon);
  lv_image_set_scale(mark, 100);   // ~40% of 120px -> ~48px
  lv_obj_set_style_image_recolor(mark, C_VIOLET(), LV_PART_MAIN);
  lv_obj_set_style_image_recolor_opa(mark, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(mark, LV_ALIGN_TOP_MID, -70, 6);
  lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t *title = lv_label_create(s_screen);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, C_VIOLET(), LV_PART_MAIN);
  lv_label_set_text(title, "CALMATI");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 6, 16);

  // Scrollable control grid (proven-tappable config from the Tools menu).
  s_grid = lv_obj_create(s_screen);
  lv_obj_set_size(s_grid, 400, 402);
  lv_obj_align(s_grid, LV_ALIGN_TOP_MID, 0, 62);
  lv_obj_set_style_bg_color(s_grid, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_width(s_grid, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_grid, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_grid, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_column(s_grid, 12, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_layout(s_grid, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  // Top spacer: keeps the first real control clear of the rounded top edge / the
  // non-tappable band, so SCAN/STOP are always reachable.
  { lv_obj_t *sp = lv_obj_create(s_grid); lv_obj_set_size(sp, lv_pct(100), 40);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(sp, 0, 0);
    lv_obj_clear_flag(sp, LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE); }

  // --- status chip (full width) ---
  lv_obj_t *chip = lv_obj_create(s_grid);
  lv_obj_set_size(chip, lv_pct(100), 40);
  lv_obj_set_style_radius(chip, 20, LV_PART_MAIN);
  lv_obj_set_style_bg_color(chip, lv_color_make(0x0d, 0x13, 0x1a), LV_PART_MAIN);
  lv_obj_set_style_border_color(chip, lv_color_make(0x26, 0x41, 0x4a), LV_PART_MAIN);
  lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
  lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
  s_dot = lv_obj_create(chip);
  lv_obj_set_size(s_dot, 12, 12);
  lv_obj_set_style_radius(s_dot, 6, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_dot, C_AMBER(), LV_PART_MAIN);
  lv_obj_set_style_border_width(s_dot, 0, LV_PART_MAIN);
  lv_obj_align(s_dot, LV_ALIGN_LEFT_MID, 12, 0);
  lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
  s_status = lv_label_create(chip);
  lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_status, C_MUTE(), LV_PART_MAIN);
  lv_label_set_text(s_status, "pronto");
  lv_obj_align(s_status, LV_ALIGN_LEFT_MID, 34, 0);

  // --- hero controls: SCAN / STOP ---
  make_btn(s_grid, "SCAN", "scanall",  C_GREEN(), true);
  make_btn(s_grid, "STOP", "stopscan", C_RED(),   true);

  // --- channel stepper (full-width row) ---
  lv_obj_t *chrow = make_row(s_grid, 60);
  make_btn(chrow, "-", "", C_AMBER(), false, on_ch_down);
  lv_obj_t *cwrap = lv_obj_create(chrow);
  lv_obj_set_size(cwrap, 120, 60);
  lv_obj_set_style_bg_opa(cwrap, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(cwrap, 0, LV_PART_MAIN);
  lv_obj_clear_flag(cwrap, LV_OBJ_FLAG_SCROLLABLE);
  s_chlbl = lv_label_create(cwrap);
  lv_obj_set_style_text_font(s_chlbl, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_chlbl, C_AMBER(), LV_PART_MAIN);
  lv_label_set_text_fmt(s_chlbl, "%d", s_channel);
  lv_obj_center(s_chlbl);
  make_btn(chrow, "+", "", C_AMBER(), false, on_ch_up);

  // --- Select AP (full width) -> opens the targets picker ---
  {
    lv_obj_t *tgt = lv_obj_create(s_grid);
    lv_obj_set_size(tgt, lv_pct(100), 60);
    lv_obj_set_style_radius(tgt, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tgt, tint(C_AMBER()), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tgt, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(tgt, C_AMBER(), LV_PART_MAIN);
    lv_obj_set_style_border_width(tgt, 2, LV_PART_MAIN);
    lv_obj_clear_flag(tgt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tgt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tgt, [](lv_event_t *) { s_active = false; calmati_targets_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tl = lv_label_create(tgt);
    lv_obj_set_style_text_font(tl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(tl, C_AMBER(), LV_PART_MAIN);
    lv_label_set_text(tl, LV_SYMBOL_GPS "  SELECT AP");
    lv_obj_center(tl);
    lv_obj_add_flag(tl, LV_OBJ_FLAG_EVENT_BUBBLE);
  }

  // --- fire commands (two per row) ---
  make_btn(s_grid, "Beacon",   "sniffbeacon",      C_GREEN(),  false);
  make_btn(s_grid, "Deauth",   "attack -t deauth", C_VIOLET(), false);
  make_btn(s_grid, "Sniff BT", "sniffbt",          C_VIOLET(), false);
  make_btn(s_grid, "Sniff Sk", "sniffskim",        C_GREEN(),  false);
  make_btn(s_grid, "Wardrive", "wardrive",         C_GREEN(),  false);
  make_btn(s_grid, "Reboot",   "reboot",           C_VIOLET(), false);

  // --- result views (read the dongle's live lists) ---
  make_btn(s_grid, "R:Skim", "", C_VIOLET(), false, on_res_skim);
  make_btn(s_grid, "R:BLE",  "", C_VIOLET(), false, on_res_ble);
  make_btn(s_grid, "R:Staz", "", C_GREEN(),  false, on_res_sta);

  // Bottom spacer so the last row can scroll up into the tappable centre band.
  { lv_obj_t *sp = lv_obj_create(s_grid); lv_obj_set_size(sp, lv_pct(100), 60);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0); lv_obj_set_style_border_width(sp, 0, 0);
    lv_obj_clear_flag(sp, LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE); }
}

void calmati_remote_screen_show() {
  if (!s_screen) build();
  s_active = true;
  set_status("pronto", C_MUTE());
  set_dot(C_AMBER());
  lv_scr_load(s_screen);
  lv_timer_t *t = lv_timer_create(on_connect_timer, 400, nullptr);
  lv_timer_set_repeat_count(t, 1);
}

bool calmati_remote_screen_is_active() { return s_active; }
