#include "calmati_result_screen.h"
#include "calmati_remote_screen.h"
#include "calmati_remote.h"
#include <lvgl.h>
#include <Arduino.h>

static lv_obj_t *s_screen = nullptr;
static lv_obj_t *s_list   = nullptr;
static bool s_active = false;
static char s_title[24] = {0};
static char s_endpoint[24] = {0};
static bool s_alert = false;

// All-white scheme. Skimmer "alert" rows stand out via a "!" prefix and a
// thicker/brighter border, not a different hue.
static lv_color_t C_GREEN() { return lv_color_make(0xFF, 0xFF, 0xFF); }
static lv_color_t C_VIOLET(){ return lv_color_make(0xFF, 0xFF, 0xFF); }
static lv_color_t C_MUTE()  { return lv_color_make(0x86, 0x8e, 0x94); }
static lv_color_t C_INK()   { return lv_color_make(0xEC, 0xF0, 0xF3); }

// ---- extract a "key":"..." string within [from,to) ----
static String jstr(const String &j, const char *key, int from, int to) {
  int p = j.indexOf(key, from);
  if (p < 0 || p >= to) return "";
  int s = p + strlen(key); String out = "";
  while (s < to) { char c = j[s];
    if (c == '\\' && s + 1 < to) { out += j[s + 1]; s += 2; continue; }
    if (c == '"') break; out += c; s++; }
  return out;
}
// ---- extract a "key":<int> within [from,to); returns true if found ----
static bool jint(const String &j, const char *key, int from, int to, int &val) {
  int p = j.indexOf(key, from);
  if (p < 0 || p >= to) return false;
  int s = p + strlen(key); bool neg = false;
  if (s < to && j[s] == '-') { neg = true; s++; }
  int v = 0; bool any = false;
  while (s < to && isDigit(j[s])) { v = v * 10 + (j[s] - '0'); s++; any = true; }
  if (!any) return false; val = neg ? -v : v; return true;
}

static int parse_rows(const String &j, String *labels, String *metas, int maxn) {
  int n = 0, pos = 0;
  while (n < maxn) {
    int ip = j.indexOf("\"i\":", pos);
    if (ip < 0) break;
    int nx = j.indexOf("\"i\":", ip + 4);
    int end = (nx < 0) ? j.length() : nx;
    String lab = jstr(j, "\"e\":\"", ip, end);
    if (lab.length() == 0) lab = jstr(j, "\"m\":\"", ip, end);
    if (lab.length() == 0) lab = "(?)";
    String meta = ""; int v;
    if (jint(j, "\"r\":", ip, end, v))      meta = String(v) + "dB";
    else if (jint(j, "\"p\":", ip, end, v)) meta = String(v) + " pkt";
    labels[n] = lab; metas[n] = meta;
    n++; pos = (nx < 0) ? j.length() : nx;
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

static void populate();
static void on_refresh(lv_event_t *) { populate(); }

static void populate() {
  if (!s_list) return;
  lv_obj_clean(s_list);

  // Refresh control.
  lv_obj_t *rb = lv_obj_create(s_list);
  lv_obj_set_size(rb, lv_pct(100), 46);
  lv_obj_set_style_radius(rb, 14, LV_PART_MAIN);
  lv_obj_set_style_bg_color(rb, lv_color_make(0x0a, 0x15, 0x08), LV_PART_MAIN);
  lv_obj_set_style_border_color(rb, C_GREEN(), LV_PART_MAIN);
  lv_obj_set_style_border_width(rb, 2, LV_PART_MAIN);
  lv_obj_clear_flag(rb, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(rb, on_refresh, LV_EVENT_CLICKED, NULL);
  lv_obj_t *rl = lv_label_create(rb);
  lv_obj_set_style_text_font(rl, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(rl, C_GREEN(), LV_PART_MAIN);
  lv_label_set_text(rl, LV_SYMBOL_REFRESH "  Aggiorna"); lv_obj_center(rl);
  lv_obj_add_flag(rl, LV_OBJ_FLAG_EVENT_BUBBLE);

  String j;
  if (!calmati_get(s_endpoint, j)) {
    lv_obj_t *m = lv_label_create(s_list); lv_obj_set_width(m, lv_pct(100));
    lv_obj_set_style_text_color(m, C_MUTE(), LV_PART_MAIN);
    lv_label_set_text(m, "AP non raggiungibile"); return;
  }
  static String labels[60]; static String metas[60];
  int n = parse_rows(j, labels, metas, 60);
  if (n == 0) {
    lv_obj_t *m = lv_label_create(s_list); lv_obj_set_width(m, lv_pct(100));
    lv_obj_set_style_text_color(m, C_MUTE(), LV_PART_MAIN);
    lv_label_set_text(m, "nessun risultato - fai lo scan e attendi"); return;
  }

  lv_color_t hi = s_alert ? C_VIOLET() : C_GREEN();
  for (int k = 0; k < n; k++) {
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, lv_pct(100), 50);
    lv_obj_set_style_radius(row, 11, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, s_alert ? lv_color_make(0x1a, 0x1c, 0x1e) : lv_color_make(0x0d, 0x0f, 0x11), LV_PART_MAIN);
    lv_obj_set_style_border_color(row, s_alert ? C_VIOLET() : lv_color_make(0x24, 0x2c, 0x34), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, s_alert ? 2 : 1, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nm = lv_label_create(row);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(nm, s_alert ? hi : C_INK(), LV_PART_MAIN);
    String lab = labels[k]; if (lab.length() > 18) lab = lab.substring(0, 18);
    if (s_alert) lab = String("! ") + lab;
    lv_label_set_text(nm, lab.c_str());
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 10, 0);

    if (metas[k].length()) {
      lv_obj_t *mt = lv_label_create(row);
      lv_obj_set_style_text_font(mt, &lv_font_montserrat_16, LV_PART_MAIN);
      lv_obj_set_style_text_color(mt, C_MUTE(), LV_PART_MAIN);
      lv_label_set_text(mt, metas[k].c_str());
      lv_obj_align(mt, LV_ALIGN_RIGHT_MID, -10, 0);
    }
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
  lv_obj_set_style_text_color(title, s_alert ? C_VIOLET() : C_GREEN(), LV_PART_MAIN);
  lv_label_set_text(title, s_title);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

  lv_obj_t *bk = lv_obj_create(s_screen);
  lv_obj_set_size(bk, 240, 44);
  lv_obj_set_style_radius(bk, 16, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bk, lv_color_make(0x0a, 0x0d, 0x12), LV_PART_MAIN);
  lv_obj_set_style_border_color(bk, C_INK(), LV_PART_MAIN);
  lv_obj_set_style_border_width(bk, 2, LV_PART_MAIN);
  lv_obj_clear_flag(bk, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(bk, LV_ALIGN_TOP_MID, 0, 42);
  lv_obj_add_event_cb(bk, [](lv_event_t *) { s_active = false; calmati_remote_screen_show(); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bl = lv_label_create(bk);
  lv_obj_set_style_text_font(bl, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(bl, C_INK(), LV_PART_MAIN);
  lv_label_set_text(bl, LV_SYMBOL_LEFT "  INDIETRO"); lv_obj_center(bl);
  lv_obj_add_flag(bl, LV_OBJ_FLAG_EVENT_BUBBLE);

  s_list = lv_obj_create(s_screen);
  lv_obj_set_size(s_list, 400, 400);
  lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 96);
  lv_obj_set_style_bg_color(s_list, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_list, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_list, 9, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_layout(s_list, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
}

void calmati_result_screen_show(const char *title, const char *endpoint, bool alert) {
  strncpy(s_title, title, sizeof(s_title) - 1); s_title[sizeof(s_title)-1]=0;
  strncpy(s_endpoint, endpoint, sizeof(s_endpoint) - 1); s_endpoint[sizeof(s_endpoint)-1]=0;
  s_alert = alert;
  // Rebuild every time so the title/colour match the chosen result type.
  s_screen = nullptr;
  build();
  s_active = true;
  lv_obj_t *l = lv_label_create(s_list); lv_obj_set_width(l, lv_pct(100));
  lv_obj_set_style_text_color(l, C_MUTE(), LV_PART_MAIN);
  lv_label_set_text(l, "caricamento...");
  lv_scr_load(s_screen);
  lv_timer_t *t = lv_timer_create(on_populate_timer, 350, nullptr);
  lv_timer_set_repeat_count(t, 1);
}

bool calmati_result_screen_is_active() { return s_active; }
