#include "mic_rec.h"
#include "usb_sd.h"           // usb_sd_is_running()
#include <Arduino.h>
#include <LilyGoLib.h>
#include <SD.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <string.h>
#include <math.h>

// PDM mic is fixed at 16 kHz mono 16-bit (see LilyGoLib initMicrophone()).
#define MIC_RATE        16000
#define MIC_BITS        16
#define MIC_CHANS       1
#define MIC_BYTES_SEC   (MIC_RATE * (MIC_BITS / 8) * MIC_CHANS)   // 32000 B/s

#define REC_MAX_MS      (10 * 60 * 1000)   // hard cap: 10 min (~19 MB)
#define SB_SIZE         (64 * 1024)        // ~2 s of slack between reader/writer
#define MIC_CHUNK       2048               // bytes per mic read (~64 ms)
#define DRAIN_TMP       4096

static StreamBufferHandle_t s_sb   = nullptr;
static TaskHandle_t   s_task       = nullptr;
static lv_timer_t    *s_drain      = nullptr;
static File           s_file;

static volatile bool  s_run        = false;   // reader loop should keep going
static volatile bool  s_task_done  = false;   // reader task has exited
static volatile bool  s_recording  = false;   // public state (until finalised)
static uint32_t       s_start_ms   = 0;
static size_t         s_bytes      = 0;
static int            s_level      = 0;
static char           s_path[48]   = {0};
static const char    *s_status     = "idle";

// ---- WAV header ------------------------------------------------------------

static void put_u32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put_u16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }

// Build a 44-byte canonical PCM WAV header for `data_len` bytes of audio.
static void wav_header(uint8_t *h, uint32_t data_len)
{
    memcpy(h, "RIFF", 4);
    put_u32(h + 4, 36 + data_len);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_u32(h + 16, 16);                 // PCM fmt chunk size
    put_u16(h + 20, 1);                  // audioFormat = PCM
    put_u16(h + 22, MIC_CHANS);
    put_u32(h + 24, MIC_RATE);
    put_u32(h + 28, MIC_BYTES_SEC);      // byteRate
    put_u16(h + 32, MIC_CHANS * (MIC_BITS / 8));  // blockAlign
    put_u16(h + 34, MIC_BITS);
    memcpy(h + 36, "data", 4);
    put_u32(h + 40, data_len);
}

// ---- filename --------------------------------------------------------------

// /Recordings/rec_000.wav, incrementing past whatever already exists.
static void next_path(char *out, size_t sz)
{
    if (!SD.exists("/Recordings")) SD.mkdir("/Recordings");
    for (int i = 0; i < 1000; i++) {
        snprintf(out, sz, "/Recordings/rec_%03d.wav", i);
        if (!SD.exists(out)) return;
    }
    snprintf(out, sz, "/Recordings/rec_ovf.wav");   // 1000 files: reuse last
}

// ---- reader task (core 0) --------------------------------------------------

static void mic_task(void *)
{
    uint8_t buf[MIC_CHUNK];
    while (s_run) {
        size_t n = instance.mic.readBytes((char *)buf, sizeof(buf));  // blocks
        if (n && s_sb) xStreamBufferSend(s_sb, buf, n, portMAX_DELAY);
        if (millis() - s_start_ms >= REC_MAX_MS) s_run = false;       // hard cap
    }
    s_task_done = true;
    s_task = nullptr;
    vTaskDelete(nullptr);
}

// ---- drain + level (main/LVGL task) ----------------------------------------

static void update_level(const uint8_t *p, size_t n)
{
    const int16_t *s = (const int16_t *)p;
    size_t cnt = n / 2;
    int peak = 0;
    for (size_t i = 0; i < cnt; i++) {
        int v = s[i] < 0 ? -s[i] : s[i];
        if (v > peak) peak = v;
    }
    // Scala logaritmica (dB), come i VU-meter veri: la voce a volume normale
    // fa picchi a ~3000-8000 su 32767 (= -20/-12 dBFS), che in lineare erano
    // solo 10-24 su 100. In dB riempiono bene le barre e reagiscono alla voce.
    // Mappa la finestra utile [-52 dBFS (rumore/voce lontana) .. -8 dBFS (voce
    // forte)] su 0..100.
    int lvl;
    if (peak < 1) {
        lvl = 0;
    } else {
        float db = 20.0f * log10f((float)peak / 32767.0f);   // -90..0 dBFS
        const float floor_db = -52.0f, ceil_db = -8.0f;
        lvl = (int)((db - floor_db) / (ceil_db - floor_db) * 100.0f);
        if (lvl < 0)   lvl = 0;
        if (lvl > 100) lvl = 100;
    }
    if (lvl > s_level) s_level = lvl;          // attack: salto immediato su
    else               s_level -= 6;           // release: decadimento morbido
    if (s_level < 0) s_level = 0;
}

static void finalize()
{
    // Patch the header with the real sizes, then close.
    if (s_file) {
        uint8_t h[44];
        wav_header(h, (uint32_t)s_bytes);
        s_file.seek(0);
        s_file.write(h, sizeof(h));
        s_file.close();
    }
    if (s_sb) { vStreamBufferDelete(s_sb); s_sb = nullptr; }
    s_recording = false;
    s_status = s_bytes ? "saved" : "no audio";
    s_level = 0;
    if (s_drain) { lv_timer_del(s_drain); s_drain = nullptr; }
}

static void drain_cb(lv_timer_t *)
{
    if (!s_sb) return;
    uint8_t tmp[DRAIN_TMP];
    for (;;) {
        size_t n = xStreamBufferReceive(s_sb, tmp, sizeof(tmp), 0);
        if (n == 0) break;
        update_level(tmp, n);
        if (s_file) s_file.write(tmp, n);
        s_bytes += n;
    }
    // Finalise once the reader has stopped and the buffer is fully drained.
    if (!s_run && s_task_done && xStreamBufferBytesAvailable(s_sb) == 0)
        finalize();
}

// ---- public API ------------------------------------------------------------

void mic_rec_start()
{
    if (s_recording) return;

    if (!instance.isCardReady() || usb_sd_is_running()) {
        s_status = "no SD card";
        return;
    }

    next_path(s_path, sizeof(s_path));
    s_file = SD.open(s_path, FILE_WRITE);      // truncate/create
    if (!s_file) { s_status = "SD open failed"; s_path[0] = '\0'; return; }

    uint8_t h[44];
    wav_header(h, 0);                          // placeholder; patched on stop
    s_file.write(h, sizeof(h));

    s_sb = xStreamBufferCreate(SB_SIZE, 1);
    if (!s_sb) { s_file.close(); s_status = "out of memory"; return; }

    s_bytes     = 0;
    s_level     = 0;
    s_start_ms  = millis();
    s_task_done = false;
    s_run       = true;
    s_recording = true;
    s_status    = "recording";

    s_drain = lv_timer_create(drain_cb, 40, nullptr);
    xTaskCreatePinnedToCore(mic_task, "micrec", 4096, nullptr, 3, &s_task, 0);
}

void mic_rec_stop()
{
    if (!s_recording) return;
    s_run = false;         // reader exits; drain_cb finalises the file
    s_status = "saving";
}

void mic_rec_toggle()
{
    if (s_recording) mic_rec_stop();
    else             mic_rec_start();
}

bool     mic_rec_is_recording() { return s_recording; }
uint32_t mic_rec_elapsed_ms()   { return s_recording ? (millis() - s_start_ms) : 0; }
size_t   mic_rec_bytes()        { return s_bytes; }
int      mic_rec_level()        { return s_level; }
const char *mic_rec_last_path() { return s_path; }
const char *mic_rec_status_text(){ return s_status; }
