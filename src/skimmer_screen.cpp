#include "skimmer_screen.h"
#include <LilyGoLib.h>
#include "skimmer.h"
#include "ble_scan_manager.h"
#include "gps_screen.h"
#include "skimmer_probe.h"
#include <string.h>
#include <stdio.h>
#include "esp_gap_ble_api.h"

// Defined in tools_screen.cpp
void tools_screen_show();

// ── catalogo dei moduli seriali BT/BLE noti (le firme di skimmer.cpp) ──────────
// L'ultima voce e' il bucket "rinominato" (match solo su servizio FFE0/FFF0).
static const char *CAT[] = {
    "HC-03","HC-04","HC-05","HC-06","HC-07","HC-08","HC-42","HM-1","HMSoft","SH-HC",
    "CC41","BLE-CC41","AT-05","AT-09","AT-19","BT05","BT-05","MLT-BT05","BT-HC","FBT06",
    "JDY-","SPP-C","Linvor","ZS-040","TB-04","RF-BM","DX-BT","KCX","FSC-BT","AC690",
    "DSD TECH","Bolutek"
};
#define NCAT (int)(sizeof(CAT) / sizeof(CAT[0]))
#define NROW (NCAT + 1)                 // +1 = riga "RINOMINATO (FFE0/FFF0)"
#define RENAMED_ROW NCAT
// Solo 10 righe VERE riusate come finestra scorrevole: creare 33 righe
// persistenti (~100 oggetti LVGL) esauriva l'heap e bloccava il watch.
#define NVIS 10

// palette fluo
#define C_CYAN    lv_color_make(0x00, 0xe5, 0xff)
#define C_GREEN   lv_color_make(0x39, 0xff, 0x14)
#define C_GREEND  lv_color_make(0x1f, 0x9e, 0x0c)
#define C_RED     lv_color_make(0xff, 0x1e, 0x4d)
#define C_AMBER   lv_color_make(0xff, 0xbb, 0x00)
#define C_DIMNAME lv_color_make(0x38, 0x59, 0x7a)
#define C_ROWBG   lv_color_make(0x08, 0x0f, 0x1a)
#define C_ROWBG_HIT lv_color_make(0x1e, 0x07, 0x10)
#define C_BORDER  lv_color_make(0x16, 0x28, 0x3c)

// ── detected devices (written by the BT-task scan consumer, read on main) ──────
struct SkDev {
    uint8_t  mac[6];
    uint8_t  type;
    int8_t   rssi;
    char     name[24];
    bool     svc_only;
    bool     alerted;      // popup gia' mostrato per questo device (match per nome)
    uint32_t last_ms;
};
#define SK_MAX 12
static SkDev  s_dev[SK_MAX];
static volatile int s_dev_n = 0;

static bool   s_pending_scan = false;
static bool   s_lock_gps = false;
static double s_lock_lat = 0, s_lock_lon = 0;

static lv_obj_t *s_scr;
static lv_obj_t *check_label;
static lv_obj_t *verdict_label;
static lv_obj_t *list_box;

// pool di righe visibili (riusate) — mostra solo i device VISTI, non un
// catalogo statico: niente sweep, niente ridisegno se non cambia nulla.
static lv_obj_t *vrow[NVIS], *vname[NVIS], *vstat[NVIS];
static int       vslot[NVIS];            // indice slot, per la callback di tap
static int       vidx[NVIS];             // indice in s_dev[] mostrato dallo slot, -1 = vuoto
static char      s_last_sig[SK_MAX * 12 + 8] = "";   // firma anti-flicker

// popup di allarme
static lv_obj_t *s_popup = nullptr, *pop_model = nullptr, *pop_rssi = nullptr;

// Detail/probe view
enum { SK_VIEW_LIST, SK_VIEW_DETAIL };
static int s_view = SK_VIEW_LIST;
static uint8_t s_tgt_mac[6];
static esp_ble_addr_type_t s_tgt_type = BLE_ADDR_TYPE_RANDOM;
static lv_obj_t *detail_box, *detail_label;
static lv_obj_t *detail_prox, *detail_bar_bg, *detail_bar_fill;
static bool s_probe_kicked = false;
static bool s_detail_scan_resumed = false;

// ── scan consumer (BT task) ───────────────────────────────────────────────────
static void sk_scan_cb(esp_ble_gap_cb_param_t *param)
{
    auto &r = param->scan_rst;
    int total = (int)r.adv_data_len + (int)r.scan_rsp_len;
    if (total > 62) total = 62;
    char name[24];
    if (!skimmer_ad_match(r.ble_adv, total, name, sizeof(name))) return;

    uint32_t now = millis();
    for (int i = 0; i < s_dev_n; i++)
        if (memcmp(s_dev[i].mac, r.bda, 6) == 0) {
            s_dev[i].rssi = (int8_t)r.rssi;
            s_dev[i].last_ms = now;
            if (name[0] && s_dev[i].name[0] == '\0') {
                strncpy(s_dev[i].name, name, sizeof(s_dev[i].name) - 1);
                s_dev[i].svc_only = false;
            }
            return;
        }
    if (s_dev_n >= SK_MAX) return;
    SkDev &d = s_dev[s_dev_n];
    memcpy(d.mac, r.bda, 6);
    d.type = (uint8_t)r.ble_addr_type;
    d.rssi = (int8_t)r.rssi;
    d.last_ms = now;
    d.alerted = false;
    d.svc_only = (name[0] == '\0');
    strncpy(d.name, name[0] ? name : "(serial-BLE)", sizeof(d.name) - 1);
    d.name[sizeof(d.name) - 1] = '\0';
    Serial.printf("[SKIM] %02X:%02X:%02X:%02X:%02X:%02X rssi=%d match=%s name=\"%s\"\n",
                  r.bda[0],r.bda[1],r.bda[2],r.bda[3],r.bda[4],r.bda[5],
                  (int)r.rssi, d.svc_only ? "SVC(FFE0/FFF0)" : "NAME", d.name);
    s_dev_n++;
}

// ── helpers ───────────────────────────────────────────────────────────────────
static const char *prox_txt(int rssi) {
    if (rssi > -55) return "VICINO";
    if (rssi > -75) return "MEDIO";
    return "LONTANO";
}
static lv_color_t prox_col(int rssi) {
    if (rssi > -55) return lv_color_make(0xff, 0x44, 0x44);
    if (rssi > -75) return C_AMBER;
    return lv_color_make(0x55, 0x99, 0x55);
}
static int rssi_pct(int rssi) {
    int p = (rssi + 90) * 2;
    return p < 6 ? 6 : (p > 100 ? 100 : p);
}
static const char *cat_name_of(int idx) {
    return idx == RENAMED_ROW ? "RINOMINATO (FFE0)" : CAT[idx];
}

// ── DETAIL ────────────────────────────────────────────────────────────────────
static void enter_detail(const uint8_t *mac, esp_ble_addr_type_t type)
{
    memcpy(s_tgt_mac, mac, 6);
    s_lock_gps = gps_screen_has_lock() && instance.gps.location.isValid();
    if (s_lock_gps) { s_lock_lat = instance.gps.location.lat(); s_lock_lon = instance.gps.location.lng(); }
    s_tgt_type = type;
    instance.vibrator();
    s_view = SK_VIEW_DETAIL;
    s_probe_kicked = false;
    s_detail_scan_resumed = false;
    ble_scan_remove(sk_scan_cb);
}
static void on_row_clicked(lv_event_t *e)
{
    int j = *(int *)lv_event_get_user_data(e);
    if (j < 0 || j >= NVIS) return;
    int idx = vidx[j];
    if (idx < 0 || idx >= s_dev_n) return;
    enter_detail(s_dev[idx].mac, (esp_ble_addr_type_t)s_dev[idx].type);
}

// ── popup ─────────────────────────────────────────────────────────────────────
static void on_popup_close(lv_event_t *)
{
    if (s_popup) lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
}
static void show_popup(const char *model, int rssi)
{
    if (!s_popup) return;
    lv_label_set_text(pop_model, model);
    lv_label_set_text_fmt(pop_rssi, "%d dBm  -  %s", rssi, prox_txt(rssi));
    lv_obj_clear_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_popup);
}
static bool popup_visible() { return s_popup && !lv_obj_has_flag(s_popup, LV_OBJ_FLAG_HIDDEN); }

// ── pool di righe ─────────────────────────────────────────────────────────────
static void make_slot(int j)
{
    vslot[j] = j;
    vidx[j]  = -1;
    lv_obj_t *row = lv_obj_create(list_box);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, C_ROWBG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, C_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 8, LV_PART_MAIN);
    lv_obj_set_style_outline_color(row, C_CYAN, LV_PART_MAIN);
    lv_obj_set_style_outline_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_outline_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(row, C_RED, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, &vslot[j]);

    lv_obj_t *nm = lv_label_create(row);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(nm, C_DIMNAME, LV_PART_MAIN);
    lv_label_set_text(nm, "");

    lv_obj_t *st = lv_label_create(row);
    lv_obj_set_style_text_font(st, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(st, lv_color_make(0x20, 0x36, 0x4d), LV_PART_MAIN);
    lv_label_set_text(st, ".");

    vrow[j] = row; vname[j] = nm; vstat[j] = st;
}

static void reset_state()
{
    s_last_sig[0] = '\0';
    if (!vrow[0]) return;
    for (int j = 0; j < NVIS; j++) { lv_obj_add_flag(vrow[j], LV_OBJ_FLAG_HIDDEN); vidx[j] = -1; }
    lv_label_set_text(check_label, "in ascolto...");
    lv_label_set_text(verdict_label, LV_SYMBOL_OK " AREA PULITA");
    lv_obj_set_style_text_color(verdict_label, C_GREEN, LV_PART_MAIN);
    lv_obj_set_style_bg_color(verdict_label, lv_color_make(0x08, 0x1a, 0x12), LV_PART_MAIN);
}

// ── lista live: mostra solo cio' che e' stato davvero rilevato, nessuno sweep
// di catalogo. Ridisegna solo se la "firma" (device visti + fascia RSSI)
// e' cambiata, cosi' LVGL non lavora inutilmente 4 volte al secondo quando
// l'area e' pulita — questo e' quello che affamava l'heap e bloccava lo schermo.
static void compute_and_render()
{
    uint32_t now = millis();
    int order[SK_MAX], n = 0;
    for (int k = 0; k < s_dev_n; k++)
        if (now - s_dev[k].last_ms < 12000) order[n++] = k;
    // insertion sort per RSSI decrescente (il piu' vicino in cima)
    for (int a = 1; a < n; a++) {
        int idxv = order[a]; int8_t r = s_dev[idxv].rssi; int b = a - 1;
        while (b >= 0 && s_dev[order[b]].rssi < r) { order[b + 1] = order[b]; b--; }
        order[b + 1] = idxv;
    }

    // popup una tantum per ogni device con match per NOME (alta confidenza)
    for (int a = 0; a < n; a++) {
        SkDev &d = s_dev[order[a]];
        if (!d.svc_only && !d.alerted) {
            d.alerted = true;
            show_popup(d.name, d.rssi);
            instance.vibrator();
        }
    }

    char sig[SK_MAX * 12 + 8]; int p = 0;
    for (int a = 0; a < n && p < (int)sizeof(sig) - 12; a++) {
        SkDev &d = s_dev[order[a]];
        p += snprintf(sig + p, sizeof(sig) - p, "%02X%02X%02X:%d;",
                      d.mac[3], d.mac[4], d.mac[5], d.rssi / 4);
    }
    if (strcmp(sig, s_last_sig) == 0) return;     // niente di nuovo: zero lavoro LVGL
    strncpy(s_last_sig, sig, sizeof(s_last_sig) - 1);
    s_last_sig[sizeof(s_last_sig) - 1] = '\0';

    for (int j = 0; j < NVIS; j++) {
        if (j >= n) { lv_obj_add_flag(vrow[j], LV_OBJ_FLAG_HIDDEN); vidx[j] = -1; continue; }
        SkDev &d = s_dev[order[j]];
        lv_obj_clear_flag(vrow[j], LV_OBJ_FLAG_HIDDEN);
        vidx[j] = order[j];
        if (d.svc_only) {
            lv_label_set_text(vname[j], cat_name_of(RENAMED_ROW));
            lv_label_set_text_fmt(vstat[j], "FFE0? %d dBm", d.rssi);
            lv_obj_set_style_text_color(vstat[j], C_AMBER, LV_PART_MAIN);
            lv_obj_set_style_text_color(vname[j], C_AMBER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(vrow[j], lv_color_make(0x1a, 0x14, 0x00), LV_PART_MAIN);
            lv_obj_set_style_border_color(vrow[j], C_AMBER, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(vrow[j], 0, LV_PART_MAIN);
        } else {
            lv_label_set_text(vname[j], d.name);
            lv_label_set_text_fmt(vstat[j], LV_SYMBOL_WARNING " %d dBm", d.rssi);
            lv_obj_set_style_text_color(vstat[j], C_RED, LV_PART_MAIN);
            lv_obj_set_style_text_color(vname[j], lv_color_make(0xff, 0x5c, 0x7a), LV_PART_MAIN);
            lv_obj_set_style_bg_color(vrow[j], C_ROWBG_HIT, LV_PART_MAIN);
            lv_obj_set_style_border_color(vrow[j], C_RED, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(vrow[j], 14, LV_PART_MAIN);
        }
    }

    int nname = 0, nsvc = 0;
    for (int a = 0; a < n; a++) (s_dev[order[a]].svc_only ? nsvc : nname)++;

    if (n == 0) lv_label_set_text(check_label, "in ascolto...");
    else lv_label_set_text_fmt(check_label, "%d dispositiv%s in zona", n, n == 1 ? "o" : "i");

    if (nname > 0) {
        char b[36]; snprintf(b, sizeof(b), LV_SYMBOL_WARNING " %d SKIMMER TROVATO", nname);
        lv_label_set_text(verdict_label, b);
        lv_obj_set_style_text_color(verdict_label, C_RED, LV_PART_MAIN);
        lv_obj_set_style_bg_color(verdict_label, C_ROWBG_HIT, LV_PART_MAIN);
    } else if (nsvc > 0) {
        lv_label_set_text(verdict_label, "! segnale FFE0 - verifica");
        lv_obj_set_style_text_color(verdict_label, C_AMBER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(verdict_label, lv_color_make(0x1a, 0x14, 0x00), LV_PART_MAIN);
    } else {
        lv_label_set_text(verdict_label, LV_SYMBOL_OK " AREA PULITA");
        lv_obj_set_style_text_color(verdict_label, C_GREEN, LV_PART_MAIN);
        lv_obj_set_style_bg_color(verdict_label, lv_color_make(0x08, 0x1a, 0x12), LV_PART_MAIN);
    }
}

static void render_detail()
{
    lv_obj_add_flag(list_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(detail_box, LV_OBJ_FLAG_HIDDEN);

    if (!s_probe_kicked) { s_probe_kicked = true; skimmer_probe_start(s_tgt_mac, s_tgt_type); }

    char buf[320], gps[48];
    if (s_lock_gps) snprintf(gps, sizeof(gps), "GPS %.5f, %.5f", s_lock_lat, s_lock_lon);
    else            snprintf(gps, sizeof(gps), "GPS: nessun fix");

    if (!skimmer_probe_done()) {
        snprintf(buf, sizeof(buf),
                 "%02X:%02X:%02X:%02X:%02X:%02X\n%s\n\nsondaggio GATT in corso...",
                 s_tgt_mac[0],s_tgt_mac[1],s_tgt_mac[2],s_tgt_mac[3],s_tgt_mac[4],s_tgt_mac[5], gps);
        lv_obj_add_flag(detail_prox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(detail_bar_bg, LV_OBJ_FLAG_HIDDEN);
    } else {
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X\n%s\n\n%s",
                 s_tgt_mac[0],s_tgt_mac[1],s_tgt_mac[2],s_tgt_mac[3],s_tgt_mac[4],s_tgt_mac[5],
                 gps, skimmer_probe_result());
        if (!s_detail_scan_resumed) { s_detail_scan_resumed = true; ble_scan_add(sk_scan_cb); }
        int rssi = -100; bool seen = false;
        for (int i = 0; i < s_dev_n; i++)
            if (memcmp(s_dev[i].mac, s_tgt_mac, 6) == 0) { rssi = s_dev[i].rssi; seen = true; break; }
        lv_obj_clear_flag(detail_prox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(detail_bar_bg, LV_OBJ_FLAG_HIDDEN);
        lv_color_t pc = prox_col(rssi);
        lv_label_set_text_fmt(detail_prox, "%s  %d dBm", seen ? prox_txt(rssi) : "...", rssi);
        lv_obj_set_style_text_color(detail_prox, pc, LV_PART_MAIN);
        lv_obj_set_width(detail_bar_fill, lv_pct(rssi_pct(rssi)));
        lv_obj_set_style_bg_color(detail_bar_fill, pc, LV_PART_MAIN);
    }
    lv_label_set_text(detail_label, buf);
}

static void on_refresh(lv_timer_t *)
{
    if (lv_scr_act() != s_scr) return;
    if (s_pending_scan) {
        s_pending_scan = false;
        if (ble_stack_acquire()) ble_scan_add(sk_scan_cb);
    }
    if (s_view == SK_VIEW_DETAIL) { render_detail(); return; }
    lv_obj_clear_flag(list_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(detail_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(detail_prox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(detail_bar_bg, LV_OBJ_FLAG_HIDDEN);
    if (!popup_visible()) compute_and_render();
}

void skimmer_screen_stop()
{
    ble_scan_remove(sk_scan_cb);
    ble_stack_release();
}

static void on_rescan(lv_event_t *)
{
    s_dev_n = 0;
    reset_state();
    if (s_popup) lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
    instance.vibrator();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) != LV_DIR_TOP) return;
    if (popup_visible()) { lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN); return; }
    if (s_view == SK_VIEW_DETAIL) {
        s_view = SK_VIEW_LIST;
        ble_scan_add(sk_scan_cb);
        return;
    }
    skimmer_screen_stop();
    tools_screen_show();
}

void skimmer_screen_create()
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_make(0x05, 0x07, 0x0d), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_scr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_color(title, C_CYAN, LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(title, "SKIMMER DETECT");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    check_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(check_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(check_label, C_CYAN, LV_PART_MAIN);
    lv_label_set_text(check_label, "in ascolto...");
    lv_obj_align(check_label, LV_ALIGN_TOP_MID, 0, 40);

    list_box = lv_obj_create(s_scr);
    lv_obj_set_size(list_box, 372, 340);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_color(list_box, lv_color_make(0x05, 0x07, 0x0d), LV_PART_MAIN);
    lv_obj_set_style_border_color(list_box, lv_color_make(0x12, 0x23, 0x3a), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list_box, 4, LV_PART_MAIN);
    lv_obj_clear_flag(list_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(list_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
    for (int j = 0; j < NVIS; j++) make_slot(j);

    verdict_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(verdict_label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(verdict_label, C_GREEN, LV_PART_MAIN);
    lv_obj_set_style_bg_color(verdict_label, lv_color_make(0x08, 0x1a, 0x12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(verdict_label, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(verdict_label, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(verdict_label, 12, LV_PART_MAIN);
    lv_obj_set_style_text_align(verdict_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(verdict_label, 262);
    lv_obj_align(verdict_label, LV_ALIGN_BOTTOM_LEFT, 26, -16);
    lv_label_set_text(verdict_label, LV_SYMBOL_OK " AREA PULITA");

    lv_obj_t *btn = lv_button_create(s_scr);
    lv_obj_set_size(btn, 66, 52);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, -26, -16);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x08, 0x14, 0x0a), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, C_GREEN, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(btn, C_GREEN, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 16, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, on_rescan, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_obj_set_style_text_color(bl, C_GREEN, LV_PART_MAIN);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(bl, LV_SYMBOL_REFRESH);
    lv_obj_center(bl);

    detail_box = lv_obj_create(s_scr);
    lv_obj_set_size(detail_box, 372, 240);
    lv_obj_align(detail_box, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_bg_color(detail_box, lv_color_make(0x08, 0x12, 0x08), LV_PART_MAIN);
    lv_obj_set_style_border_color(detail_box, C_CYAN, LV_PART_MAIN);
    lv_obj_set_style_border_width(detail_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(detail_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(detail_box, 12, LV_PART_MAIN);
    lv_obj_set_scroll_dir(detail_box, LV_DIR_VER);
    lv_obj_add_flag(detail_box, LV_OBJ_FLAG_HIDDEN);
    detail_label = lv_label_create(detail_box);
    lv_obj_set_width(detail_label, 344);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(detail_label, lv_color_make(0xdd, 0xdd, 0xdd), LV_PART_MAIN);
    lv_label_set_text(detail_label, "");

    detail_prox = lv_label_create(s_scr);
    lv_obj_set_style_text_font(detail_prox, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(detail_prox, lv_color_make(0x55,0x99,0x55), LV_PART_MAIN);
    lv_label_set_text(detail_prox, "...");
    lv_obj_align(detail_prox, LV_ALIGN_TOP_MID, 0, 340);
    lv_obj_add_flag(detail_prox, LV_OBJ_FLAG_HIDDEN);

    detail_bar_bg = lv_obj_create(s_scr);
    lv_obj_set_size(detail_bar_bg, 372, 26);
    lv_obj_align(detail_bar_bg, LV_ALIGN_TOP_MID, 0, 388);
    lv_obj_set_style_bg_color(detail_bar_bg, lv_color_make(0x22,0x22,0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(detail_bar_bg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(detail_bar_bg, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(detail_bar_bg, 13, LV_PART_MAIN);
    lv_obj_set_style_pad_all(detail_bar_bg, 0, LV_PART_MAIN);
    lv_obj_clear_flag(detail_bar_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(detail_bar_bg, LV_OBJ_FLAG_HIDDEN);
    detail_bar_fill = lv_obj_create(detail_bar_bg);
    lv_obj_set_size(detail_bar_fill, lv_pct(2), 26);
    lv_obj_align(detail_bar_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(detail_bar_fill, lv_color_make(0x55,0x99,0x55), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(detail_bar_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(detail_bar_fill, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(detail_bar_fill, 13, LV_PART_MAIN);
    lv_obj_clear_flag(detail_bar_fill, LV_OBJ_FLAG_SCROLLABLE);

    // popup di allarme (nascosto)
    s_popup = lv_obj_create(s_scr);
    lv_obj_set_size(s_popup, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_popup, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_popup, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_popup, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_popup, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *card = lv_obj_create(s_popup);
    lv_obj_set_size(card, 300, 270);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_make(0x0a, 0x0e, 0x14), LV_PART_MAIN);
    lv_obj_set_style_border_color(card, C_RED, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(card, C_RED, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 12, LV_PART_MAIN);

    lv_obj_t *ptl = lv_label_create(card);
    lv_obj_set_style_text_font(ptl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(ptl, C_RED, LV_PART_MAIN);
    lv_label_set_text(ptl, LV_SYMBOL_WARNING " SKIMMER TROVATO");

    pop_model = lv_label_create(card);
    lv_obj_set_style_text_font(pop_model, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(pop_model, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(pop_model, "-");

    pop_rssi = lv_label_create(card);
    lv_obj_set_style_text_font(pop_rssi, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(pop_rssi, C_AMBER, LV_PART_MAIN);
    lv_label_set_text(pop_rssi, "-");

    lv_obj_t *cbtn = lv_button_create(card);
    lv_obj_set_size(cbtn, 190, 54);
    lv_obj_set_style_bg_color(cbtn, lv_color_make(0x1e, 0x07, 0x10), LV_PART_MAIN);
    lv_obj_set_style_border_color(cbtn, C_RED, LV_PART_MAIN);
    lv_obj_set_style_border_width(cbtn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(cbtn, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(cbtn, on_popup_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cbl = lv_label_create(cbtn);
    lv_obj_set_style_text_color(cbl, C_RED, LV_PART_MAIN);
    lv_obj_set_style_text_font(cbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(cbl, "CHIUDI");
    lv_obj_center(cbl);

    reset_state();
    Serial.printf("[SKIM] create fatto, heap libero=%u\n", (unsigned)ESP.getFreeHeap());

    lv_obj_add_event_cb(s_scr, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_refresh, 250, NULL);
}

void skimmer_screen_show()
{
    Serial.printf("[SKIM] show, heap libero=%u\n", (unsigned)ESP.getFreeHeap());
    s_dev_n = 0;
    s_view  = SK_VIEW_LIST;
    reset_state();
    if (s_popup)      lv_obj_add_flag(s_popup, LV_OBJ_FLAG_HIDDEN);
    if (detail_box)   lv_obj_add_flag(detail_box, LV_OBJ_FLAG_HIDDEN);
    if (detail_prox)  lv_obj_add_flag(detail_prox, LV_OBJ_FLAG_HIDDEN);
    if (detail_bar_bg)lv_obj_add_flag(detail_bar_bg, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(s_scr);
    s_pending_scan = true;
}

bool skimmer_screen_is_active()
{
    return lv_scr_act() == s_scr;
}
