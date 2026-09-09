#include "calmati_targets_screen.h"
#include "calmati_remote_screen.h"
#include "calmati_remote.h"
#include <lvgl.h>
#include <Arduino.h>

static lv_obj_t *s_screen = nullptr;
static lv_obj_t *s_list   = nullptr;   // scrollable list of AP rows
static lv_obj_t *s_hint   = nullptr;
static bool s_active = false;

// All-white scheme.
static lv_color_t C_AMBER() { return lv_color_make(0xFF, 0xFF, 0xFF); }
static lv_color_t C_GREEN() { return lv_color_make(0xFF, 0xFF, 0xFF); }
static lv_color_t C_MUTE()  { return lv_color_make(0x86, 0x8e, 0x94); }
static lv_color_t C_INK()   { return lv_color_make(0xEC, 0xF0, 0xF3); }

// ---- tiny JSON array parser for [{"i":N,"e":"..","c":N,"r":N,"s":N},...] ----
static int parse_aps(const String &j, int *idxs, String *names, bool *sels, int maxn) {
  int n = 0, pos = 0;
  while (n < maxn) {
    int ip = j.indexOf("\"i\":", pos);
    if (ip < 0) break;
    int q = ip + 4, val = 0; bool any = false;
    while (q < (int)j.length() && isDigit(j[q])) { val = val * 10 + (j[q] - '0'); q++; any = true; }
    if (!any) break;
    idxs[n] = val;
    String name = "";
    int ep = j.indexOf("\"e\":\"", ip);
    if (ep >= 0) { int s = ep + 5;
      while (s < (int)j.length()) { char c = j[s];
        if (c == '\\' && s + 1 < (int)j.length()) { name += j[s + 1]; s += 2; continue; }
        if (c == '"') break; name += c; s++; } }
    names[n] = name;
    int sp = j.indexOf("\"s\":", ip);
    sels[n] = (sp >= 0 && sp + 4 < (int)j.length() && j[sp + 4] == '1');
    n++; pos = q;
  }
  return n;
}

static void on_gesture(lv_event_t *e) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_event_get_indev(e));
  if (dir == LV_DIR_TOP) {
    if (!s_list || lv_obj_get_scroll_bottom(s_list) <= 4) { s_active = false; calmati_remote_screen_show(); }
  } else if (dir == LV_DIR_BOTTOM) {
    if (s_list && lv_obj_get_scroll_top(s_list) <= 4) { s_active = false; calmati_remote_screen_show(); }
  }
}

static void populate();  // fwd

static void on_pick(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  String out; char cmd[24]; snprintf(cmd, sizeof(cmd), "select -a %d", i);
  calmati_send(cmd, out);
  populate();   // refresh selection state from the dongle
}
static void on_selall(lv_event_t *) { String o; calmati_send("select -a all", o); populate(); }
static void on_refresh(lv_event_t *) { populate(); }

static lv_obj_t *ctrl_btn(lv_obj_t *parent, const char *label, lv_color_t col, lv_event_cb_t cb) {
  lv_obj_t *b = lv_obj_create(parent);
  lv_obj_set_size(b, 180, 52);
  lv_obj_set_style_radius(b, 14, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, lv_color_darken(col, 210), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(b, col, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, col, LV_PART_MAIN);
  lv_label_set_text(l, label); lv_obj_center(l);
  lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
  return b;
}

static void populate() {
  if (!s_list) return;
  lv_obj_clean(s_list);

  // Controls row: Refresh + Select all.
  ctrl_btn(s_list, LV_SYMBOL_REFRESH "  Aggiorna", C_GREEN(), on_refresh);
  ctrl_btn(s_list, "Sel. tutti", C_AMBER(), on_selall);

  String j;
  bool ok = calmati_aps(j);
  if (!ok) {
    s_hint = lv_label_create(s_list); lv_obj_set_width(s_hint, lv_pct(100));
    lv_obj_set_style_text_color(s_hint, C_MUTE(), LV_PART_MAIN);
    lv_label_set_text(s_hint, "AP non raggiungibile"); return;
  }

  static int idxs[40]; static String names[40]; static bool sels[40];
  int n = parse_aps(j, idxs, names, sels, 40);
  if (n == 0) {
    s_hint = lv_label_create(s_list); lv_obj_set_width(s_hint, lv_pct(100));
    lv_obj_set_style_text_color(s_hint, C_MUTE(), LV_PART_MAIN);
    lv_label_set_text(s_hint, "nessun AP - fai SCAN, attendi, poi Aggiorna"); return;
  }

  for (int k = 0; k < n; k++) {
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, lv_pct(100), 54);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_make(0x0d, 0x13, 0x1a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, sels[k] ? C_GREEN() : lv_color_make(0x26, 0x41, 0x4a), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, sels[k] ? 2 : 1, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_pick, LV_EVENT_CLICKED, (void *)(intptr_t)idxs[k]);

    lv_obj_t *nm = lv_label_create(row);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(nm, sels[k] ? C_GREEN() : C_INK(), LV_PART_MAIN);
    String label = String(sels[k] ? LV_SYMBOL_OK "  " : "     ") + (names[k].length() ? names[k] : String("(hidden)"));
    if (label.length() > 22) label = label.substring(0, 22);
    lv_label_set_text(nm, label.c_str());
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_add_flag(nm, LV_OBJ_FLAG_EVENT_BUBBLE);
  }
}

static void on_populate_timer(lv_timer_t *t) { populate(); lv_timer_del(t); }

static void build() {
  s_screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

  lv_obj_t *title = lv_label_create(s_screen);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(title, C_AMBER(), LV_PART_MAIN);
  lv_label_set_text(title, "TARGETS  /  AP");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

  // Fixed Back button (always visible/tappable — swipe-back is unreliable while
  // the list is scrolled).
  {
    lv_obj_t *bk = lv_obj_create(s_screen);
    lv_obj_set_size(bk, 240, 46);
    lv_obj_set_style_radius(bk, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bk, lv_color_make(0x0d, 0x13, 0x1a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bk, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(bk, C_INK(), LV_PART_MAIN);
    lv_obj_set_style_border_width(bk, 2, LV_PART_MAIN);
    lv_obj_clear_flag(bk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bk, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_add_event_cb(bk, [](lv_event_t *) { s_active = false; calmati_remote_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bk);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(bl, C_INK(), LV_PART_MAIN);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  INDIETRO");
    lv_obj_center(bl);
    lv_obj_add_flag(bl, LV_OBJ_FLAG_EVENT_BUBBLE);
  }

  s_list = lv_obj_create(s_screen);
  lv_obj_set_size(s_list, 400, 400);
  lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 96);
  lv_obj_set_style_bg_color(s_list, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_list, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_list, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_column(s_list, 10, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_layout(s_list, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
}

void calmati_targets_screen_show() {
  if (!s_screen) build();
  s_active = true;
  lv_obj_clean(s_list);
  lv_obj_t *l = lv_label_create(s_list); lv_obj_set_width(l, lv_pct(100));
  lv_obj_set_style_text_color(l, C_MUTE(), LV_PART_MAIN);
  lv_label_set_text(l, "caricamento AP...");
  lv_scr_load(s_screen);
  lv_timer_t *t = lv_timer_create(on_populate_timer, 350, nullptr);
  lv_timer_set_repeat_count(t, 1);
}

bool calmati_targets_screen_is_active() { return s_active; }
