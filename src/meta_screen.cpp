#include "meta_screen.h"
#include "meta_glasses.h"
#include "tools_screen.h"
#include <lvgl.h>
#include <SD.h>
#include <LilyGoLib.h>          // instance.isCardReady()
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_screen = nullptr;
static lv_obj_t *s_status = nullptr;
static lv_obj_t *s_list   = nullptr;
static lv_timer_t *s_timer = nullptr;
static bool      s_active = false;
static uint32_t  s_sig = 0xFFFFFFFF;

// ─── model photo (loaded from SD on tap) ─────────────────────────────────────
// Files live on the microSD as raw RGB565 with a tiny header:
//   /meta/<family>.bin  =  [w:u16 LE][h:u16 LE][ w*h*2 bytes RGB565 LE ]
// family is one of: rayban, oakley, quest, meta  (fallback: glasses).
static lv_obj_t     *s_detail   = nullptr;   // full-screen overlay
static lv_obj_t     *s_detail_img = nullptr;
static lv_obj_t     *s_detail_lbl = nullptr;
static lv_obj_t     *s_detail_sub = nullptr;
static uint8_t      *s_img_buf  = nullptr;   // PSRAM pixel buffer
static lv_image_dsc_t s_img_dsc;

// ── capture mode (optician) ──────────────────────────────────────────────────
// When CAPTURE is on, tapping a row opens a labelling card (not the photo) so
// you can save that device's raw advert tagged with the model you're standing
// in front of. Diff two labelled captures to find a per-model discriminator.
static lv_obj_t *s_cap        = nullptr;     // capture/labelling overlay
static lv_obj_t *s_cap_info   = nullptr;
static lv_obj_t *s_cap_result = nullptr;
static lv_obj_t *s_capbtn     = nullptr;     // CAPTURE toggle on the main screen
static lv_obj_t *s_capbtn_lbl = nullptr;
static MetaHit   s_shown[16];                // last rendered rows (index = user_data)
static int       s_shown_n    = 0;
static uint8_t   s_cap_mac[6] = {0};         // device chosen for labelling
static const char *LBL_WAY  = "WAYFARER";
static const char *LBL_HEAD = "HEADLINER";
static const char *LBL_OAK  = "OAKLEY";
static const char *LBL_OTH  = "ALTRO";

static void show_capture(int idx);
static void hide_capture();
static void update_capbtn();
static void add_nearest_row();

// Which glasses family did the matched identifier point at?
static const char *model_name(uint16_t id)
{
    switch (id) {
        case 0x0D53: return "Ray-Ban / Oakley";   // Luxottica
        case 0x01AB: return "Meta";
        case 0x058E: return "Meta / Quest";
        case 0xFD5F: return "Meta / Quest";
        case 0xFEB7:
        case 0xFEB8: return "Meta";
        default:     return "Meta glasses";
    }
}

// Map the matched identifier to a photo family (SD filename stem).
static const char *family_file(uint16_t id)
{
    switch (id) {
        case 0x058E: case 0xFD5F: return "quest";
        case 0x0D53:              return "rayban";  // Luxottica (Ray-Ban / Oakley)
        default:                  return "meta";    // 0x01AB / 0xFEB7 / 0xFEB8
    }
}

static void free_photo()
{
    if (s_img_buf) { heap_caps_free(s_img_buf); s_img_buf = nullptr; }
}

// Load /meta/<name>.bin ([w:u16][h:u16][RGB565…]) into s_img_dsc. False if absent.
static bool load_photo(const char *name)
{
    free_photo();
    if (!instance.isCardReady()) return false;   // never touch SD when the card isn't mounted
    char path[48];
    snprintf(path, sizeof(path), "/meta/%s.bin", name);
    if (!SD.exists(path)) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint8_t hdr[4];
    if (f.read(hdr, 4) != 4) { f.close(); return false; }
    uint16_t w = hdr[0] | (hdr[1] << 8);
    uint16_t h = hdr[2] | (hdr[3] << 8);
    size_t px = (size_t)w * h * 2;
    if (!w || !h || w > 410 || h > 460) { f.close(); return false; }
    s_img_buf = (uint8_t *)heap_caps_malloc(px, MALLOC_CAP_SPIRAM);
    if (!s_img_buf) { f.close(); return false; }
    size_t rd = f.read(s_img_buf, px);
    f.close();
    if (rd != px) { free_photo(); return false; }
    memset(&s_img_dsc, 0, sizeof(s_img_dsc));
    s_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w      = w;
    s_img_dsc.header.h      = h;
    s_img_dsc.header.stride = w * 2;
    s_img_dsc.data_size     = px;
    s_img_dsc.data          = s_img_buf;
    return true;
}

static void hide_detail()
{
    if (!s_detail) return;
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_image_set_src(s_detail_img, NULL);
    free_photo();
}

static void show_detail(uint16_t id)
{
    const char *fam = family_file(id);
    bool ok = load_photo(fam);
    if (ok) {
        lv_image_set_src(s_detail_img, &s_img_dsc);
        lv_obj_clear_flag(s_detail_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_detail_img, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_detail_lbl, model_name(id));
    if (ok) lv_label_set_text_fmt(s_detail_sub, "0x%04X  ·  tocca per chiudere", id);
    else    lv_label_set_text_fmt(s_detail_sub, "manca /meta/%s.bin sulla SD", fam);
    lv_obj_move_foreground(s_detail);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
}

static void on_row_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_shown_n) return;
    if (meta_glasses_capture_enabled()) show_capture(idx);   // label + save raw
    else                                show_detail(s_shown[idx].id);  // photo
}

static void add_row(int idx)
{
    const MetaHit &h = s_shown[idx];
    bool cap = meta_glasses_capture_enabled();
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, 384, 60);
    lv_obj_set_style_bg_color(row, lv_color_make(0x0A, 0x0A, 0x12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, cap ? lv_color_make(0x00, 0xCC, 0x55)
                                           : lv_color_make(0x33, 0x66, 0xFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 8, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);   // tap → photo, or capture card
    lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *head = lv_label_create(row);
    lv_obj_set_style_text_font(head, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(head, lv_color_make(0x66, 0x99, 0xFF), LV_PART_MAIN);
    lv_label_set_text_fmt(head, LV_SYMBOL_EYE_OPEN "  %s", model_name(h.id));
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, 4, 2);

    lv_obj_t *info = lv_label_create(row);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(info, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text_fmt(info, "%d dBm  ·  id 0x%04X  ·  ..%02X:%02X:%02X  ·  %ux",
        (int)h.rssi, h.id, h.mac[3], h.mac[4], h.mac[5], (unsigned)h.hits);
    lv_obj_align(info, LV_ALIGN_BOTTOM_LEFT, 4, -2);
}

static void refresh()
{
    bool cap = meta_glasses_capture_enabled();
    int n = meta_glasses_count();
    if (cap)
        lv_label_set_text_fmt(s_status,
            LV_SYMBOL_SAVE "  CATTURA · %d salvati · tocca un occhiale",
            meta_glasses_capture_total());
    else
        lv_label_set_text_fmt(s_status,
            n > 0 ? LV_SYMBOL_EYE_OPEN "  %d occhiali smart rilevati"
                  : LV_SYMBOL_REFRESH "  Scansione BLE...  (%d)", n);

    int got = meta_glasses_get(s_shown, 16);
    s_shown_n = got;
    uint32_t sig = (uint32_t)got * 2654435761u ^ (cap ? 0x5A5A5A5Au : 0);
    for (int i = 0; i < got; i++)
        sig ^= (uint32_t)(s_shown[i].mac[5] ^ (uint8_t)s_shown[i].rssi ^ s_shown[i].id) << ((i % 4) * 8);
    if (sig == s_sig) return;
    s_sig = sig;

    lv_obj_clean(s_list);
    if (got == 0) {
        lv_obj_t *e = lv_label_create(s_list);
        lv_obj_set_style_text_font(e, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(e, lv_color_make(0x55, 0x66, 0x99), LV_PART_MAIN);
        lv_label_set_text(e,
            "Nessun occhiale smart nei paraggi.\n\n"
            "Rileva Ray-Ban Stories/Meta,\n"
            "Oakley Meta e Quest via BLE\n"
            "(company id Meta/Luxottica).");
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        return;
    }
    if (cap) add_nearest_row();                 // one-tap: grab the closest pair
    for (int i = 0; i < got; i++) add_row(i);
}

static void on_timer(lv_timer_t *) { if (s_active) refresh(); }

static void on_clear(lv_event_t *) { meta_glasses_reset(); s_sig = 0xFFFFFFFF; refresh(); }

// ── capture card ─────────────────────────────────────────────────────────────
static void hide_capture()
{
    if (s_cap) lv_obj_add_flag(s_cap, LV_OBJ_FLAG_HIDDEN);
}

static void show_capture(int idx)
{
    const MetaHit &h = s_shown[idx];
    memcpy(s_cap_mac, h.mac, 6);
    lv_label_set_text_fmt(s_cap_info,
        "%02X:%02X:%02X:%02X:%02X:%02X\nid 0x%04X · %d dBm · %ux",
        h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5],
        h.id, (int)h.rssi, (unsigned)h.hits);
    lv_label_set_text(s_cap_result, "Scegli il modello che hai davanti per salvarlo");
    lv_obj_set_style_text_color(s_cap_result, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_move_foreground(s_cap);
    lv_obj_clear_flag(s_cap, LV_OBJ_FLAG_HIDDEN);
}

// One-tap capture of the closest device. s_shown is sorted by RSSI, so index 0
// is the strongest — hold the pair against the watch and it's guaranteed top.
static void on_near_click(lv_event_t *)
{
    if (s_shown_n > 0) show_capture(0);
}

static void add_nearest_row()
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, 384, 54);
    lv_obj_set_style_bg_color(row, lv_color_make(0x06, 0x22, 0x10), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_make(0x00, 0xCC, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, on_near_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_make(0x00, 0xFF, 0x66), LV_PART_MAIN);
    lv_label_set_text_fmt(l, LV_SYMBOL_GPS "  CATTURA IL PIU' VICINO  (%d dBm)",
                          (int)s_shown[0].rssi);
    lv_obj_center(l);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static void on_cap_label(lv_event_t *e)
{
    const char *lbl = (const char *)lv_event_get_user_data(e);
    bool ok = meta_glasses_save_labeled(s_cap_mac, lbl);
    if (ok) {
        lv_label_set_text_fmt(s_cap_result,
            LV_SYMBOL_OK "  salvato [%s] · tot %d\n(ritoccalo per un altro campione)",
            lbl, meta_glasses_capture_total());
        lv_obj_set_style_text_color(s_cap_result, lv_color_make(0x00, 0xCC, 0x55), LV_PART_MAIN);
    } else {
        lv_label_set_text(s_cap_result,
            LV_SYMBOL_WARNING "  non salvato (device sparito o SD non pronta)");
        lv_obj_set_style_text_color(s_cap_result, lv_color_make(0xFF, 0x66, 0x00), LV_PART_MAIN);
    }
}

static void update_capbtn()
{
    bool on = meta_glasses_capture_enabled();
    lv_obj_set_style_bg_color(s_capbtn,
        on ? lv_color_make(0x00, 0x88, 0x33) : lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_capbtn,
        on ? lv_color_make(0x00, 0xCC, 0x55) : lv_color_make(0x33, 0x66, 0xFF), LV_PART_MAIN);
    lv_label_set_text(s_capbtn_lbl, on ? LV_SYMBOL_SAVE "  CAPTURE ON"
                                       : LV_SYMBOL_SAVE "  CAPTURE");
}

static void on_capture_toggle(lv_event_t *)
{
    meta_glasses_set_capture(!meta_glasses_capture_enabled());
    if (!meta_glasses_capture_enabled()) hide_capture();
    update_capbtn();
    s_sig = 0xFFFFFFFF;   // force list + status rebuild (border colour changes)
    refresh();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) {
        if (s_cap && !lv_obj_has_flag(s_cap, LV_OBJ_FLAG_HIDDEN)) {
            hide_capture();   // capture card open → close it first
            return;
        }
        if (s_detail && !lv_obj_has_flag(s_detail, LV_OBJ_FLAG_HIDDEN)) {
            hide_detail();    // photo card open → close it first
            return;
        }
        s_active = false;
        meta_glasses_stop();
        tools_screen_show();
    }
}

void meta_screen_create()
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, NULL);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_make(0x66, 0x99, 0xFF), LV_PART_MAIN);
    lv_label_set_text(title, "META GLASSES");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    s_status = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(s_status, LV_SYMBOL_REFRESH "  Scansione BLE...");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 60);

    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, 404, 340);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_list, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 92);

    lv_obj_t *clr = lv_obj_create(s_screen);
    lv_obj_set_size(clr, 150, 40);
    lv_obj_set_style_radius(clr, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(clr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(clr, lv_color_make(0x33, 0x66, 0xFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(clr, 1, LV_PART_MAIN);
    lv_obj_clear_flag(clr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(clr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(clr, LV_ALIGN_BOTTOM_MID, -80, -14);   // left of centre (off the corners)
    lv_obj_add_event_cb(clr, on_clear, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clbl = lv_label_create(clr);
    lv_obj_set_style_text_font(clbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(clbl, lv_color_make(0x66, 0x99, 0xFF), LV_PART_MAIN);
    lv_label_set_text(clbl, LV_SYMBOL_TRASH "  CLEAR");
    lv_obj_center(clbl);
    lv_obj_add_flag(clbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    // CAPTURE toggle (right of centre). On → tapping a row saves its raw advert
    // tagged with a model, for diffing pairs at the optician.
    s_capbtn = lv_obj_create(s_screen);
    lv_obj_set_size(s_capbtn, 150, 40);
    lv_obj_set_style_radius(s_capbtn, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_capbtn, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_capbtn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_capbtn, lv_color_make(0x33, 0x66, 0xFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_capbtn, 1, LV_PART_MAIN);
    lv_obj_clear_flag(s_capbtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_capbtn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_capbtn, LV_ALIGN_BOTTOM_MID, 80, -14);
    lv_obj_add_event_cb(s_capbtn, on_capture_toggle, LV_EVENT_CLICKED, NULL);
    s_capbtn_lbl = lv_label_create(s_capbtn);
    lv_obj_set_style_text_font(s_capbtn_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_capbtn_lbl, lv_color_make(0x66, 0x99, 0xFF), LV_PART_MAIN);
    lv_label_set_text(s_capbtn_lbl, LV_SYMBOL_SAVE "  CAPTURE");
    lv_obj_center(s_capbtn_lbl);
    lv_obj_add_flag(s_capbtn_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Photo detail overlay — full screen, tap anywhere to close.
    s_detail = lv_obj_create(s_screen);
    lv_obj_set_size(s_detail, 410, 502);
    lv_obj_align(s_detail, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_detail, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_detail, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_detail, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_detail, [](lv_event_t *) { hide_detail(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);

    s_detail_lbl = lv_label_create(s_detail);
    lv_obj_set_style_text_font(s_detail_lbl, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_detail_lbl, lv_color_make(0x66, 0x99, 0xFF), LV_PART_MAIN);
    lv_label_set_text(s_detail_lbl, "");
    lv_obj_align(s_detail_lbl, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_add_flag(s_detail_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_detail_img = lv_image_create(s_detail);
    lv_obj_align(s_detail_img, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_detail_img, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_detail_sub = lv_label_create(s_detail);
    lv_obj_set_style_text_font(s_detail_sub, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_detail_sub, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(s_detail_sub, "");
    lv_obj_align(s_detail_sub, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_add_flag(s_detail_sub, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Capture / labelling overlay — device info + one button per model.
    s_cap = lv_obj_create(s_screen);
    lv_obj_set_size(s_cap, 410, 502);
    lv_obj_align(s_cap, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_cap, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_cap, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cap, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_cap, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_cap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cap, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *ct = lv_label_create(s_cap);
    lv_obj_set_style_text_font(ct, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(ct, lv_color_make(0x00, 0xCC, 0x55), LV_PART_MAIN);
    lv_label_set_text(ct, LV_SYMBOL_SAVE "  REGISTRA MODELLO");
    lv_obj_align(ct, LV_ALIGN_TOP_MID, 0, 26);

    s_cap_info = lv_label_create(s_cap);
    lv_obj_set_style_text_font(s_cap_info, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_cap_info, lv_color_make(0x66, 0x99, 0xFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_cap_info, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_cap_info, "");
    lv_obj_align(s_cap_info, LV_ALIGN_TOP_MID, 0, 66);

    struct { const char *txt; const char *lbl; } models[4] = {
        { "Ray-Ban Stories (Wayfarer)", LBL_WAY },
        { "Ray-Ban Meta (Headliner)",   LBL_HEAD },
        { "Oakley Meta",                LBL_OAK },
        { "Altro / sconosciuto",        LBL_OTH },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_obj_create(s_cap);
        lv_obj_set_size(b, 300, 46);
        lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_make(0x0A, 0x1A, 0x0A), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(b, lv_color_make(0x00, 0xCC, 0x55), LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 140 + i * 56);
        lv_obj_add_event_cb(b, on_cap_label, LV_EVENT_CLICKED, (void *)models[i].lbl);
        lv_obj_t *bl = lv_label_create(b);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(bl, lv_color_white(), LV_PART_MAIN);
        lv_label_set_text(bl, models[i].txt);
        lv_obj_center(bl);
        lv_obj_add_flag(bl, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    s_cap_result = lv_label_create(s_cap);
    lv_obj_set_style_text_font(s_cap_result, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_cap_result, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_cap_result, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_cap_result, 360);
    lv_label_set_long_mode(s_cap_result, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_cap_result, "");
    lv_obj_align(s_cap_result, LV_ALIGN_TOP_MID, 0, 372);

    lv_obj_t *cx = lv_obj_create(s_cap);
    lv_obj_set_size(cx, 150, 40);
    lv_obj_set_style_radius(cx, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cx, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cx, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(cx, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    lv_obj_set_style_border_width(cx, 1, LV_PART_MAIN);
    lv_obj_clear_flag(cx, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cx, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(cx, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(cx, [](lv_event_t *) { hide_capture(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cxl = lv_label_create(cx);
    lv_obj_set_style_text_font(cxl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(cxl, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_label_set_text(cxl, LV_SYMBOL_CLOSE "  CHIUDI");
    lv_obj_center(cxl);
    lv_obj_add_flag(cxl, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_timer = lv_timer_create(on_timer, 500, NULL);
}

void meta_screen_show()
{
    if (!s_screen) meta_screen_create();
    s_active = true;
    s_sig = 0xFFFFFFFF;
    meta_glasses_start();
    update_capbtn();
    refresh();
    lv_scr_load(s_screen);
}

bool meta_screen_is_active() { return s_active; }
