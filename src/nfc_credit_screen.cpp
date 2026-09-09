#include "nfc_credit_screen.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <time.h>

void tools_screen_show();                          // ritorno ai Tools
void clock_screen_get_local_time(struct tm *out);  // definita in main.cpp

// ============================================================================
//  FRANTIC FEST — verificatore PRE/POST (tile "Credito")
//  Tessera NTAG216-compat. Due slot: PRIMA e DOPO. Ogni cattura fa un dump
//  completo (231 pagine) via rfalT2TPollerRead, lo salva su SD e — quando si
//  cattura DOPO — mostra a schermo il DIFF (pagine cambiate old->new).
//  Serve a capire se un pasto/ricarica cambia il chip o è tutto server-side.
// ============================================================================

static lv_obj_t *cr_screen;
static lv_obj_t *cr_prima_btn, *cr_prima_lbl;
static lv_obj_t *cr_dopo_btn,  *cr_dopo_lbl;
static lv_obj_t *cr_status;
static lv_obj_t *cr_result;      // label dentro il pannello scrollabile

#define CR_VIOLET     lv_color_make(0x8A, 0x63, 0xD6)
#define CR_VIOLET_HI  lv_color_make(0xB9, 0x9C, 0xF5)
#define CR_VIOLET_DK  lv_color_make(0x2A, 0x1B, 0x45)
#define CR_BLUE       lv_color_make(0x37, 0x6A, 0xC8)
#define CR_GREEN      lv_color_make(0x2E, 0xA8, 0x5C)
#define CR_DIM        lv_color_make(0x55, 0x48, 0x70)

#define NPAGES 231

static bool     cr_nfc_powered = false;
enum CrState { CR_IDLE, CR_SCAN };
static CrState  cr_state  = CR_IDLE;
static bool     cr_ready  = false;
static int      cr_target = 0;                 // 0=PRIMA, 1=DOPO

// due slot in RAM: [slot][pagina*4 byte]
static uint8_t  s_slot[2][NPAGES * 4];
static bool     s_valid[2] = { false, false };
static char     s_uid[2][24];
static struct tm s_time[2];

static const char *slot_name(int s) { return s == 0 ? "PRIMA" : "DOPO"; }

// ---- RFAL ----
static void cr_on_notify(rfalNfcState st)
{
    if (st == RFAL_NFC_STATE_ACTIVATED) cr_ready = true;
}

static void cr_discover()
{
    rfalNfcDiscoverParam p;
    memset(&p, 0, sizeof(p));
    p.devLimit      = 1;
    p.techs2Find    = RFAL_NFC_POLL_TECH_A;
    p.GBLen         = RFAL_NFCDEP_GB_MAX_LEN;
    p.notifyCb      = cr_on_notify;
    p.totalDuration = 1000U;
    p.wakeupEnabled = false;
    NFCReader.rfalNfcDiscover(&p);
}

static void cr_nfc_on()
{
    if (cr_nfc_powered) return;
    instance.powerControl(POWER_NFC, true);
    instance.initNFC();
    cr_nfc_powered = true;
}

static void cr_stop_scan()
{
    if (cr_state == CR_SCAN) {
        NFCReader.rfalNfcDeactivate(false);
        cr_state = CR_IDLE;
    }
}

static void cr_set_scanning(bool on)
{
    lv_color_t pc = on ? lv_color_make(0xCC, 0x22, 0x22) : CR_BLUE;
    lv_color_t dc = on ? lv_color_make(0xCC, 0x22, 0x22) : CR_GREEN;
    lv_obj_set_style_bg_color(cr_prima_btn, pc, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cr_dopo_btn,  dc, LV_PART_MAIN);
    lv_label_set_text(cr_prima_lbl, on ? "STOP" : "PRIMA");
    lv_label_set_text(cr_dopo_lbl,  on ? "STOP" : "DOPO");
}

// Legge tutte le pagine nel buffer dello slot. Ritorna quante pagine lette.
static int cr_full_read(int slot)
{
    uint8_t  rx[32];
    uint16_t rcv;
    int      pages = 0;
    // 0..227 a blocchi di 4
    for (int p = 0; p <= 224; p += 4) {
        rcv = 0;
        if (NFCReader.rfalT2TPollerRead(p, rx, sizeof(rx), &rcv) != ST_ERR_NONE || rcv < 16)
            return pages;
        for (int i = 0; i < 4 && (p + i) < NPAGES; i++) {
            memcpy(&s_slot[slot][(p + i) * 4], rx + i * 4, 4);
            if (p + i + 1 > pages) pages = p + i + 1;
        }
    }
    // coda: leggendo pag.227 prendo 227..230 (l'ultima dell'NTAG216)
    rcv = 0;
    if (NFCReader.rfalT2TPollerRead(227, rx, sizeof(rx), &rcv) == ST_ERR_NONE && rcv >= 16) {
        for (int i = 0; i < 4 && (227 + i) < NPAGES; i++) {
            memcpy(&s_slot[slot][(227 + i) * 4], rx + i * 4, 4);
            if (227 + i + 1 > pages) pages = 227 + i + 1;
        }
    }
    return pages;
}

static bool page_nonzero(int slot, int pg)
{
    const uint8_t *b = &s_slot[slot][pg * 4];
    return b[0] || b[1] || b[2] || b[3];
}

// Scrive lo slot su SD (+ diff se DOPO). Ritorna true se salvato.
static bool cr_save_sd(int slot, const char *diff)
{
    if (!instance.isCardReady()) return false;
    if (!SD.exists("/frantic")) SD.mkdir("/frantic");
    File f = SD.open("/frantic/verify_log.txt", FILE_APPEND);
    if (!f) return false;
    struct tm *t = &s_time[slot];
    f.printf("\n=== %s  %04d-%02d-%02d %02d:%02d:%02d  UID=%s ===\n",
             slot_name(slot), t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec, s_uid[slot]);
    for (int pg = 4; pg < NPAGES; pg++) {
        if (!page_nonzero(slot, pg)) continue;
        const uint8_t *b = &s_slot[slot][pg * 4];
        f.printf("pag %3d: %02X %02X %02X %02X\n", pg, b[0], b[1], b[2], b[3]);
    }
    if (diff && diff[0]) f.print(diff);
    f.close();
    return true;
}

// Costruisce il testo diff PRIMA->DOPO in buf. Ritorna n. pagine cambiate.
static int cr_build_diff(char *buf, int bufsize)
{
    int n = 0, changed = 0;
    n += snprintf(buf + n, bufsize - n, "--- CAMBIATO (PRIMA->DOPO) ---\n");
    for (int pg = 0; pg < NPAGES && n < bufsize - 80; pg++) {
        const uint8_t *a = &s_slot[0][pg * 4];
        const uint8_t *b = &s_slot[1][pg * 4];
        if (memcmp(a, b, 4) == 0) continue;
        changed++;
        n += snprintf(buf + n, bufsize - n,
                      "pag %d: %02X%02X%02X%02X -> %02X%02X%02X%02X\n",
                      pg, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
    }
    if (changed == 0)
        snprintf(buf, bufsize, "--- NESSUN CAMBIAMENTO ---\n(chip identico: dato non sul chip)\n");
    return changed;
}

static void cr_process(int slot)
{
    int pages = cr_full_read(slot);
    if (pages < 52) {   // non siamo arrivati almeno a pag.51
        lv_label_set_text(cr_status, "Lettura incompleta, riprova");
        return;
    }
    clock_screen_get_local_time(&s_time[slot]);
    // UID dallo scan attivo
    rfalNfcDevice *dev;
    NFCReader.rfalNfcGetActiveDevice(&dev);
    int u = 0;
    for (int i = 0; i < dev->nfcidLen && u < (int)sizeof(s_uid[slot]) - 3; i++)
        u += snprintf(s_uid[slot] + u, sizeof(s_uid[slot]) - u, "%02X", dev->nfcid[i]);
    s_valid[slot] = true;

    const uint8_t *p50 = &s_slot[slot][50 * 4];
    const uint8_t *p51 = &s_slot[slot][51 * 4];

    char out[900];
    int n = 0;
    n += snprintf(out + n, sizeof(out) - n, "%s salvato  %02d:%02d:%02d\n",
                  slot_name(slot), s_time[slot].tm_hour, s_time[slot].tm_min, s_time[slot].tm_sec);
    n += snprintf(out + n, sizeof(out) - n, "UID %s\n", s_uid[slot]);
    n += snprintf(out + n, sizeof(out) - n, "50: %02X %02X %02X %02X\n", p50[0], p50[1], p50[2], p50[3]);
    n += snprintf(out + n, sizeof(out) - n, "51: %02X %02X %02X %02X\n", p51[0], p51[1], p51[2], p51[3]);

    char diff[500];
    diff[0] = '\0';
    if (slot == 1 && s_valid[0]) {              // DOPO con PRIMA disponibile
        if (strcmp(s_uid[0], s_uid[1]) != 0)
            n += snprintf(out + n, sizeof(out) - n, "!! UID PRIMA != DOPO (tessere diverse) !!\n");
        cr_build_diff(diff, sizeof(diff));
        n += snprintf(out + n, sizeof(out) - n, "%s", diff);
    }

    bool sd = cr_save_sd(slot, diff);
    n += snprintf(out + n, sizeof(out) - n, sd ? "[SD ok /frantic/verify_log.txt]"
                                              : "[SD non pronta: solo a schermo]");
    lv_label_set_text(cr_result, out);
    lv_label_set_text_fmt(cr_status, "%s letto (%d pag)", slot_name(slot), pages);
}

static void cr_start(int slot)
{
    cr_nfc_on();
    if (cr_state == CR_SCAN) { cr_stop_scan(); cr_set_scanning(false); lv_label_set_text(cr_status, ""); return; }
    cr_target = slot;
    cr_ready  = false;
    cr_state  = CR_SCAN;
    cr_set_scanning(true);
    lv_label_set_text_fmt(cr_status, "%s: avvicina la tessera", slot_name(slot));
    cr_discover();               // <-- avvia davvero la scansione RFAL
}

static void cr_on_prima(lv_event_t *e) { cr_start(0); }
static void cr_on_dopo (lv_event_t *e) { cr_start(1); }

static void cr_on_gesture(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) {
        cr_stop_scan();
        cr_set_scanning(false);
        tools_screen_show();
    }
}

static lv_obj_t *mk_btn(lv_obj_t *par, lv_color_t col, int xo, lv_obj_t **lbl_out, const char *txt)
{
    lv_obj_t *b = lv_obj_create(par);
    lv_obj_set_size(b, 172, 60);
    lv_obj_align(b, LV_ALIGN_TOP_MID, xo, 118);
    lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, col, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    *lbl_out = l;
    return b;
}

void nfc_credit_screen_create()
{
    cr_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(cr_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(cr_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cr_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cr_screen, cr_on_gesture, LV_EVENT_GESTURE, NULL);

    // Logo tondo piccolo
    lv_obj_t *logo = lv_obj_create(cr_screen);
    lv_obj_set_size(logo, 92, 92);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_radius(logo, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(logo, CR_VIOLET_DK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(logo, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(logo, CR_VIOLET_HI, LV_PART_MAIN);
    lv_obj_set_style_border_width(logo, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(logo, 0, LV_PART_MAIN);
    lv_obj_clear_flag(logo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *lt = lv_label_create(logo);
    lv_obj_set_style_text_align(lt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(lt, CR_VIOLET_HI, LV_PART_MAIN);
    lv_obj_set_style_text_font(lt, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(lt, "FRANTIC\nVERIFY");
    lv_obj_center(lt);

    // Due tasti PRIMA / DOPO
    cr_prima_btn = mk_btn(cr_screen, CR_BLUE,  -92, &cr_prima_lbl, "PRIMA");
    cr_dopo_btn  = mk_btn(cr_screen, CR_GREEN, +92, &cr_dopo_lbl,  "DOPO");
    lv_obj_add_event_cb(cr_prima_btn, cr_on_prima, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(cr_dopo_btn,  cr_on_dopo,  LV_EVENT_CLICKED, NULL);

    // Stato
    cr_status = lv_label_create(cr_screen);
    lv_obj_set_style_text_font(cr_status, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(cr_status, CR_VIOLET_HI, LV_PART_MAIN);
    lv_obj_set_style_text_align(cr_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(cr_status, 380);
    lv_label_set_long_mode(cr_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(cr_status, "PRIMA prima del pasto/ricarica, DOPO subito dopo");
    lv_obj_align(cr_status, LV_ALIGN_TOP_MID, 0, 188);

    // Pannello risultati scrollabile
    lv_obj_t *panel = lv_obj_create(cr_screen);
    lv_obj_set_size(panel, 396, 232);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 232);
    lv_obj_set_style_bg_color(panel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, CR_VIOLET_DK, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);

    cr_result = lv_label_create(panel);
    lv_obj_set_width(cr_result, lv_pct(100));
    lv_obj_set_style_text_color(cr_result, CR_VIOLET_HI, LV_PART_MAIN);
    lv_obj_set_style_text_font(cr_result, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_long_mode(cr_result, LV_LABEL_LONG_WRAP);
    lv_label_set_text(cr_result, "");

    lv_obj_t *hint = lv_label_create(cr_screen);
    lv_obj_set_style_text_color(hint, CR_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(hint, "Swipe su / Boot per tornare");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
}

void nfc_credit_screen_show()
{
    cr_nfc_powered = instance.pmu.isEnableDLDO1();
    cr_nfc_on();
    cr_stop_scan();
    cr_set_scanning(false);
    lv_scr_load(cr_screen);
}

bool nfc_credit_screen_is_active()
{
    return lv_screen_active() == cr_screen;
}

void nfc_credit_screen_worker()
{
    if (cr_state == CR_IDLE) return;
    if (!nfc_credit_screen_is_active()) return;

    NFCReader.rfalNfcWorker();

    if (cr_ready) {
        cr_ready = false;
        cr_process(cr_target);
        NFCReader.rfalNfcDeactivate(true);
        NFCReader.rfalNfcaPollerSleep();
        cr_state = CR_IDLE;
        cr_set_scanning(false);
    }
}
