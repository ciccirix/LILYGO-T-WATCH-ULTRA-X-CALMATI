#include "voice_screen.h"
#include "tools_screen.h"        // tools_screen_show() per il back-gesture
#include "mic_rec.h"             // registrazione WAV 16k mono su SD
#include "wifi_creds.h"          // credenziali WiFi (NVS / wifi.txt)
#include <Arduino.h>
#include <lvgl.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <stdlib.h>              // rand()
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Indirizzo di UDDA (il Raspberry). Prova prima mDNS "raspberrypi.local",
// poi ripiega su questo IP fisso (cambialo se il tuo Pi ha un altro IP).
#ifndef UDDA_IP
#define UDDA_IP "10.42.0.2"    // IP secondario del Pi (NON il gateway .1):
                               // lo stack lwIP dell'ESP32 non emette il SYN
                               // verso l'IP dell'AP a cui e' associato, ma
                               // verso un altro IP on-link si'.
#endif
#define UDDA_PORT   8082
#define REC_CAP_MS  8000        // stop automatico registrazione a 8s (~256KB)

static lv_obj_t  *s_scr, *s_status, *s_btn, *s_btnlbl, *s_you, *s_udda;
static lv_timer_t *s_tick = nullptr;

// --- voicebox stile KITT (Supercar): fila di LED rossi che pulsano col mic ---
// voicebox KITT: 3 colonne di segmenti rettangolari rossi (la centrale piu'
// alta), con bagliore (glow) sui segmenti accesi.
#define NCOL 3
static const uint8_t COL_MAX[NCOL] = {5, 7, 5};
static lv_obj_t *s_seg[NCOL][7];

static void kitt_set(int lvl)   // 0..100 -> riempie le 3 colonne dal basso
{
    if (lvl < 0) lvl = 0; if (lvl > 100) lvl = 100;
    for (int col = 0; col < NCOL; col++) {
        // piccola oscillazione indipendente per colonna: le 3 barre "ballano"
        // come il modulatore vocale di KITT quando c'e' voce.
        int j = lvl;
        if (lvl > 5) { j = lvl + (rand() % 31 - 15); if (j < 0) j = 0; if (j > 100) j = 100; }
        int n = (j * COL_MAX[col] + 50) / 100;      // segmenti accesi
        for (int s = 0; s < COL_MAX[col]; s++) {
            lv_obj_t *seg = s_seg[col][s];
            if (!seg) continue;
            bool on = s < n;
            lv_obj_set_style_bg_color(seg,
                on ? lv_color_make(0xff, 0x22, 0x00) : lv_color_make(0x1a, 0x02, 0x00),
                LV_PART_MAIN);
            lv_obj_set_style_shadow_width(seg, on ? 10 : 0, LV_PART_MAIN);  // glow
        }
    }
}

enum VState { V_IDLE, V_REC, V_SAVE, V_UPLOAD, V_DONE };
static VState s_state = V_IDLE;

static uint8_t      *s_wav = nullptr;    // buffer WAV in PSRAM
static size_t        s_wav_len = 0;
static volatile bool s_upl_done = false;
static char          s_you_txt[256];
static char          s_reply_txt[600];
static TaskHandle_t  s_task = nullptr;
static uint32_t      s_upl_start = 0;      // per il watchdog dell'upload

static void set_status(const char *s) { if (s_status) lv_label_set_text(s_status, s); }
static void ensure_wifi();

// --- risoluzione indirizzo UDDA (mDNS -> IP fisso) --------------------------
static void resolve_udda(char *host, size_t n)
{
    // L'hotspot UDDA ha il Pi sempre a 10.42.0.1. Fisso e affidabile
    // (gatewayIP() a volte torna 0/stale subito dopo lo switch di rete).
    snprintf(host, n, "%s", UDDA_IP);
}

// --- task di upload (background: l'HTTP dura ~30s, non blocca la UI) --------
static void upload_task(void *)
{
    // WiFi era spento (per il mic): ora e' stato riacceso da ensure_wifi.
    // Il cold-start del WiFi e' lento -> aspetta fino a ~25s e ritenta la
    // begin a meta' strada (la prima associazione da radio spenta puo' fallire).
    for (int i = 0; i < 250 && WiFi.status() != WL_CONNECTED; i++) {
        if (i == 100) {                 // ~10s: ritenta l'associazione a UDDA
            WiFi.mode(WIFI_STA);
            WiFi.begin("UDDA");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (WiFi.status() != WL_CONNECTED) {
        s_you_txt[0] = '\0';
        snprintf(s_reply_txt, sizeof(s_reply_txt), "WiFi UDDA non connesso. Riprova.");
        if (s_wav) { free(s_wav); s_wav = nullptr; }
        s_upl_done = true; s_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    // Associato != pronto: dopo il cold-start il DHCP puo' non aver ancora dato
    // l'IP. Se si fa la POST con localIP == 0.0.0.0 l'HTTP torna -1 e NON mette
    // nulla sul filo (nessun source address). Aspetta un lease valido (~12s).
    for (int i = 0; i < 120 && WiFi.localIP()[0] == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (WiFi.localIP()[0] == 0) {
        s_you_txt[0] = '\0';
        snprintf(s_reply_txt, sizeof(s_reply_txt), "UDDA: nessun IP (DHCP). Riprova.");
        if (s_wav) { free(s_wav); s_wav = nullptr; }
        s_upl_done = true; s_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    // Il power-save del WiFi (modem-sleep, attivo di default) fa cadere i primi
    // pacchetti dopo una riconnessione: il SYN del POST parte ma non viene mai
    // trasmesso e l'HTTP torna -1 dopo il connectTimeout. Va DISATTIVATO.
    WiFi.setSleep(false);
    vTaskDelay(pdMS_TO_TICKS(800));    // margine per l'ARP del gateway

    Serial.printf("[VOICE] connesso SSID=%s IP=%s GW=%s status=%d rssi=%d\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str(), WiFi.status(), WiFi.RSSI());

    char host[40]; resolve_udda(host, sizeof(host));

    // Sonda TCP grezza verso il servizio: apre e chiude una connessione a vuoto.
    // Sveglia il link e conferma la raggiungibilita' prima del POST da 180KB.
    for (int p = 1; p <= 5; p++) {
        WiFiClient probe;
        probe.setTimeout(4);
        bool up = probe.connect(host, UDDA_PORT, 4000);
        Serial.printf("[VOICE] sonda %d connect(%s:%d)=%d\n", p, host, UDDA_PORT, up);
        probe.stop();
        if (up) break;
        vTaskDelay(pdMS_TO_TICKS(700));
    }

    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d/api/voice?format=text", host, UDDA_PORT);
    Serial.printf("[VOICE] POST -> %s  (wav %u byte)\n", url, (unsigned)s_wav_len);
    s_you_txt[0] = '\0'; s_reply_txt[0] = '\0';

    bool ok = false;
    for (int attempt = 1; attempt <= 3 && !ok; attempt++) {
        WiFiClient cli;
        HTTPClient http;
        http.setConnectTimeout(8000);
        http.setTimeout(90000);
        http.setReuse(false);
        if (!http.begin(cli, url)) {
            Serial.printf("[VOICE] tent.%d http.begin FALLITO\n", attempt);
            snprintf(s_reply_txt, sizeof(s_reply_txt),
                     "Non contatto UDDA %s (tent.%d)", host, attempt);
            vTaskDelay(pdMS_TO_TICKS(1200));
            continue;
        }
        http.addHeader("Content-Type", "application/octet-stream");
        int code = http.POST(s_wav, s_wav_len);
        Serial.printf("[VOICE] tent.%d http.POST -> code=%d\n", attempt, code);
        if (code == 200) {
            String body = http.getString();
            int sep = body.indexOf("\n---\n");
            if (sep >= 0) {
                strlcpy(s_you_txt, body.substring(0, sep).c_str(), sizeof(s_you_txt));
                strlcpy(s_reply_txt, body.substring(sep + 5).c_str(), sizeof(s_reply_txt));
            } else {
                strlcpy(s_reply_txt, body.c_str(), sizeof(s_reply_txt));
            }
            ok = true;
        } else {
            snprintf(s_reply_txt, sizeof(s_reply_txt),
                     "Errore HTTP %d (tent.%d/3) UDDA %s tuo %s",
                     code, attempt, host, WiFi.localIP().toString().c_str());
            vTaskDelay(pdMS_TO_TICKS(1200));
        }
        http.end();
    }

    if (s_wav) { free(s_wav); s_wav = nullptr; }
    s_upl_done = true;
    s_task = nullptr;
    vTaskDelete(nullptr);
}

// Legge il WAV finalizzato dalla SD in un buffer PSRAM (contesto LVGL = unico
// a toccare la SD, come da design del firmware).
static bool load_wav()
{
    const char *p = mic_rec_last_path();
    if (!p || !p[0]) return false;
    File f = SD.open(p, FILE_READ);
    if (!f) return false;
    size_t sz = f.size();
    if (sz < 45 || sz > 2 * 1024 * 1024) { f.close(); return false; }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (uint8_t *)malloc(sz);
    if (!buf) { f.close(); return false; }
    size_t rd = f.read(buf, sz);
    f.close();
    if (rd != sz) { free(buf); return false; }
    s_wav = buf; s_wav_len = sz;
    return true;
}

// --- macchina a stati sul task LVGL -----------------------------------------
static void tick_cb(lv_timer_t *)
{
    switch (s_state) {
    case V_REC: {
        kitt_set(mic_rec_level());
        uint32_t el = mic_rec_elapsed_ms();
        char b[40]; snprintf(b, sizeof(b), "Registro... %lus", (unsigned long)(el / 1000));
        set_status(b);
        if (el >= REC_CAP_MS) {
            mic_rec_stop();
            s_state = V_SAVE;
            set_status("Salvo...");
            lv_label_set_text(s_btnlbl, "ATTENDI");
        }
        break;
    }
    case V_SAVE:
        if (!mic_rec_is_recording()) {           // file finalizzato
            kitt_set(0);
            if (mic_rec_bytes() == 0 || !load_wav()) {
                set_status(mic_rec_bytes() == 0 ? "Nessun audio" : "Errore lettura audio");
                lv_label_set_text(s_btnlbl, "PARLA");
                s_state = V_IDLE;
                break;
            }
            ensure_wifi();               // ora che il mic e' fermo, accendi il WiFi
            s_upl_done = false;
            s_upl_start = millis();
            s_state = V_UPLOAD;
            set_status("UDDA pensa... (~30s)");
            xTaskCreatePinnedToCore(upload_task, "voice_up", 16384, nullptr, 1, &s_task, 0);
        }
        break;
    case V_UPLOAD:
        if (s_upl_done) {
            if (s_you_txt[0]) {
                char b[300]; snprintf(b, sizeof(b), "Tu: %s", s_you_txt);
                lv_label_set_text(s_you, b);
            } else {
                lv_label_set_text(s_you, "");
            }
            lv_label_set_text(s_udda, s_reply_txt[0] ? s_reply_txt : "(nessuna risposta)");
            set_status("Pronto - swipe su per uscire");
            lv_label_set_text(s_btnlbl, "ANCORA");
            s_state = V_DONE;
        } else if (millis() - s_upl_start > 100000) {
            // watchdog: se dopo 100s non ha risposto, sblocca e fai riprovare
            lv_label_set_text(s_you, "");
            lv_label_set_text(s_udda, "Timeout: UDDA non risponde. Stessa rete WiFi?");
            set_status("Timeout - riprova");
            lv_label_set_text(s_btnlbl, "ANCORA");
            s_state = V_DONE;
        }
        break;
    default:
        break;
    }
}

// Legge la rete dell'hotspot da /wifi2.txt (riga1=SSID, riga2=pass, # commenti).
// Se il file manca, default = "UDDA" aperto.
static void read_wifi2(char *ssid, size_t sn, char *pass, size_t pn)
{
    snprintf(ssid, sn, "UDDA");
    pass[0] = '\0';
    if (!SD.exists("/wifi2.txt"))
        return;
    File f = SD.open("/wifi2.txt", FILE_READ);
    if (!f)
        return;
    int pos = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line[0] == '#')
            continue;
        if (pos == 0) { line.toCharArray(ssid, sn); pos = 1; }
        else if (pos == 1) { line.toCharArray(pass, pn); break; }
    }
    f.close();
}

static void ensure_wifi()
{
    // La voce vive su UDDA (l'assistente e' li'). Forziamo UDDA anche se il
    // watch e' su TIM/casa. Al reboot torna alla rete di /wifi.txt (casa).
    char ssid[40], pass[68];
    read_wifi2(ssid, sizeof(ssid), pass, sizeof(pass));
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == String(ssid)) {
        set_status("Su UDDA");
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);              // niente power-save: fa cadere il POST
    WiFi.disconnect();                 // lascia l'eventuale rete di casa
    if (pass[0]) WiFi.begin(ssid, pass);
    else         WiFi.begin(ssid);     // hotspot aperto
    set_status("Connetto a UDDA...");
}

static void on_btn(lv_event_t *)
{
    if (s_state == V_IDLE || s_state == V_DONE) {
        if (s_task) {                    // upload precedente ancora in corso
            set_status("Attendo la risposta precedente...");
            return;
        }
        // Il WiFi ATTIVO disturba il codec audio (mic muto). Quindi lo
        // SPEGNIAMO del tutto durante la registrazione; lo riaccendo dopo lo
        // stop, solo per l'upload. Cosi' il mic funziona sempre.
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        lv_label_set_text(s_you, "");
        lv_label_set_text(s_udda, "");
        mic_rec_start();
        if (mic_rec_is_recording()) {
            s_state = V_REC;
            lv_label_set_text(s_btnlbl, "STOP");
        } else {
            set_status(mic_rec_status_text());       // es. "no SD card"
        }
    } else if (s_state == V_REC) {
        mic_rec_stop();
        s_state = V_SAVE;
        set_status("Salvo...");
        lv_label_set_text(s_btnlbl, "ATTENDI");
    }
}

static void on_gesture(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_event_get_indev(e)) == LV_DIR_TOP) {
        // registrazione/upload proseguono in background se in corso.
        tools_screen_show();
    }
}

static void build()
{
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_scr, on_gesture, LV_EVENT_GESTURE, nullptr);
    lv_obj_set_flex_flow(s_scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_scr, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_scr, 8, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_obj_set_style_text_color(title, lv_color_make(0x00, 0xCC, 0x66), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(title, "Assistente UDDA");

    s_status = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_status, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_obj_set_width(s_status, lv_pct(92));
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_status, "Tocca PARLA e fai la domanda");

    // voicebox KITT: riquadro nero con 3 colonne di segmenti rossi rettangolari.
    lv_obj_t *kitt = lv_obj_create(s_scr);
    lv_obj_set_size(kitt, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_bg_color(kitt, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(kitt, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(kitt, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(kitt, lv_color_make(0x1a, 0x1a, 0x1a), LV_PART_MAIN);
    lv_obj_set_style_radius(kitt, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(kitt, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(kitt, 6, LV_PART_MAIN);
    lv_obj_clear_flag(kitt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(kitt, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(kitt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(kitt, 14, LV_PART_MAIN);   // spazio fra le 3 barre
    for (int col = 0; col < NCOL; col++) {
        lv_obj_t *bar = lv_obj_create(kitt);
        lv_obj_set_size(bar, 24, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(bar, 3, LV_PART_MAIN);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_COLUMN_REVERSE);   // riempie dal basso
        lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        for (int s = 0; s < COL_MAX[col]; s++) {
            lv_obj_t *seg = lv_obj_create(bar);
            lv_obj_set_size(seg, 24, 5);             // rettangolo largo e basso
            lv_obj_set_style_radius(seg, 1, LV_PART_MAIN);
            lv_obj_set_style_border_width(seg, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_color(seg, lv_color_make(0x1a, 0x02, 0x00), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_shadow_color(seg, lv_color_make(0xff, 0x30, 0x10), LV_PART_MAIN);
            lv_obj_set_style_shadow_width(seg, 0, LV_PART_MAIN);   // glow acceso da kitt_set
            lv_obj_set_style_shadow_spread(seg, 1, LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(seg, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
            s_seg[col][s] = seg;
        }
    }

    s_btn = lv_button_create(s_scr);
    lv_obj_set_size(s_btn, 170, 74);
    lv_obj_set_style_bg_color(s_btn, lv_color_make(0x00, 0x55, 0x2b), LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn, on_btn, LV_EVENT_CLICKED, nullptr);
    s_btnlbl = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_btnlbl, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_label_set_text(s_btnlbl, "PARLA");
    lv_obj_center(s_btnlbl);

    s_you = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_you, lv_color_make(0x33, 0xBB, 0xFF), LV_PART_MAIN);
    lv_obj_set_width(s_you, lv_pct(92));
    lv_label_set_long_mode(s_you, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_you, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_you, "");

    s_udda = lv_label_create(s_scr);
    lv_obj_set_style_text_color(s_udda, lv_color_make(0x00, 0xDD, 0x77), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_udda, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(s_udda, lv_pct(92));
    lv_label_set_long_mode(s_udda, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_udda, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(s_udda, "");
}

void voice_screen_show()
{
    if (!s_scr) build();
    // Non azzerare se c'e' un upload in corso (rientro durante l'attesa).
    if (s_state != V_UPLOAD && s_state != V_SAVE) {
        s_state = V_IDLE;
        lv_label_set_text(s_btnlbl, "PARLA");
        lv_label_set_text(s_you, "");
        lv_label_set_text(s_udda, "");
        set_status("Tocca PARLA e fai la domanda");
        kitt_set(0);
    }
    lv_scr_load(s_scr);
    if (!s_tick) s_tick = lv_timer_create(tick_cb, 100, nullptr);
}
