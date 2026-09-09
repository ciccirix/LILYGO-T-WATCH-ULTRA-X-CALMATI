#include "arp_mitm_screen.h"
#include "arp_mitm.h"
#include <LilyGoLib.h>
#include <string.h>
#include <stdio.h>

// Defined in tools_screen.cpp
void tools_screen_show();

enum AState {
    AST_NET,        // connecting / needs WiFi
    AST_NOWIFI,     // no saved creds — tell the user to connect first
    AST_READY,      // connected, tap SCAN
    AST_SWEEPING,   // ARP sweep running
    AST_LIST,       // host list shown, tap to target
    AST_RUNNING,    // poison in progress
};

static lv_obj_t *screen;
static lv_obj_t *status_label;
static lv_obj_t *list_box;
static lv_obj_t *btn_scan, *btn_scan_lbl;
static lv_obj_t *btn_atk,  *btn_atk_lbl;

static AState s_state = AST_NET;
static int    s_sel   = -1;          // -1 none, ARP_TARGET_ALL block-all, >=0 host idx

// ---- list rendering ---------------------------------------------------------

static void on_row_clicked(lv_event_t *e);

static lv_obj_t *make_card(bool selected, lv_color_t sel_bg, lv_color_t sel_border,
                           int click_id)
{
    lv_obj_t *card = lv_obj_create(list_box);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card,
        selected ? sel_bg : lv_color_make(0x16, 0x16, 0x16), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card,
        selected ? sel_border : lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, selected ? 2 : 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 2, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, on_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)click_id);
    return card;
}

static void show_hosts()
{
    lv_obj_clean(list_box);

    // Synthetic first row: whole-/24 blackhole.
    {
        bool sel = (s_sel == ARP_TARGET_ALL);
        lv_obj_t *card = make_card(sel, lv_color_make(0x3a, 0x10, 0x10),
                                   lv_color_make(0xcc, 0x33, 0x33), ARP_TARGET_ALL);
        lv_obj_t *l1 = lv_label_create(card);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(l1, lv_color_make(0xFF, 0x55, 0x55), LV_PART_MAIN);
        lv_label_set_text(l1, "BLOCK ALL  /24");
        lv_obj_t *l2 = lv_label_create(card);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l2, lv_color_make(0x99, 0x55, 0x55), LV_PART_MAIN);
        lv_label_set_text(l2, "blackhole the whole subnet");
    }

    int n = arp_mitm_host_count();
    if (n == 0) {
        lv_obj_t *l = lv_label_create(list_box);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, lv_color_make(0x00, 0x66, 0x00), LV_PART_MAIN);
        lv_label_set_text(l, "No hosts found");
        return;
    }

    uint8_t pfx[3];
    arp_mitm_prefix(pfx);
    for (int i = 0; i < n; i++) {
        uint8_t ip4 = 0, mac[6] = {0}; bool is_gw = false;
        if (!arp_mitm_host(i, &ip4, mac, &is_gw)) continue;
        bool sel = (s_sel == i);
        lv_obj_t *card = make_card(sel, lv_color_make(0x10, 0x28, 0x10),
                                   lv_color_make(0x33, 0xcc, 0x33), i);

        char line[48];
        snprintf(line, sizeof(line), "%u.%u.%u.%u%s", pfx[0], pfx[1], pfx[2], ip4,
                 is_gw ? "  [GW]" : "");
        lv_obj_t *l1 = lv_label_create(card);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(l1,
            is_gw ? lv_color_make(0xFF, 0xCC, 0x00) : lv_color_make(0x00, 0xFF, 0x00),
            LV_PART_MAIN);
        lv_label_set_text(l1, line);

        char det[40];
        snprintf(det, sizeof(det), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        lv_obj_t *l2 = lv_label_create(card);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(l2, lv_color_make(0x00, 0x99, 0x00), LV_PART_MAIN);
        lv_label_set_text(l2, det);
    }
}

// ---- status / buttons -------------------------------------------------------

static void update_status()
{
    char buf[96];
    const char *txt = buf;
    lv_color_t  col = lv_color_make(0x88, 0x88, 0x88);

    switch (s_state) {
    case AST_NET:
        txt = "Connecting to WiFi...";
        col = lv_color_make(0xFF, 0xCC, 0x00);
        break;
    case AST_NOWIFI:
        txt = "Connect WiFi first (WiFi app)";
        col = lv_color_make(0xFF, 0x88, 0x00);
        break;
    case AST_READY: {
        char info[80];
        arp_mitm_net_info(info, sizeof(info));
        snprintf(buf, sizeof(buf), "%s", info);
        col = lv_color_make(0x00, 0x99, 0x00);
        break;
    }
    case AST_SWEEPING:
        txt = "ARP sweep...";
        col = lv_color_make(0xFF, 0xCC, 0x00);
        break;
    case AST_LIST:
        if (s_sel == ARP_TARGET_ALL)      snprintf(buf, sizeof(buf), "Target: WHOLE /24 (block)");
        else if (s_sel >= 0) {
            uint8_t ip4 = 0, mac[6]; bool gw; uint8_t p[3]; arp_mitm_prefix(p);
            arp_mitm_host(s_sel, &ip4, mac, &gw);
            snprintf(buf, sizeof(buf), "Target: %u.%u.%u.%u", p[0], p[1], p[2], ip4);
        } else snprintf(buf, sizeof(buf), "%d hosts - tap one to target", arp_mitm_host_count());
        break;
    case AST_RUNNING:
        snprintf(buf, sizeof(buf), "%s  -  %lu cycles",
                 (s_sel == ARP_TARGET_ALL) ? "BLOCKING /24" : "MitM active",
                 (unsigned long)arp_mitm_cycles());
        col = lv_color_make(0xFF, 0x44, 0x44);
        break;
    }
    lv_label_set_text(status_label, txt);
    lv_obj_set_style_text_color(status_label, col, LV_PART_MAIN);
}

static void update_buttons()
{
    if (s_state == AST_RUNNING) {
        lv_obj_add_flag(btn_scan, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(btn_atk_lbl, "STOP");
        lv_obj_set_style_bg_color(btn_atk, lv_color_make(0x88, 0x22, 0x22), LV_PART_MAIN);
        lv_obj_align(btn_atk, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    // SCAN needs a live association; hide it until connected.
    bool can_scan = (s_state == AST_READY || s_state == AST_LIST || s_state == AST_SWEEPING);
    if (can_scan) {
        lv_obj_clear_flag(btn_scan, LV_OBJ_FLAG_HIDDEN);
        const char *sl = (s_state == AST_SWEEPING) ? "SCANNING..." :
                         (arp_mitm_host_count() > 0) ? "RESCAN" : "SCAN";
        lv_label_set_text(btn_scan_lbl, sl);
    } else {
        lv_obj_add_flag(btn_scan, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_sel != -1 && s_state == AST_LIST) {
        lv_obj_clear_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(btn_atk_lbl, "START");
        lv_obj_set_style_bg_color(btn_atk, lv_color_make(0xCC, 0x33, 0x33), LV_PART_MAIN);
        lv_obj_align(btn_scan, LV_ALIGN_LEFT_MID,  6, 0);
        lv_obj_align(btn_atk,  LV_ALIGN_RIGHT_MID, -6, 0);
    } else {
        lv_obj_add_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);
        if (can_scan) lv_obj_align(btn_scan, LV_ALIGN_CENTER, 0, 0);
    }
}

// ---- events -----------------------------------------------------------------

static void on_row_clicked(lv_event_t *e)
{
    if (s_state != AST_LIST) return;
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    s_sel = id;                      // host index, or ARP_TARGET_ALL
    show_hosts();
    update_status();
    update_buttons();
}

static void on_scan_btn(lv_event_t *)
{
    if (s_state != AST_READY && s_state != AST_LIST) return;
    s_sel = -1;
    arp_mitm_request_sweep();
    s_state = AST_SWEEPING;
    lv_obj_clean(list_box);
    update_status();
    update_buttons();
}

static void on_attack_btn(lv_event_t *)
{
    if (s_state == AST_RUNNING) {
        arp_mitm_stop();             // heals + cycles the radio down
        s_sel = -1;
        lv_obj_clean(list_box);
        // Re-establish the link so the user can scan/attack again without a
        // trip back to the WiFi app (handles autoconnect being off).
        ArpNet n = arp_mitm_prepare();
        s_state = (n == ARP_NET_CONNECTED) ? AST_READY :
                  (n == ARP_NET_NONE)      ? AST_NOWIFI : AST_NET;
    } else if (s_state == AST_LIST && s_sel != -1) {
        if (arp_mitm_start(s_sel)) s_state = AST_RUNNING;
    }
    update_status();
    update_buttons();
}

static void on_refresh(lv_timer_t *)
{
    if (lv_screen_active() != screen) return;

    switch (s_state) {
    case AST_NET: {
        ArpNet n = arp_mitm_poll_net();
        if (n == ARP_NET_CONNECTED) { s_state = AST_READY; update_status(); update_buttons(); }
        else if (n == ARP_NET_NONE) { s_state = AST_NOWIFI; update_status(); update_buttons(); }
        break;
    }
    case AST_SWEEPING:
        if (!arp_mitm_sweeping() && arp_mitm_sweep_done()) {
            s_state = AST_LIST;
            show_hosts();
            update_status();
            update_buttons();
        }
        break;
    case AST_RUNNING:
        update_status();
        break;
    default:
        break;
    }
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        arp_mitm_screen_stop();
        tools_screen_show();
    }
}

// ---- layout -----------------------------------------------------------------

static lv_obj_t *make_button(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                             lv_color_t bg, lv_obj_t **label_out)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_center(l);
    if (label_out) *label_out = l;
    return b;
}

void arp_mitm_screen_create()
{
    screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "ARP MitM");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *warn = lv_label_create(screen);
    lv_obj_set_style_text_font(warn, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(warn, lv_color_make(0x88, 0x55, 0x55), LV_PART_MAIN);
    lv_label_set_text(warn, "Only on networks you're authorised to test");
    lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 58);

    status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x00, 0x88, 0x00), LV_PART_MAIN);
    lv_label_set_text(status_label, "Connecting to WiFi...");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 82);

    lv_obj_t *btn_row = lv_obj_create(screen);
    lv_obj_set_size(btn_row, 404, 50);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(btn_row, LV_ALIGN_TOP_MID, 0, 108);

    btn_scan = make_button(btn_row, 196, 48, lv_color_make(0x00, 0x88, 0xCC), &btn_scan_lbl);
    lv_obj_align(btn_scan, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(btn_scan, on_scan_btn, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn_scan_lbl, "SCAN");
    lv_obj_add_flag(btn_scan, LV_OBJ_FLAG_HIDDEN);

    btn_atk = make_button(btn_row, 196, 48, lv_color_make(0xCC, 0x33, 0x33), &btn_atk_lbl);
    lv_obj_align(btn_atk, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_event_cb(btn_atk, on_attack_btn, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(btn_atk_lbl, "START");
    lv_obj_add_flag(btn_atk, LV_OBJ_FLAG_HIDDEN);

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
    lv_timer_create(on_refresh, 500, NULL);
}

void arp_mitm_screen_show()
{
    s_sel = -1;
    lv_obj_clean(list_box);
    ArpNet n = arp_mitm_prepare();
    if (n == ARP_NET_CONNECTED)      s_state = AST_READY;
    else if (n == ARP_NET_NONE)      s_state = AST_NOWIFI;
    else                             s_state = AST_NET;
    update_status();
    update_buttons();
    lv_scr_load(screen);
}

void arp_mitm_screen_stop()
{
    if (arp_mitm_is_running() || s_state == AST_LIST || s_state == AST_SWEEPING)
        arp_mitm_stop();
    s_sel = -1;
    s_state = AST_NET;
}

bool arp_mitm_screen_is_active()
{
    return lv_screen_active() == screen;
}
