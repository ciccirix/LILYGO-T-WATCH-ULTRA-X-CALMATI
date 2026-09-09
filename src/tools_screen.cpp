#include "tools_screen.h"
#include "esp32-hal-tinyusb.h"    // usb_persist_restart(RESTART_BOOTLOADER) — tile Download
#include "gps_screen.h"           // gps_screen_power_on() per la tile UDDA
#include "meshtastic.h"           // meshtastic_set_active/announce/short_name
#include "meshtastic_screen.h"    // meshtastic_screen_show()
#include "lora_screen.h"          // lora_screen_force_on() per la tile UDDA
#include "voice_screen.h"         // voice_screen_show() per la tile Assistente
#include <LilyGoLib.h>            // instance, radio, POWER_RADIO, initLoRa
#include "pager.h"                // pager_stop()  (libera la radio SX1262)
#include "tpms.h"                 // tpms_stop()
#include "aprs.h"                 // aprs_stop()
#include "airtag.h"
#include "skimmer.h"
#include "skimmer_screen.h"
#include "evil_twin.h"
#include "evil_twin_screen.h"
#include "flock.h"
#include "tesla_cp_screen.h"
#include "tpms_screen.h"
#include "pager_screen.h"
#include "mouse_screen.h"
#include "usb_sd_screen.h"
#include "aprs_screen.h"
#include "wifi_screen.h"
#include "analyze_screen.h"
#include "threat_radar_screen.h"
#include "adsb_screen.h"
#include "mic_screen.h"
#include "webpanel_screen.h"
#include "calmati_remote_screen.h"

// Calmati brand logo (1-bit A1 mask, src/calmati_icon.c — C linkage).
extern "C" const lv_image_dsc_t calmati_icon;
#include "scan_screen.h"
#include "car_screen.h"
#include "meta_screen.h"
#include "task_manager.h"
#include "pet_screen.h"
#include "stealth.h"
#include "handshake.h"
#include "camera_screen.h"
#include "deauther_screen.h"
#include "arp_mitm_screen.h"
#include "ble_pair_screen.h"
#include "drone_screen.h"
#include "gatt_explorer_screen.h"
#include "phantom_flood_screen.h"
#include "rid_spoof_screen.h"
#include "subghz_sentinel_screen.h"
#include "printer_screen.h"
#include "tile_icons.h"
#include "mifare_screen.h"
#include "badusb_screen.h"
#include "waterfall_screen.h"
#include "wpa3_sae_screen.h"
#include "espnow_screen.h"
#include "nfc_credit_screen.h"
#include <LilyGoLib.h>

// Defined in main.cpp
void clock_screen_show();
void main_loop_request_lvgl_priority(int cycles);

static lv_obj_t *tools_screen;
static lv_obj_t *t_airtag;    // referenced by on_airtag_clicked for colour swap
static lv_obj_t *t_skimmer;   // referenced by on_skimmer_clicked for colour swap
static lv_obj_t *t_eviltwin;  // opens the active Evil Twin panel (no colour swap)
static lv_obj_t *t_flock;     // referenced by on_flock_clicked for colour swap
static lv_obj_t *t_duress;    // referenced by on_duress_clicked for colour swap
static lv_obj_t *t_handshake; // referenced by on_handshake_clicked for colour swap
static lv_obj_t *s_tools_grid = nullptr; // the scrollable tile grid, for on_gesture

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);

    // The tile grid scrolls vertically and holds far more tiles than fit on one
    // screen, so an up/down swipe normally means "scroll to more tiles". Only
    // treat the swipe as "leave the Tools screen" when the grid is already at
    // the matching edge (nothing more to reveal that way). Otherwise every
    // upward flick would exit to the clock and the lower tiles (Cameras,
    // Deauth, Waterfall, ESP-NOW) would be unreachable.
    if (dir == LV_DIR_TOP) {
        if (!s_tools_grid || lv_obj_get_scroll_bottom(s_tools_grid) <= 4)
            clock_screen_show();
    } else if (dir == LV_DIR_BOTTOM) {
        if (s_tools_grid && lv_obj_get_scroll_top(s_tools_grid) <= 4)
            clock_screen_show();
    }
}

static void set_airtag_tile_running(bool running)
{
    lv_obj_set_style_bg_color(t_airtag,
        running ? lv_color_make(0x00, 0x55, 0x22)
                : lv_color_make(0x11, 0x11, 0x11),
        LV_PART_MAIN);
}

static void on_airtag_clicked(lv_event_t *e)
{
    if (airtag_is_running()) {
        airtag_stop();
        set_airtag_tile_running(false);
    } else {
        bool ok = airtag_start();
        set_airtag_tile_running(ok);   // stays gray if BT init failed
    }
}

static void set_skimmer_tile_running(bool running)
{
    lv_obj_set_style_bg_color(t_skimmer,
        running ? lv_color_make(0x00, 0x55, 0x22)
                : lv_color_make(0x11, 0x11, 0x11),
        LV_PART_MAIN);
}

static void on_skimmer_clicked(lv_event_t *e)
{
    // Opens the dedicated interactive scanner (bounded scan through the fixed
    // stack_up — no more freezing background toggle).
    skimmer_screen_show();
}

// The evil-twin DETECTOR now runs inside the unified Scanner, so this tile is
// repurposed as the ACTIVE Evil Twin panel (scan → clone → capture). No more
// toggle/colour-swap here — it just opens the screen.

static void set_flock_tile_running(bool running)
{
    lv_obj_set_style_bg_color(t_flock,
        running ? lv_color_make(0x00, 0x55, 0x22)
                : lv_color_make(0x11, 0x11, 0x11),
        LV_PART_MAIN);
}

static void on_flock_clicked(lv_event_t *e)
{
    if (flock_is_running()) {
        flock_stop();
        set_flock_tile_running(false);
    } else {
        bool ok = flock_start();
        set_flock_tile_running(ok);
    }
}

// Duress disguise arm/disarm. Green when armed; a shake then drops the watch
// into the bare-clock disguise (which survives reboot until the 4 s hold).
static void set_duress_tile_armed(bool armed)
{
    lv_obj_set_style_bg_color(t_duress,
        armed ? lv_color_make(0x00, 0x55, 0x22)
              : lv_color_make(0x11, 0x11, 0x11),
        LV_PART_MAIN);
}

static void on_duress_clicked(lv_event_t *)
{
    bool now = !stealth_armed();
    stealth_set_armed(now);
    set_duress_tile_armed(now);
}

// Passive WPA handshake / PMKID capture toggle. Green while capturing.
static void set_handshake_tile_running(bool on)
{
    lv_obj_set_style_bg_color(t_handshake,
        on ? lv_color_make(0x00, 0x55, 0x22) : lv_color_make(0x11, 0x11, 0x11),
        LV_PART_MAIN);
}

static void on_handshake_clicked(lv_event_t *)
{
    if (handshake_is_running()) {
        handshake_stop();
        set_handshake_tile_running(false);
    } else {
        bool ok = handshake_start();
        set_handshake_tile_running(ok);
    }
}

// Tile container — 180x180 button-like card with a label at the bottom.
// The icon-drawing helpers below fill the upper portion using LVGL primitives
// (no image assets needed). The tile is clickable so future feature wiring
// is a single lv_obj_add_event_cb call per tile.
// Force the ROM USB/UART download mode on the next boot, then reset. Lets us
// reflash the watch without the BOOT-button dance: it reappears as the ROM
// USB-JTAG (esptool-ready), then reboots to normal after flashing.
static void enter_download_mode()
{
    // TinyUSB (USB-OTG) board: this persists USB and reboots into the ROM
    // USB-Serial-JTAG bootloader with the USB link kept alive, so the watch
    // re-enumerates ready for esptool (same effect as the DTR/RTS reset-dance).
    usb_persist_restart(RESTART_BOOTLOADER);
}

static lv_obj_t *make_tile(lv_obj_t *parent, const char *label_text)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, 180, 180);
    lv_obj_set_style_bg_color(tile, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(tile, lv_color_make(0x00, 0x44, 0x00), LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(tile, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(tile);
    lv_obj_set_style_text_color(lbl, lv_color_make(0x00, 0xCC, 0x00), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(lbl, label_text);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -12);

    return tile;
}

// Make an icon and all its nested parts transparent to touch, so a tap anywhere
// on a tile reaches the tile's own CLICKED handler instead of being eaten by a
// decorative child shape.
static void tile_icons_passthrough(lv_obj_t *obj)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
        tile_icons_passthrough(c);
    }
}

// Upper-left: WiFi — signal glyph in cyan, for the site-survey / ping-sweep tool
static void draw_wifi_icon(lv_obj_t *tile)
{
    lv_obj_t *wifi = lv_label_create(tile);
    lv_obj_set_style_text_color(wifi, lv_color_make(0x33, 0xBB, 0xFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(wifi, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(wifi, LV_SYMBOL_WIFI);
    lv_obj_align(wifi, LV_ALIGN_TOP_MID, 0, 44);
}

// Analyze — spectrum-analyzer logo: a row of vertical bars at varying heights
// sitting on a baseline, colours stepping from green through yellow to red to
// suggest channel saturation.
static void draw_analyzer_icon(lv_obj_t *tile)
{
    // Baseline (axis) under the bars.
    lv_obj_t *base = lv_obj_create(tile);
    lv_obj_set_size(base, 116, 2);
    lv_obj_set_style_bg_color(base, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(base, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(base, 0, LV_PART_MAIN);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(base, LV_ALIGN_TOP_MID, 0, 116);

    // Bar heights / colours give a spectrum-analyzer look with a clear peak.
    static const int heights[7]    = { 22, 44, 70, 96, 78, 50, 30 };
    static const uint32_t colors[7] = {
        0x00CC66, 0x00CC66, 0x44BBFF, 0xFFCC00,
        0xFF8844, 0xFFCC00, 0x00CC66,
    };
    const int bar_w = 12;
    const int gap   = 4;
    const int total = 7 * bar_w + 6 * gap;     // 84 + 24 = 108 px
    const int start = (180 - total) / 2;       // = 36 px

    for (int i = 0; i < 7; i++) {
        lv_obj_t *b = lv_obj_create(tile);
        lv_obj_set_size(b, bar_w, heights[i]);
        lv_obj_set_style_bg_color(b,
            lv_color_make((colors[i] >> 16) & 0xFF,
                          (colors[i] >>  8) & 0xFF,
                          (colors[i]      ) & 0xFF),
            LV_PART_MAIN);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(b, 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        // Each bar stands on the baseline at y=116, growing upward.
        lv_obj_align(b, LV_ALIGN_TOP_LEFT,
                     start + i * (bar_w + gap),
                     116 - heights[i]);
    }
}

// Upper-right: AirTag — round disc with a small dot in the centre
static void draw_airtag_icon(lv_obj_t *tile)
{
    // Outer disc (off-white)
    lv_obj_t *outer = lv_obj_create(tile);
    lv_obj_set_size(outer, 84, 84);
    lv_obj_set_style_radius(outer, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(outer, lv_color_make(0xEE, 0xEE, 0xEE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(outer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(outer, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_border_width(outer, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(outer, 0, LV_PART_MAIN);
    lv_obj_clear_flag(outer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(outer, LV_ALIGN_TOP_MID, 0, 28);

    // Small dot (the AirTag's Apple-logo placement)
    lv_obj_t *dot = lv_obj_create(tile);
    lv_obj_set_size(dot, 20, 20);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_make(0x99, 0x99, 0x99), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dot, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 60);
}

// Small helper: a filled circle on the tile at (dx,dy) from top-mid.
static lv_obj_t *icon_dot(lv_obj_t *tile, int sz, lv_color_t col, int dx, int dy) {
    lv_obj_t *o = lv_obj_create(tile);
    lv_obj_set_size(o, sz, sz);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, col, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(o, LV_ALIGN_TOP_MID, dx, dy);
    return o;
}
static lv_obj_t *icon_rect(lv_obj_t *tile, int w, int h, int rad, lv_color_t col, int dx, int dy) {
    lv_obj_t *o = lv_obj_create(tile);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, rad, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, col, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(o, LV_ALIGN_TOP_MID, dx, dy);
    return o;
}

// Glyph-Neon image icons (generated by tools/gen_tile_icons.py, ARGB8888 84x84).
static void place_icon(lv_obj_t *tile, const lv_image_dsc_t *dsc) {
    lv_obj_t *img = lv_image_create(tile);
    lv_image_set_src(img, dsc);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 26);
}

// Drone ID — quadcopter: central body + 4 rotor discs.
static void draw_drone_icon(lv_obj_t *tile) {
    lv_color_t blue = lv_color_make(0x33, 0xcc, 0xff);
    icon_dot(tile, 20, blue, 0, 44);              // body
    icon_dot(tile, 16, blue, -30, 30);            // NW rotor
    icon_dot(tile, 16, blue,  30, 30);            // NE rotor
    icon_dot(tile, 16, blue, -30, 62);            // SW rotor
    icon_dot(tile, 16, blue,  30, 62);            // SE rotor
}

// GATT / SkeletonKey — a key: ring bow + shaft + two teeth.
static void draw_key_icon(lv_obj_t *tile) {
    lv_color_t pur = lv_color_make(0xcc, 0x99, 0xff);
    lv_obj_t *bow = icon_dot(tile, 34, pur, -14, 30);   // bow (ring)
    lv_obj_set_style_bg_opa(bow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(bow, pur, LV_PART_MAIN);
    lv_obj_set_style_border_width(bow, 6, LV_PART_MAIN);
    icon_rect(tile, 40, 8, 4, pur, 12, 44);             // shaft
    icon_rect(tile, 8, 16, 2, pur, 26, 52);             // tooth 1
    icon_rect(tile, 8, 12, 2, pur, 34, 52);             // tooth 2
}

// AirTag Replay — two stacked "loop" bars with arrow-ish ends (broadcast copy).
static void draw_replay_icon(lv_obj_t *tile) {
    lv_color_t org = lv_color_make(0xff, 0xaa, 0x00);
    icon_rect(tile, 44, 10, 5, org, 6, 34);       // top bar
    icon_dot(tile, 14, org, 30, 32);              // top arrow head
    icon_rect(tile, 44, 10, 5, org, -6, 58);      // bottom bar
    icon_dot(tile, 14, org, -30, 56);             // bottom arrow head
}

// Phantom Flood — a little ghost: round head + two eyes.
static void draw_phantom_icon(lv_obj_t *tile) {
    lv_color_t pk = lv_color_make(0xff, 0x66, 0xcc);
    icon_rect(tile, 44, 52, 22, pk, 0, 30);       // rounded body (top rounded)
    icon_dot(tile, 10, lv_color_black(), -9, 46);  // left eye
    icon_dot(tile, 10, lv_color_black(),  9, 46);  // right eye
}

// RID Spoof — drone body + a broadcast dot above (fake Remote ID).
static void draw_ridspoof_icon(lv_obj_t *tile) {
    lv_color_t bl = lv_color_make(0x33, 0xcc, 0xff);
    icon_dot(tile, 18, bl, 0, 52);                // body
    icon_dot(tile, 14, bl, -28, 40);              // rotor L
    icon_dot(tile, 14, bl,  28, 40);              // rotor R
    icon_dot(tile, 10, lv_color_make(0xff,0x66,0x00), 0, 28);  // broadcast/spoof pip
}

// SubGHz Sentinel — a shield outline with an inner dot.
static void draw_sentinel_icon(lv_obj_t *tile) {
    lv_color_t gr = lv_color_make(0x00, 0xff, 0xaa);
    lv_obj_t *sh = icon_rect(tile, 46, 54, 14, gr, 0, 30);  // shield body
    lv_obj_set_style_bg_opa(sh, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(sh, gr, LV_PART_MAIN);
    lv_obj_set_style_border_width(sh, 5, LV_PART_MAIN);
    icon_dot(tile, 14, gr, 0, 50);                // core
}

// Skimmer detector icon — a credit card on its side with a thin magnetic
// stripe and a small red warning chip in the corner, hinting at "card +
// compromise". Reads at-a-glance as "card reader / skimmer".
static void draw_skimmer_icon(lv_obj_t *tile)
{
    // Card body — wide rounded rectangle in a neutral plastic colour.
    lv_obj_t *card = lv_obj_create(tile);
    lv_obj_set_size(card, 116, 74);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_make(0xDD, 0xDD, 0xDD), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 36);   // y=36..110

    // Magnetic stripe — the long black band across the upper portion.
    lv_obj_t *stripe = lv_obj_create(tile);
    lv_obj_set_size(stripe, 116, 14);
    lv_obj_set_style_radius(stripe, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(stripe, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(stripe, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(stripe, 0, LV_PART_MAIN);
    lv_obj_clear_flag(stripe, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(stripe, LV_ALIGN_TOP_MID, 0, 48);

    // EMV chip — small gold square in the lower-left quadrant of the card.
    lv_obj_t *chip = lv_obj_create(tile);
    lv_obj_set_size(chip, 18, 14);
    lv_obj_set_style_radius(chip, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip, lv_color_make(0xD4, 0xAF, 0x37), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip, lv_color_make(0x88, 0x66, 0x11), LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(chip, LV_ALIGN_TOP_LEFT, 44, 74);

    // Two short digit-stripe placeholders on the card face to suggest
    // embossed numbers without trying to render real digits.
    for (int row = 0; row < 2; row++) {
        lv_obj_t *digits = lv_obj_create(tile);
        lv_obj_set_size(digits, 44, 3);
        lv_obj_set_style_radius(digits, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(digits, lv_color_make(0x99, 0x99, 0x99), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(digits, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(digits, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(digits, 0, LV_PART_MAIN);
        lv_obj_clear_flag(digits, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(digits, LV_ALIGN_TOP_MID, 16, 92 + row * 6);
    }

    // Warning badge in the upper-right — red circle with a "!" so the icon
    // reads as "compromise" rather than just "credit card".
    lv_obj_t *badge = lv_obj_create(tile);
    lv_obj_set_size(badge, 26, 26);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(badge, lv_color_make(0xCC, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(badge, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_border_width(badge, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(badge, 0, LV_PART_MAIN);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -8, 26);

    lv_obj_t *bang = lv_label_create(badge);
    lv_obj_set_style_text_font(bang, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(bang, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(bang, "!");
    lv_obj_center(bang);
}

// Evil Twin -- two overlapping WiFi wedge shapes, the second one in red to
// signal "impostor".  The legitimate AP is drawn in white/grey at left-center;
// the rogue is drawn slightly offset and smaller in red at right, so at a
// glance the icon reads as "two APs claiming the same name".
static void draw_eviltwin_icon(lv_obj_t *tile)
{
    // Legitimate AP: three concentric arcs (large, medium, small) + dot,
    // stacked vertically, centre-left of the tile.
    lv_color_t legit  = lv_color_make(0xCC, 0xCC, 0xCC);
    lv_color_t rogue  = lv_color_make(0xDD, 0x22, 0x22);
    lv_color_t shared = lv_color_make(0xFF, 0xAA, 0x00);

    struct ArcDef { int x, y, w, h; lv_color_t col; };
    ArcDef arcs[] = {
        // legit AP arcs (left side)
        { -24, 28, 56, 28, legit },
        { -24, 42, 40, 20, legit },
        { -24, 56, 24, 12, legit },
        // rogue AP arcs (right side, red)
        {  12, 38, 56, 28, rogue },
        {  12, 52, 40, 20, rogue },
        {  12, 66, 24, 12, rogue },
    };
    for (auto &a : arcs) {
        lv_obj_t *arc = lv_obj_create(tile);
        lv_obj_set_size(arc, a.w, a.h);
        lv_obj_set_style_radius(arc, a.h / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(arc, a.col, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(arc, 0, LV_PART_MAIN);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(arc, LV_ALIGN_TOP_MID, a.x, a.y);
    }

    // Dot for legit AP
    lv_obj_t *d1 = lv_obj_create(tile);
    lv_obj_set_size(d1, 8, 8);
    lv_obj_set_style_radius(d1, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d1, legit, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d1, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(d1, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(d1, 0, LV_PART_MAIN);
    lv_obj_clear_flag(d1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(d1, LV_ALIGN_TOP_MID, -24, 68);

    // Dot for rogue AP
    lv_obj_t *d2 = lv_obj_create(tile);
    lv_obj_set_size(d2, 8, 8);
    lv_obj_set_style_radius(d2, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d2, rogue, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d2, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(d2, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(d2, 0, LV_PART_MAIN);
    lv_obj_clear_flag(d2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(d2, LV_ALIGN_TOP_MID, 12, 78);

    // Small "=" badge between the two APs to suggest "same name, different AP"
    for (int i = 0; i < 2; i++) {
        lv_obj_t *eq = lv_obj_create(tile);
        lv_obj_set_size(eq, 12, 3);
        lv_obj_set_style_radius(eq, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(eq, shared, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(eq, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(eq, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(eq, 0, LV_PART_MAIN);
        lv_obj_clear_flag(eq, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(eq, LV_ALIGN_TOP_MID, -6, 56 + i * 7);
    }
}

// Flock -- camera silhouette (rectangular body + circular lens) to represent
// surveillance cameras and drones detected by OUI/name matching.
static void draw_flock_icon(lv_obj_t *tile)
{
    lv_color_t cam_body  = lv_color_make(0x33, 0x33, 0x33);
    lv_color_t cam_edge  = lv_color_make(0x66, 0x66, 0x66);
    lv_color_t lens_ring = lv_color_make(0x88, 0x88, 0x88);
    lv_color_t lens_fill = lv_color_make(0x11, 0x44, 0x88);
    lv_color_t lens_glint= lv_color_make(0xAA, 0xCC, 0xFF);
    lv_color_t alert_red = lv_color_make(0xCC, 0x22, 0x22);

    // Camera body
    lv_obj_t *body = lv_obj_create(tile);
    lv_obj_set_size(body, 88, 56);
    lv_obj_set_style_radius(body, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(body, cam_body, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, cam_edge, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 30);

    // Lens ring
    lv_obj_t *lring = lv_obj_create(tile);
    lv_obj_set_size(lring, 36, 36);
    lv_obj_set_style_radius(lring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lring, lens_ring, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lring, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(lring, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lring, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lring, LV_ALIGN_TOP_MID, 0, 40);

    // Lens fill
    lv_obj_t *lfill = lv_obj_create(tile);
    lv_obj_set_size(lfill, 26, 26);
    lv_obj_set_style_radius(lfill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lfill, lens_fill, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lfill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(lfill, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lfill, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lfill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lfill, LV_ALIGN_TOP_MID, 0, 45);

    // Lens glint
    lv_obj_t *glint = lv_obj_create(tile);
    lv_obj_set_size(glint, 7, 7);
    lv_obj_set_style_radius(glint, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(glint, lens_glint, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(glint, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(glint, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(glint, 0, LV_PART_MAIN);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(glint, LV_ALIGN_TOP_MID, -6, 48);

    // Small mount stub on top of the body
    lv_obj_t *mount = lv_obj_create(tile);
    lv_obj_set_size(mount, 18, 10);
    lv_obj_set_style_radius(mount, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mount, cam_body, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mount, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(mount, cam_edge, LV_PART_MAIN);
    lv_obj_set_style_border_width(mount, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mount, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mount, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mount, LV_ALIGN_TOP_MID, 0, 21);

    // Red recording indicator dot (top-right of body)
    lv_obj_t *rec = lv_obj_create(tile);
    lv_obj_set_size(rec, 10, 10);
    lv_obj_set_style_radius(rec, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rec, alert_red, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rec, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(rec, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rec, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rec, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rec, LV_ALIGN_TOP_RIGHT, -24, 38);
}

// Lower-left: microSD card — rounded body with a chamfered top-left corner and
// a row of gold contact pins. For the USB Mass Storage card-reader tool.
// Lower-left: microSD card — slimmer body than a full-size SD card, eight
// gold contact pins (the microSD pin count), and a small notch carved out of
// the lower-left edge as the distinguishing microSD silhouette feature.
static void draw_microsd_icon(lv_obj_t *tile)
{
    // Card body — narrower and a touch darker than the previous SD-ish slab.
    lv_obj_t *card = lv_obj_create(tile);
    lv_obj_set_size(card, 60, 88);
    lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_make(0x2A, 0x3A, 0x55), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_make(0x7A, 0x90, 0xBA), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // 180-wide tile → card spans x=60..120; top edge at y=24
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 24);

    // Chamfered top-left corner — a tile-coloured square rotated 45° about
    // its own centre, positioned over the card's corner.
    lv_obj_t *chamfer = lv_obj_create(tile);
    lv_obj_set_size(chamfer, 30, 30);
    lv_obj_set_style_radius(chamfer, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chamfer, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chamfer, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(chamfer, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chamfer, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(chamfer, 15, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(chamfer, 15, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(chamfer, 450, LV_PART_MAIN);
    lv_obj_clear_flag(chamfer, LV_OBJ_FLAG_SCROLLABLE);
    // Centre the 30x30 square on the card's top-left corner (60, 24)
    lv_obj_align(chamfer, LV_ALIGN_TOP_LEFT, 45, 9);

    // Eight gold contact pins — the microSD card layout (vs the nine of an
    // SD card). Pins centred under the chamfer.
    for (int i = 0; i < 8; i++) {
        lv_obj_t *pin = lv_obj_create(tile);
        lv_obj_set_size(pin, 4, 18);
        lv_obj_set_style_radius(pin, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(pin, lv_color_make(0xD4, 0xAF, 0x37), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(pin, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(pin, 0, LV_PART_MAIN);
        lv_obj_clear_flag(pin, LV_OBJ_FLAG_SCROLLABLE);
        // Pin centres at offsets -21 .. +21 in steps of 6 from tile centre
        lv_obj_align(pin, LV_ALIGN_TOP_MID, -21 + i * 6, 50);
    }

    // The microSD-defining notch on the lower-left edge — tile-coloured
    // rectangle that overlaps the card border to carve a small chunk out.
    lv_obj_t *notch = lv_obj_create(tile);
    lv_obj_set_size(notch, 6, 8);
    lv_obj_set_style_radius(notch, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(notch, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(notch, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(notch, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(notch, 0, LV_PART_MAIN);
    lv_obj_clear_flag(notch, LV_OBJ_FLAG_SCROLLABLE);
    // Left edge of card is at x=60; place the notch straddling it so a few
    // px of card disappear (the rest of the notch sits over the tile bg).
    lv_obj_align(notch, LV_ALIGN_TOP_LEFT, 57, 88);
}

// Pager -- iconic yellow plastic body, wide green LCD across the top,
// and a control row below it: two rectangular keys each with a coloured
// LED indicator dot at the top (green and red), then a 4-way directional
// pad made of four separate rounded keys around a centre circle.
static void draw_pager_icon(lv_obj_t *tile)
{
    lv_color_t body_yellow = lv_color_make(0xFF, 0xCC, 0x33);
    lv_color_t body_shade  = lv_color_make(0xCC, 0x99, 0x22);
    lv_color_t btn_face    = lv_color_make(0x10, 0x10, 0x10);
    lv_color_t btn_edge    = lv_color_make(0x44, 0x44, 0x44);
    lv_color_t dpad_face   = lv_color_make(0x28, 0x28, 0x28);
    lv_color_t dpad_edge   = lv_color_make(0x55, 0x55, 0x55);

    // Yellow body -- body spans y=20..116 in tile coords
    lv_obj_t *body = lv_obj_create(tile);
    lv_obj_set_size(body, 116, 96);
    lv_obj_set_style_radius(body, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(body, body_yellow, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, body_shade, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 20);

    // Green LCD display
    lv_obj_t *lcd = lv_obj_create(tile);
    lv_obj_set_size(lcd, 96, 30);
    lv_obj_set_style_radius(lcd, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lcd, lv_color_make(0x00, 0xCC, 0x66), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lcd, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(lcd, lv_color_make(0x00, 0x77, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(lcd, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lcd, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lcd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lcd, LV_ALIGN_TOP_MID, 0, 28);

    // Two darker bands standing in for pager message text
    for (int row = 0; row < 2; row++) {
        lv_obj_t *line = lv_obj_create(tile);
        lv_obj_set_size(line, 72, 3);
        lv_obj_set_style_radius(line, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(line, lv_color_make(0x00, 0x55, 0x22), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(line, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(line, 0, LV_PART_MAIN);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 35 + row * 9);
    }

    // ---- Control row ----
    // Left side: green and red action buttons.  Each is an 18x18 rounded-rect
    // key with a small oval LED indicator pill near the top, matching the
    // physical Motorola Advisor indicator lights.

    // Green action button
    lv_obj_t *gbtn = lv_obj_create(tile);
    lv_obj_set_size(gbtn, 18, 18);
    lv_obj_set_style_radius(gbtn, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(gbtn, btn_face, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(gbtn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(gbtn, btn_edge, LV_PART_MAIN);
    lv_obj_set_style_border_width(gbtn, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(gbtn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(gbtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(gbtn, LV_ALIGN_TOP_MID, -38, 76);

    lv_obj_t *gled = lv_obj_create(gbtn);
    lv_obj_set_size(gled, 8, 5);
    lv_obj_set_style_radius(gled, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(gled, lv_color_make(0x22, 0xFF, 0x44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(gled, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(gled, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(gled, 0, LV_PART_MAIN);
    lv_obj_clear_flag(gled, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(gled, LV_ALIGN_TOP_MID, 0, 3);

    // Red action button
    lv_obj_t *rbtn = lv_obj_create(tile);
    lv_obj_set_size(rbtn, 18, 18);
    lv_obj_set_style_radius(rbtn, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rbtn, btn_face, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rbtn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(rbtn, btn_edge, LV_PART_MAIN);
    lv_obj_set_style_border_width(rbtn, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rbtn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rbtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rbtn, LV_ALIGN_TOP_MID, -18, 76);

    lv_obj_t *rled = lv_obj_create(rbtn);
    lv_obj_set_size(rled, 8, 5);
    lv_obj_set_style_radius(rled, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(rled, lv_color_make(0xFF, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rled, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(rled, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rled, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rled, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rled, LV_ALIGN_TOP_MID, 0, 3);

    // Right side: Advisor-style 4-way directional pad.
    // Four separate rounded keys with a 2 px gap between each arm and the
    // centre circle.  D-pad geometric centre sits at tile (+30, 87).
    //
    // Geometry (tile TOP_MID relative, y from tile top):
    //   up    align(+30, 73) size(12, 8)  bottom at 81
    //   ctr   align(+30, 83) size( 8, 8)  top 83, bot 91   <-- 2 px gaps
    //   down  align(+30, 93) size(12, 8)  top  at 93
    //   left  align(+20, 81) size( 8,12)  right at tile+24  <-- 2 px to ctr
    //   right align(+40, 81) size( 8,12)  left  at tile+36  <-- 2 px from ctr
    struct { int x, y, w, h; } dp_keys[4] = {
        { 30, 73, 12,  8 },
        { 30, 93, 12,  8 },
        { 20, 81,  8, 12 },
        { 40, 81,  8, 12 },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *key = lv_obj_create(tile);
        lv_obj_set_size(key, dp_keys[i].w, dp_keys[i].h);
        lv_obj_set_style_radius(key, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(key, dpad_face, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(key, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(key, dpad_edge, LV_PART_MAIN);
        lv_obj_set_style_border_width(key, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(key, 0, LV_PART_MAIN);
        lv_obj_clear_flag(key, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(key, LV_ALIGN_TOP_MID, dp_keys[i].x, dp_keys[i].y);
    }

    // Centre circle (OK / select key)
    lv_obj_t *dp_ctr = lv_obj_create(tile);
    lv_obj_set_size(dp_ctr, 8, 8);
    lv_obj_set_style_radius(dp_ctr, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dp_ctr, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dp_ctr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(dp_ctr, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    lv_obj_set_style_border_width(dp_ctr, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dp_ctr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(dp_ctr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(dp_ctr, LV_ALIGN_TOP_MID, 30, 83);
}

// Lower-right: TPMS — tire ring with a small valve stem
static void draw_tpms_icon(lv_obj_t *tile)
{
    // Tire (thick gray ring)
    lv_obj_t *tire = lv_obj_create(tile);
    lv_obj_set_size(tire, 84, 84);
    lv_obj_set_style_radius(tire, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tire, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(tire, lv_color_make(0xBB, 0xBB, 0xBB), LV_PART_MAIN);
    lv_obj_set_style_border_width(tire, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tire, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tire, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(tire, LV_ALIGN_TOP_MID, 0, 24);

    // Inner rim ring for contrast
    lv_obj_t *rim = lv_obj_create(tile);
    lv_obj_set_size(rim, 32, 32);
    lv_obj_set_style_radius(rim, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rim, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(rim, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    lv_obj_set_style_border_width(rim, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(rim, 0, LV_PART_MAIN);
    lv_obj_clear_flag(rim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(rim, LV_ALIGN_TOP_MID, 0, 50);

    // Valve stem protruding from the bottom of the tire
    lv_obj_t *valve = lv_obj_create(tile);
    lv_obj_set_size(valve, 6, 12);
    lv_obj_set_style_radius(valve, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(valve, lv_color_make(0xBB, 0xBB, 0xBB), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(valve, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(valve, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(valve, 0, LV_PART_MAIN);
    lv_obj_clear_flag(valve, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(valve, LV_ALIGN_TOP_MID, 0, 102);
}

// Lower-right: Mouse — rounded body with a button-divider line and scroll wheel
static void draw_mouse_icon(lv_obj_t *tile)
{
    // Mouse body — rounded, taller than wide
    lv_obj_t *body = lv_obj_create(tile);
    lv_obj_set_size(body, 62, 92);
    lv_obj_set_style_radius(body, 30, LV_PART_MAIN);
    lv_obj_set_style_bg_color(body, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 26);

    // Vertical divider separating the two top buttons
    lv_obj_t *divider = lv_obj_create(tile);
    lv_obj_set_size(divider, 2, 32);
    lv_obj_set_style_bg_color(divider, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(divider, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(divider, 0, LV_PART_MAIN);
    lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 30);

    // Scroll wheel
    lv_obj_t *wheel = lv_obj_create(tile);
    lv_obj_set_size(wheel, 8, 16);
    lv_obj_set_style_radius(wheel, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(wheel, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wheel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(wheel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(wheel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(wheel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(wheel, LV_ALIGN_TOP_MID, 0, 42);
}

// APRS — broadcast antenna with two radiating signal arcs
static void draw_aprs_icon(lv_obj_t *tile)
{
    // Signal arcs radiating up from the transmitter tip (tip centre at y=59)
    for (int i = 0; i < 2; i++) {
        lv_coord_t d = 40 + i * 30;
        lv_obj_t *wave = lv_arc_create(tile);
        lv_obj_set_size(wave, d, d);
        lv_obj_set_style_bg_opa(wave, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(wave, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(wave, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(wave, 0, LV_PART_MAIN);
        lv_obj_set_style_arc_color(wave, lv_color_make(0x00, 0xCC, 0x66), LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(wave, 4, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(wave, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_arc_set_bg_angles(wave, 0, 360);
        lv_arc_set_angles(wave, 210, 330);
        lv_obj_clear_flag(wave, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(wave, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(wave, LV_ALIGN_TOP_MID, 0, 59 - d / 2);
    }

    // Transmitter tip
    lv_obj_t *tip = lv_obj_create(tile);
    lv_obj_set_size(tip, 14, 14);
    lv_obj_set_style_radius(tip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tip, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(tip, LV_ALIGN_TOP_MID, 0, 52);

    // Antenna mast
    lv_obj_t *mast = lv_obj_create(tile);
    lv_obj_set_size(mast, 5, 52);
    lv_obj_set_style_radius(mast, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mast, lv_color_make(0xBB, 0xBB, 0xBB), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mast, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(mast, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mast, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mast, LV_ALIGN_TOP_MID, 0, 64);

    // Antenna base
    lv_obj_t *base = lv_obj_create(tile);
    lv_obj_set_size(base, 36, 5);
    lv_obj_set_style_radius(base, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(base, lv_color_make(0xBB, 0xBB, 0xBB), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(base, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(base, 0, LV_PART_MAIN);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(base, LV_ALIGN_TOP_MID, 0, 112);
}

// Tesla charge-port icon — stylized rear-quarter view of a real Tesla
// charge port: a rounded matte-black housing with the three connector
// prongs visible inside. A small red dot off to the side stands in for
// the port-status LED so the icon doesn't read as "generic outlet".
static void draw_credit_icon(lv_obj_t *tile)
{
    // Gettone/moneta viola (tema Frantic Fest): cerchio pieno + anello interno.
    lv_obj_t *coin = lv_obj_create(tile);
    lv_obj_set_size(coin, 96, 96);
    lv_obj_set_style_radius(coin, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(coin, lv_color_make(0x8A, 0x63, 0xD6), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(coin, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(coin, lv_color_make(0xB9, 0x9C, 0xF5), LV_PART_MAIN);
    lv_obj_set_style_border_width(coin, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(coin, 0, LV_PART_MAIN);
    lv_obj_clear_flag(coin, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(coin, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *inner = lv_obj_create(coin);
    lv_obj_set_size(inner, 58, 58);
    lv_obj_set_style_radius(inner, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(inner, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(inner, lv_color_make(0x2A, 0x1B, 0x45), LV_PART_MAIN);
    lv_obj_set_style_border_width(inner, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(inner, 0, LV_PART_MAIN);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(inner);
}

// Helper icone: rettangolo/cerchio pieno (filled) o solo bordo.
static lv_obj_t *ic_shape(lv_obj_t *par, int w, int h, int radius, bool filled,
                          lv_color_t col, lv_align_t al, int xo, int yo)
{
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, filled ? 0 : 4, LV_PART_MAIN);
    if (filled) {
        lv_obj_set_style_bg_color(o, col, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(o, col, LV_PART_MAIN);
    }
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(o, al, xo, yo);
    return o;
}

static void draw_adsb_icon(lv_obj_t *tile)  // aeroplano
{
    lv_color_t col = lv_color_make(0x37, 0x8A, 0xDD);
    ic_shape(tile, 16, 92, 8, true, col, LV_ALIGN_TOP_MID, 0, 42);   // fusoliera
    ic_shape(tile, 98, 16, 8, true, col, LV_ALIGN_TOP_MID, 0, 74);   // ali
    ic_shape(tile, 44, 12, 6, true, col, LV_ALIGN_TOP_MID, 0, 120);  // coda
}

static void draw_mic_icon(lv_obj_t *tile)   // microfono
{
    lv_color_t col = lv_color_make(0xE0, 0x5A, 0x5A);
    ic_shape(tile, 40, 66, 20, true, col, LV_ALIGN_TOP_MID, 0, 40);   // capsula
    ic_shape(tile, 10, 24, 4,  true, col, LV_ALIGN_TOP_MID, 0, 110);  // stelo
    ic_shape(tile, 56, 10, 5,  true, col, LV_ALIGN_TOP_MID, 0, 132);  // base
}

static void draw_panel_icon(lv_obj_t *tile) // finestra/dashboard
{
    lv_color_t col = lv_color_make(0x00, 0xCC, 0x66);
    lv_obj_t *win = ic_shape(tile, 104, 84, 10, false, col, LV_ALIGN_TOP_MID, 0, 46);
    ic_shape(win, 92, 20, 4, true, col, LV_ALIGN_TOP_MID,  0, 0);    // barra titolo
    ic_shape(win, 66, 8,  4, true, col, LV_ALIGN_TOP_LEFT, 4, 30);   // riga
    ic_shape(win, 44, 8,  4, true, col, LV_ALIGN_TOP_LEFT, 4, 46);   // riga
}

static void draw_tesla_cp_icon(lv_obj_t *tile)
{
    // Outer port housing — matte black with subtle bezel.
    lv_obj_t *port = lv_obj_create(tile);
    lv_obj_set_size(port, 120, 78);
    lv_obj_set_style_radius(port, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(port, lv_color_make(0x14, 0x14, 0x14), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(port, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(port, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_obj_set_style_border_width(port, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(port, 0, LV_PART_MAIN);
    lv_obj_clear_flag(port, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(port, LV_ALIGN_TOP_MID, 0, 38);

    // Three connector prongs, evenly spaced across the housing's middle.
    for (int i = -1; i <= 1; i++) {
        lv_obj_t *prong = lv_obj_create(port);
        lv_obj_set_size(prong, 12, 20);
        lv_obj_set_style_radius(prong, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(prong, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(prong, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(prong, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(prong, 0, LV_PART_MAIN);
        lv_obj_clear_flag(prong, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(prong, LV_ALIGN_CENTER, i * 24, -6);
    }

    // Tiny red status LED — the "is the port unlocked" tell on a real Tesla.
    // Makes the icon legible as a *Tesla* charge port rather than a power
    // socket, without resorting to the literal Tesla logo.
    lv_obj_t *led = lv_obj_create(port);
    lv_obj_set_size(led, 8, 8);
    lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(led, lv_color_make(0xFF, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(led, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(led, 0, LV_PART_MAIN);
    lv_obj_clear_flag(led, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(led, LV_ALIGN_BOTTOM_RIGHT, -10, -6);
}

// Unified Scanner — a mini results panel: a dark display holding three neon
// signal rows (the flagged devices) with a bright green scan line sweeping
// across. Reads as "live list of detections", not a cold-war radar scope.
static void draw_scanner_icon(lv_obj_t *tile)
{
    lv_obj_t *panel = lv_obj_create(tile);
    lv_obj_set_size(panel, 108, 96);
    lv_obj_set_style_radius(panel, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(panel, lv_color_make(0x08, 0x0A, 0x08), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_make(0x2c, 0xa0, 0x60), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 30);

    const lv_color_t rowc[3] = {
        lv_color_make(0xFF, 0x33, 0x55),   // AirTag red
        lv_color_make(0xFF, 0x8C, 0x1A),   // Flipper orange
        lv_color_make(0xB2, 0x66, 0xFF),   // Flock violet
    };
    const int roww[3] = { 74, 58, 66 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = lv_obj_create(panel);
        lv_obj_set_size(r, roww[i], 10);
        lv_obj_set_style_radius(r, 5, LV_PART_MAIN);
        lv_obj_set_style_bg_color(r, rowc[i], LV_PART_MAIN);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(r, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(r, LV_ALIGN_TOP_LEFT, 12, 16 + i * 22);
    }

    // Bright scan line sweeping down the panel.
    lv_obj_t *scan = lv_obj_create(panel);
    lv_obj_set_size(scan, 92, 3);
    lv_obj_set_style_radius(scan, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scan, lv_color_make(0x66, 0xFF, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scan, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(scan, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scan, 0, LV_PART_MAIN);
    lv_obj_clear_flag(scan, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(scan, LV_ALIGN_TOP_MID, 0, 48);
}

// Task manager — a battery outline with a kill "×" over it, amber. Reads as
// "stop things to save power".
static void draw_taskmgr_icon(lv_obj_t *tile)
{
    lv_color_t c = lv_color_make(0xFF, 0x9A, 0x33);
    lv_obj_t *body = lv_obj_create(tile);          // battery body
    lv_obj_set_size(body, 104, 60);
    lv_obj_set_style_radius(body, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(body, c, LV_PART_MAIN);
    lv_obj_set_style_border_width(body, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(body, 0, LV_PART_MAIN);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(body, LV_ALIGN_TOP_MID, -6, 56);
    lv_obj_t *nub = lv_obj_create(tile);           // + terminal
    lv_obj_set_size(nub, 10, 26);
    lv_obj_set_style_radius(nub, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(nub, c, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(nub, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(nub, 0, LV_PART_MAIN);
    lv_obj_clear_flag(nub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(nub, LV_ALIGN_TOP_MID, 52, 73);
    // kill "×" inside
    for (int i = 0; i < 2; i++) {
        lv_obj_t *bar = lv_obj_create(tile);
        lv_obj_set_size(bar, 44, 6);
        lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_make(0xFF, 0x44, 0x44), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(bar, i == 0 ? 450 : -450, LV_PART_MAIN);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(bar, LV_ALIGN_TOP_MID, -6, 83);
    }
}

// Meta / smart-glasses detector — a pair of glasses: two rounded lenses joined
// by a bridge, with short temples. Blue to read as "Meta".
static void draw_meta_icon(lv_obj_t *tile)
{
    lv_color_t c = lv_color_make(0x33, 0x88, 0xFF);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *lens = lv_obj_create(tile);
        lv_obj_set_size(lens, 52, 44);
        lv_obj_set_style_radius(lens, 14, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(lens, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(lens, c, LV_PART_MAIN);
        lv_obj_set_style_border_width(lens, 5, LV_PART_MAIN);
        lv_obj_set_style_pad_all(lens, 0, LV_PART_MAIN);
        lv_obj_clear_flag(lens, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(lens, LV_ALIGN_TOP_MID, i == 0 ? -32 : 32, 62);
    }
    lv_obj_t *br = lv_obj_create(tile);              // bridge
    lv_obj_set_size(br, 18, 5);
    lv_obj_set_style_bg_color(br, c, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(br, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(br, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(br, 0, LV_PART_MAIN);
    lv_obj_clear_flag(br, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(br, LV_ALIGN_TOP_MID, 0, 80);
    for (int i = 0; i < 2; i++) {                    // temples
        lv_obj_t *t = lv_obj_create(tile);
        lv_obj_set_size(t, 18, 5);
        lv_obj_set_style_bg_color(t, c, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(t, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(t, 0, LV_PART_MAIN);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(t, LV_ALIGN_TOP_MID, i == 0 ? -66 : 66, 66);
    }
}

// Threat Radar — concentric sweep rings with a single red blip, evoking a
// radar scope. Rings are transparent circles with a green border; the blip is
// a contact riding a ring, the spoke a faint sweep line.
static void draw_radar_icon(lv_obj_t *tile)
{
    lv_color_t green = lv_color_make(0x22, 0xDD, 0x66);
    const int rings[3] = { 96, 64, 32 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = lv_obj_create(tile);
        lv_obj_set_size(r, rings[i], rings[i]);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(r, green, LV_PART_MAIN);
        lv_obj_set_style_border_width(r, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 30 + (96 - rings[i]) / 2);
    }
    lv_obj_t *spoke = lv_obj_create(tile);
    lv_obj_set_size(spoke, 3, 48);
    lv_obj_set_style_radius(spoke, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(spoke, green, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(spoke, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(spoke, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(spoke, 0, LV_PART_MAIN);
    lv_obj_clear_flag(spoke, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(spoke, LV_ALIGN_TOP_MID, 18, 34);

    lv_obj_t *blip = lv_obj_create(tile);
    lv_obj_set_size(blip, 14, 14);
    lv_obj_set_style_radius(blip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(blip, lv_color_make(0xFF, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(blip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(blip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(blip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(blip, LV_ALIGN_TOP_MID, 28, 44);
}

// Pwnpet — a Tamagotchi-ish handheld with a little face (two eyes + a smile).
static void draw_pet_icon(lv_obj_t *tile)
{
    lv_color_t body = lv_color_make(0x1d, 0x6f, 0x42);
    lv_color_t face = lv_color_make(0x0a, 0x12, 0x0a);
    lv_color_t eye  = lv_color_make(0x33, 0xdd, 0x88);

    lv_obj_t *b = lv_obj_create(tile);
    lv_obj_set_size(b, 104, 116);
    lv_obj_set_style_radius(b, 26, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, body, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, lv_color_make(0x2c, 0xa0, 0x60), LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *f = lv_obj_create(tile);
    lv_obj_set_size(f, 76, 64);
    lv_obj_set_style_radius(f, 14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(f, face, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(f, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(f, 0, LV_PART_MAIN);
    lv_obj_clear_flag(f, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(f, LV_ALIGN_TOP_MID, 0, 44);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *e = lv_obj_create(tile);
        lv_obj_set_size(e, 12, 16);
        lv_obj_set_style_radius(e, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(e, eye, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(e, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(e, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(e, 0, LV_PART_MAIN);
        lv_obj_clear_flag(e, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(e, LV_ALIGN_TOP_MID, i == 0 ? -16 : 16, 58);
    }

    lv_obj_t *m = lv_obj_create(tile);
    lv_obj_set_size(m, 28, 5);
    lv_obj_set_style_radius(m, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(m, eye, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(m, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(m, 0, LV_PART_MAIN);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 84);
}

// Duress — a domino mask (go-dark / disguise) over a faint clock outline.
static void draw_duress_icon(lv_obj_t *tile)
{
    lv_obj_t *mask = lv_obj_create(tile);
    lv_obj_set_size(mask, 108, 48);
    lv_obj_set_style_radius(mask, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mask, lv_color_make(0x20, 0x22, 0x2a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(mask, lv_color_make(0x55, 0x5a, 0x66), LV_PART_MAIN);
    lv_obj_set_style_border_width(mask, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mask, 0, LV_PART_MAIN);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(mask, LV_ALIGN_TOP_MID, 0, 58);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *eye = lv_obj_create(tile);
        lv_obj_set_size(eye, 22, 22);
        lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(eye, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(eye, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(eye, 0, LV_PART_MAIN);
        lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(eye, LV_ALIGN_TOP_MID, i == 0 ? -24 : 24, 71);
    }

    lv_obj_t *clk = lv_obj_create(tile);
    lv_obj_set_size(clk, 40, 40);
    lv_obj_set_style_radius(clk, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(clk, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(clk, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    lv_obj_set_style_border_width(clk, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(clk, 0, LV_PART_MAIN);
    lv_obj_clear_flag(clk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(clk, LV_ALIGN_TOP_MID, 0, 122);
}

// Handshake capture — signal rings with a captured packet dropping out (orange).
static void draw_handshake_icon(lv_obj_t *tile)
{
    lv_color_t o = lv_color_make(0xff, 0x8c, 0x1a);
    const int d[3] = { 96, 66, 36 };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *a = lv_obj_create(tile);
        lv_obj_set_size(a, d[i], d[i]);
        lv_obj_set_style_radius(a, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_color(a, o, LV_PART_MAIN);
        lv_obj_set_style_border_width(a, 3, LV_PART_MAIN);
        lv_obj_set_style_pad_all(a, 0, LV_PART_MAIN);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(a, LV_ALIGN_TOP_MID, 0, 30 + (96 - d[i]) / 2);
    }
    lv_obj_t *pkt = lv_obj_create(tile);
    lv_obj_set_size(pkt, 22, 16);
    lv_obj_set_style_radius(pkt, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pkt, o, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pkt, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(pkt, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pkt, 0, LV_PART_MAIN);
    lv_obj_clear_flag(pkt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(pkt, LV_ALIGN_TOP_MID, 0, 124);
}

// Cameras -- camera silhouette (body + lens + mount) with a small green WiFi
// glyph, for the live surveillance-camera scanner (WiFi + BLE). Distinct from
// the Flock detector tile by the green "networked scan" glyph.
static void draw_camera_scan_icon(lv_obj_t *tile)
{
    lv_color_t body  = lv_color_make(0x33, 0x33, 0x33);
    lv_color_t edge  = lv_color_make(0x66, 0x66, 0x66);
    lv_color_t ring  = lv_color_make(0x88, 0x88, 0x88);
    lv_color_t lens  = lv_color_make(0x11, 0x66, 0x55);
    lv_color_t glint = lv_color_make(0xAA, 0xFF, 0xDD);

    lv_obj_t *b = lv_obj_create(tile);
    lv_obj_set_size(b, 88, 56);
    lv_obj_set_style_radius(b, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, body, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, edge, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 44);

    lv_obj_t *lr = lv_obj_create(tile);
    lv_obj_set_size(lr, 36, 36);
    lv_obj_set_style_radius(lr, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lr, ring, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(lr, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lr, LV_ALIGN_TOP_MID, 0, 54);

    lv_obj_t *lf = lv_obj_create(tile);
    lv_obj_set_size(lf, 26, 26);
    lv_obj_set_style_radius(lf, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(lf, lens, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lf, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(lf, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lf, 0, LV_PART_MAIN);
    lv_obj_clear_flag(lf, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lf, LV_ALIGN_TOP_MID, 0, 59);

    lv_obj_t *g = lv_obj_create(tile);
    lv_obj_set_size(g, 7, 7);
    lv_obj_set_style_radius(g, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g, glint, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g, 0, LV_PART_MAIN);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(g, LV_ALIGN_TOP_MID, -6, 62);

    lv_obj_t *m = lv_obj_create(tile);
    lv_obj_set_size(m, 18, 10);
    lv_obj_set_style_radius(m, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(m, body, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(m, edge, LV_PART_MAIN);
    lv_obj_set_style_border_width(m, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(m, 0, LV_PART_MAIN);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 35);

    lv_obj_t *w = lv_label_create(tile);
    lv_obj_set_style_text_color(w, lv_color_make(0x33, 0xDD, 0x88), LV_PART_MAIN);
    lv_obj_set_style_text_font(w, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(w, LV_SYMBOL_WIFI);
    lv_obj_align(w, LV_ALIGN_TOP_RIGHT, -16, 30);
}

// Deauth -- red WiFi glyph over a burst of red "packets", for the dedicated
// deauthentication transmitter.
static void draw_deauth_icon(lv_obj_t *tile)
{
    lv_color_t red = lv_color_make(0xE0, 0x33, 0x33);

    lv_obj_t *w = lv_label_create(tile);
    lv_obj_set_style_text_color(w, red, LV_PART_MAIN);
    lv_obj_set_style_text_font(w, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(w, LV_SYMBOL_WIFI);
    lv_obj_align(w, LV_ALIGN_TOP_MID, 0, 40);

    const int xs[5] = { -40, -20, 0, 20, 40 };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *p = lv_obj_create(tile);
        lv_obj_set_size(p, 14, 10);
        lv_obj_set_style_radius(p, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(p, red, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(p, (i == 2) ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_border_width(p, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(p, 0, LV_PART_MAIN);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(p, LV_ALIGN_TOP_MID, xs[i], 120);
    }
}

// Waterfall -- cascata di righe colorate (blu->rosso, palette ESP32-DIV) per
// l'Analizzatore Banda FFT (port identico del waterfall del Marauder C5).
static void draw_waterfall_icon(lv_obj_t *tile)
{
    static const uint32_t cols[7] = {
        0x1133AA, 0x2299CC, 0x22CC66, 0xCCCC22, 0xFF8822, 0xFF3322, 0xF0F0F0
    };
    for (int i = 0; i < 7; i++) {
        lv_obj_t *r = lv_obj_create(tile);
        lv_obj_set_size(r, 96, 12);
        lv_obj_set_style_radius(r, 2, LV_PART_MAIN);
        uint32_t c = cols[i];
        lv_obj_set_style_bg_color(r, lv_color_make((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(r, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(r, 0, LV_PART_MAIN);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 34 + i * 15);
    }
}

// ESP-NOW — two peer nodes joined by a link, in the same teal the ESP-NOW
// screen uses. Conveys "two devices talking directly" at a glance.
static void draw_espnow_icon(lv_obj_t *tile)
{
    lv_color_t teal = lv_color_make(0x33, 0xDD, 0xAA);
    int cy = 78;
    // Left + right nodes.
    for (int i = 0; i < 2; i++) {
        lv_obj_t *node = lv_obj_create(tile);
        lv_obj_set_size(node, 34, 34);
        lv_obj_set_style_radius(node, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(node, teal, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(node, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(node, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(node, 0, LV_PART_MAIN);
        lv_obj_clear_flag(node, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(node, LV_ALIGN_TOP_MID, i == 0 ? -46 : 46, cy);
    }
    // Link bar between them.
    lv_obj_t *link = lv_obj_create(tile);
    lv_obj_set_size(link, 58, 8);
    lv_obj_set_style_radius(link, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(link, teal, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(link, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(link, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(link, 0, LV_PART_MAIN);
    lv_obj_clear_flag(link, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(link, LV_ALIGN_TOP_MID, 0, cy + 13);
}

// Auto / find-my-car — folded map (green + blue/yellow roads) with a red pin,
// drawn as cheap boxes. A full-colour ARGB8888 image here froze the menu on
// scroll (the heavy blit tripped the task watchdog), so it's box-art like the
// other tiles.
static void draw_car_icon(lv_obj_t *tile)
{
    lv_obj_t *map = lv_obj_create(tile);
    lv_obj_set_size(map, 108, 66);
    lv_obj_set_style_radius(map, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(map, lv_color_make(0x35, 0xA8, 0x53), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(map, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(map, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(map, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(map, true, LV_PART_MAIN);
    lv_obj_clear_flag(map, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(map, LV_ALIGN_CENTER, 0, 22);

    lv_obj_t *blue = lv_obj_create(map);          // blue diagonal road (clipped to map)
    lv_obj_set_size(blue, 160, 22);
    lv_obj_set_style_radius(blue, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(blue, lv_color_make(0x1E, 0x88, 0xE5), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(blue, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(blue, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(blue, -300, LV_PART_MAIN);
    lv_obj_clear_flag(blue, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(blue, LV_ALIGN_CENTER, -6, 12);

    lv_obj_t *ylw = lv_obj_create(map);           // yellow diagonal road
    lv_obj_set_size(ylw, 160, 9);
    lv_obj_set_style_radius(ylw, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ylw, lv_color_make(0xF4, 0xC2, 0x0D), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ylw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(ylw, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(ylw, 300, LV_PART_MAIN);
    lv_obj_clear_flag(ylw, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(ylw, LV_ALIGN_CENTER, 6, -6);

    lv_obj_t *tail = lv_obj_create(tile);         // pin point (rotated square, behind head)
    lv_obj_set_size(tail, 16, 16);
    lv_obj_set_style_radius(tail, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tail, lv_color_make(0xEA, 0x43, 0x35), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tail, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(tail, 0, LV_PART_MAIN);
    lv_obj_set_style_transform_rotation(tail, 450, LV_PART_MAIN);
    lv_obj_clear_flag(tail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(tail, LV_ALIGN_TOP_MID, 0, 44);

    lv_obj_t *pin = lv_obj_create(tile);          // pin head (circle)
    lv_obj_set_size(pin, 34, 34);
    lv_obj_set_style_radius(pin, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pin, lv_color_make(0xEA, 0x43, 0x35), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(pin, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pin, 0, LV_PART_MAIN);
    lv_obj_clear_flag(pin, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(pin, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *hole = lv_obj_create(pin);          // white hole
    lv_obj_set_size(hole, 14, 14);
    lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hole, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hole, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(hole, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hole, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(hole);
}

void tools_screen_create()
{
    tools_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(tools_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(tools_screen, 0, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(tools_screen);
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xFF, 0x00), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(title, "TOOLS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // Two-column flex grid. ROW_WRAP gives us 2 tiles per row (since each
    // 180px tile + the 12px column gap exceeds half the 384px inner width),
    // and the container scrolls vertically when future tiles overflow.
    lv_obj_t *grid = lv_obj_create(tools_screen);
    s_tools_grid = grid;   // remembered so on_gesture can query the scroll edges
    lv_obj_set_size(grid, 400, 432);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_color(grid, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(grid, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(grid, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(grid, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_column(grid, 12, LV_PART_MAIN);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid,
        LV_FLEX_ALIGN_SPACE_EVENLY,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START);

    // Insertion order maps to row-major (grid wraps every 2 tiles):
    //   [WiFi]      [Analyze]
    //   [Mouse]     [USB SD]
    //   [Pager]     [TPMS]
    //   [LoRa APRS] [Tesla CP]
    //   [AirTag]    [Flipper]
    //   [Skimmers]  [Evil Twin]
    //   [Flock]
    // The timepiece tiles (Alarm / Stopwatch / Timer / Calendar) used to live
    // at the bottom of this grid; they moved to the TIME screen (swipe up
    // from the clock face).
    lv_obj_t *t_scanner = make_tile(grid, "Scanner");
    t_duress            = make_tile(grid, "Duress");
    lv_obj_t *t_wifi    = make_tile(grid, "WiFi");
    lv_obj_t *t_analyze = make_tile(grid, "Analyze");
    lv_obj_t *t_mouse   = make_tile(grid, "Mouse");
    lv_obj_t *t_usbsd   = make_tile(grid, "USB SD");
    lv_obj_t *t_pager   = make_tile(grid, "Pager");
    lv_obj_t *t_tpms    = make_tile(grid, "TPMS");
    lv_obj_t *t_aprs    = make_tile(grid, "LoRa APRS");
    lv_obj_t *t_tesla   = make_tile(grid, "Tesla CP");
    t_airtag            = make_tile(grid, "AirTag");
    t_skimmer           = make_tile(grid, "Skimmers");
    t_eviltwin          = make_tile(grid, "Evil Twin");
    t_flock             = make_tile(grid, "Flock");
    lv_obj_t *t_radar   = make_tile(grid, "Radar");
    lv_obj_t *t_meta    = make_tile(grid, "Meta");
    lv_obj_t *t_task    = make_tile(grid, "Task Mgr");
    lv_obj_t *t_pet     = make_tile(grid, "Pet");
    t_handshake         = make_tile(grid, "Pwn");
    lv_obj_t *t_cameras = make_tile(grid, "Cameras");
    lv_obj_t *t_deauth  = make_tile(grid, "Deauth");
    lv_obj_t *t_arp     = make_tile(grid, "ARP MitM");
    lv_obj_t *t_bleaudit = make_tile(grid, "BLE Audit");
    lv_obj_t *t_drone   = make_tile(grid, "Drone ID");
    lv_obj_t *t_gatt    = make_tile(grid, "GATT");
    lv_obj_t *t_phantom = make_tile(grid, "Phantom");
    lv_obj_t *t_rid     = make_tile(grid, "RID Spoof");
    lv_obj_t *t_sentinel= make_tile(grid, "Sentinel");
    lv_obj_t *t_printer = make_tile(grid, "Printer");
    lv_obj_t *t_calmati = make_tile(grid, "Calmati");
    lv_obj_t *t_download = make_tile(grid, "Download");
    lv_obj_t *t_mifare  = make_tile(grid, "Mifare");
    lv_obj_t *t_waterfall = make_tile(grid, "Waterfall");
    lv_obj_t *t_espnow  = make_tile(grid, "ESP-NOW");
    lv_obj_t *t_car     = make_tile(grid, "Auto");
    lv_obj_t *t_udda    = make_tile(grid, "UDDA");
    lv_obj_t *t_adsb    = make_tile(grid, "ADS-B");
    lv_obj_t *t_mic     = make_tile(grid, "Mic Rec");
    lv_obj_t *t_assist  = make_tile(grid, "Assist");
    lv_obj_t *t_panel   = make_tile(grid, "Panel");
    lv_obj_t *t_credito = make_tile(grid, "Credito");
    // WPA3 SAE Overflow — flood a WPA3-SAE AP with random SAE Commit frames
    // to DoS its authentication path. Injection reuses the same primitive as
    // the deauther; PMF (mandatory on WPA3) doesn't protect against this.
    lv_obj_t *t_sae     = make_tile(grid, "WPA3 SAE");

    draw_scanner_icon(t_scanner);
    draw_wifi_icon(t_wifi);
    draw_analyzer_icon(t_analyze);
    draw_mouse_icon(t_mouse);
    draw_microsd_icon(t_usbsd);
    draw_pager_icon(t_pager);
    draw_tpms_icon(t_tpms);
    draw_aprs_icon(t_aprs);
    draw_tesla_cp_icon(t_tesla);
    draw_airtag_icon(t_airtag);
    place_icon(t_drone,    &ic_drone);
    place_icon(t_gatt,     &ic_gatt);
    place_icon(t_phantom,  &ic_phantom);
    place_icon(t_rid,      &ic_rid);
    place_icon(t_sentinel, &ic_sentinel);
    place_icon(t_udda,     &ic_lora);      // swirl LoRa sulla tile UDDA
    place_icon(t_skimmer,  &ic_skimmer);
    place_icon(t_arp,      &ic_arp);
    place_icon(t_bleaudit, &ic_bleaudit);
    place_icon(t_mifare,   &ic_mifare);
    place_icon(t_printer,  &ic_printer);
    draw_eviltwin_icon(t_eviltwin);
    draw_flock_icon(t_flock);
    draw_radar_icon(t_radar);
    draw_meta_icon(t_meta);
    place_icon(t_task, &ic_task);
    draw_pet_icon(t_pet);
    place_icon(t_duress, &ic_duress);
    draw_handshake_icon(t_handshake);
    draw_camera_scan_icon(t_cameras);
    draw_deauth_icon(t_deauth);
    draw_waterfall_icon(t_waterfall);
    draw_espnow_icon(t_espnow);
    draw_car_icon(t_car);
    draw_credit_icon(t_credito);
    draw_adsb_icon(t_adsb);
    draw_mic_icon(t_mic);
    draw_panel_icon(t_panel);

    // Credito tile opens the Frantic Fest cashless-balance reader (NFC page 50).
    lv_obj_add_event_cb(t_credito, [](lv_event_t *) { nfc_credit_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Tesla CP tile opens the 315 MHz charge-port-open transmit screen.
    lv_obj_add_event_cb(t_tesla, [](lv_event_t *) { tesla_cp_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // AirTag tile toggles the BLE Find My sniffer and swaps to a dim green
    // background while running.
    lv_obj_add_event_cb(t_airtag, on_airtag_clicked, LV_EVENT_CLICKED, NULL);
    set_airtag_tile_running(airtag_is_running());

    // Skimmers tile toggles the HC-0x card-skimmer detector. Same green-
    // when-running affordance as AirTag.
    lv_obj_add_event_cb(t_skimmer, on_skimmer_clicked, LV_EVENT_CLICKED, NULL);
    set_skimmer_tile_running(skimmer_is_running());

    // Evil Twin tile toggles the rogue-AP detector (WiFi beacon scan).
    lv_obj_add_event_cb(t_eviltwin, [](lv_event_t *) { evil_twin_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Flock tile toggles the surveillance-vendor detector (WiFi + BLE scan).
    lv_obj_add_event_cb(t_flock, on_flock_clicked, LV_EVENT_CLICKED, NULL);
    set_flock_tile_running(flock_is_running());

    // Radar tile opens the Threat Radar spatio-temporal correlation screen.
    lv_obj_add_event_cb(t_radar, [](lv_event_t *) { threat_radar_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // ADS-B tile opens the live flight radar (WiFi fetch from adsb.fi, GPS-centred).
    lv_obj_add_event_cb(t_adsb, [](lv_event_t *) { adsb_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Mic Rec tile opens the microphone recorder (streams a WAV to /Recordings).
    lv_obj_add_event_cb(t_mic, [](lv_event_t *) { mic_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_assist, [](lv_event_t *) { voice_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Panel tile opens the web control-panel screen (toggles the SoftAP).
    lv_obj_add_event_cb(t_panel, [](lv_event_t *) { webpanel_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Calmati tile: remote control of the T-Dongle-C5 "Calmati" AP over HTTP.
    lv_obj_add_event_cb(t_calmati, [](lv_event_t *) { calmati_remote_screen_show(); }, LV_EVENT_CLICKED, NULL);
    {
        lv_obj_t *ic = lv_image_create(t_calmati);
        lv_image_set_src(ic, &calmati_icon);
        lv_obj_set_style_image_recolor(ic, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_image_recolor_opa(ic, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 20);
        lv_obj_clear_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    }

    // Download tile: reboot straight into USB download mode for easy reflashing.
    lv_obj_add_event_cb(t_download, [](lv_event_t *) { enter_download_mode(); }, LV_EVENT_CLICKED, NULL);

    lv_obj_add_event_cb(t_meta, [](lv_event_t *) { meta_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_task, [](lv_event_t *) { task_manager_show(); }, LV_EVENT_CLICKED, NULL);

    // Pet tile opens the pwnpet mascot (meets nearby Pwnagotchis, reacts to events).
    lv_obj_add_event_cb(t_pet, [](lv_event_t *) { pet_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Duress tile arms/disarms the shake-to-disguise mode (green when armed).
    lv_obj_add_event_cb(t_duress, on_duress_clicked, LV_EVENT_CLICKED, NULL);
    set_duress_tile_armed(stealth_armed());

    // Pwn tile arms/disarms passive handshake/PMKID capture (green when running).
    lv_obj_add_event_cb(t_handshake, on_handshake_clicked, LV_EVENT_CLICKED, NULL);
    set_handshake_tile_running(handshake_is_running());

    // TPMS tile opens the TPMS monitor screen.
    lv_obj_add_event_cb(t_tpms, [](lv_event_t *) { tpms_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Pager tile opens the POCSAG/FLEX decoder screen.
    lv_obj_add_event_cb(t_pager, [](lv_event_t *) { pager_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Mouse tile opens the Bluetooth HID mouse screen.
    lv_obj_add_event_cb(t_mouse, [](lv_event_t *) { mouse_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // USB SD tile opens the USB mass-storage card-reader screen.
    lv_obj_add_event_cb(t_usbsd, [](lv_event_t *) { usb_sd_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // APRS tile opens the LoRa APRS receive/transmit screen.
    lv_obj_add_event_cb(t_aprs, [](lv_event_t *) { aprs_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Auto tile opens the find-my-car screen (save spot → arrow + distance).
    lv_obj_add_event_cb(t_car, [](lv_event_t *) { car_screen_show(); }, LV_EVENT_CLICKED, NULL);
    // Tile "UDDA Tracker": un tap accende GPS + mesh e si fa rilevare da UDDA.
    lv_obj_add_event_cb(t_udda, [](lv_event_t *) {
        gps_screen_power_on();                   // GPS on -> acquisisce il fix
        // Accendi la radio LoRa con la STESSA sequenza della schermata LoRa
        // (l'unica provata) + aggiorna l'indicatore a schermo. Prima duplicavo
        // i passi qui e l'indicatore restava "off": ora chiamo la funzione vera.
        lora_screen_force_on();
        meshtastic_set_short_name("UDDA");       // nome corto riconoscibile
        meshtastic_set_announce(true, 30000);    // annuncia ogni 30s -> rilevato prima
        meshtastic_screen_show();                // mostra lo stato mesh come conferma
    }, LV_EVENT_CLICKED, NULL);

    // WiFi tile opens the site-survey + ping-sweep screen.
    lv_obj_add_event_cb(t_scanner, [](lv_event_t *) { scan_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_wifi, [](lv_event_t *) { wifi_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Analyze tile opens the WiFi channel utilisation visualisation.
    lv_obj_add_event_cb(t_analyze, [](lv_event_t *) { analyze_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Cameras tile opens the live surveillance-camera scanner (WiFi + BLE).
    lv_obj_add_event_cb(t_cameras, [](lv_event_t *) { camera_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Deauth tile opens the dedicated deauthentication transmitter.
    lv_obj_add_event_cb(t_deauth, [](lv_event_t *) { deauther_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // ARP MitM tile: join WiFi, map the /24, MitM one host or blackhole all.
    lv_obj_add_event_cb(t_arp, [](lv_event_t *) { arp_mitm_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // WPA3 SAE tile: scan → filter WPA3-SAE APs → flood the target with random
    // SAE Commit frames (mbedTLS P-256, per-frame spoofed source MAC).
    lv_obj_add_event_cb(t_sae, [](lv_event_t *) { wpa3_sae_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // BLE Audit tile: passive pairing/privacy auditor for nearby advertisers.
    lv_obj_add_event_cb(t_bleaudit, [](lv_event_t *) { ble_pair_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_drone,  [](lv_event_t *) { drone_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_gatt,   [](lv_event_t *) { gatt_explorer_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_phantom,  [](lv_event_t *) { phantom_flood_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_rid,      [](lv_event_t *) { rid_spoof_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_sentinel, [](lv_event_t *) { subghz_sentinel_screen_show(); }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(t_printer,  [](lv_event_t *) { printer_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Mifare tile: ISO14443A card reader/identifier (UID/SAK/ATQA/type).
    lv_obj_add_event_cb(t_mifare, [](lv_event_t *) { mifare_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // Waterfall tile opens the FFT band analyzer (identical to the Marauder C5).
    lv_obj_add_event_cb(t_waterfall, [](lv_event_t *) { waterfall_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // ESP-NOW tile opens the out-of-mesh device-to-device messaging screen.
    lv_obj_add_event_cb(t_espnow, [](lv_event_t *) { espnow_screen_show(); }, LV_EVENT_CLICKED, NULL);

    // lv_obj_create() makes objects LV_OBJ_FLAG_CLICKABLE by default, so the
    // icon shapes filling each tile's centre would otherwise become the touch
    // target and swallow the tap (the tile's CLICKED handler never runs). Make
    // every decorative descendant non-clickable so the input device targets the
    // tile itself no matter where inside it you press. Recursive in case an icon
    // nests its parts. (More robust than EVENT_BUBBLE: with the shapes taken out
    // of hit-testing, both the tap and the scroll grab land on the tile/grid.)
    for (uint32_t i = 0; i < lv_obj_get_child_count(grid); i++)
        tile_icons_passthrough(lv_obj_get_child(grid, i));

    lv_obj_add_event_cb(tools_screen, on_gesture, LV_EVENT_GESTURE, NULL);
}

void tools_screen_show()
{
    // Re-sync the switch-tile colours: the Scanner (and others) can stop these
    // detectors behind our back, so reflect the real running state on entry
    // instead of leaving a stale green tile.
    set_airtag_tile_running(airtag_is_running());
    set_skimmer_tile_running(skimmer_is_running());
    set_flock_tile_running(flock_is_running());

    main_loop_request_lvgl_priority(12);
    lv_scr_load(tools_screen);
}
bool tools_screen_is_active() { return lv_screen_active() == tools_screen; }
