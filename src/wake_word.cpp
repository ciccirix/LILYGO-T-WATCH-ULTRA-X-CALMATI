#include "wake_word.h"
#include <LilyGoLib.h>
#include <Arduino.h>
#include <string.h>
#include <stdio.h>

// ESP-SR public headers (vendored in lib/esp-sr/).
extern "C" {
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
}

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_bt.h"
#include "esp_bt_main.h"

// libesp_audio_front_end.a calls check_chip_config() at AFE create time to
// warn if CPU/PSRAM/cache aren't set to the recommended values. It's defined
// in esp-sr's esp_process_sdkconfig.c — a file we don't compile because of
// its ~200 CONFIG_CN_SPEECH_COMMAND_IDx macros. It's a no-op if CONFIG_*
// aren't set, so shim it here. Kept even after we dropped AFE, in case some
// other .a file drags it in as a side effect during link.
extern "C" void check_chip_config(void) {}

// ---- module state ---------------------------------------------------------
//
// This engine skips the AFE (Audio Front-End) and drives the WakeNet iface
// directly. That's fine for pure wake-word detection with a single mic:
// AFE adds AEC / VAD / NS / AGC which we don't need, at a cost of ~35 KB
// contiguous internal SRAM — memory this fork can't reliably deliver at
// runtime once LVGL and the wardriver stacks are up. The direct-detect
// path fits in ~15 KB and works with whatever the mic hands us.

static volatile bool     s_active    = false;
static volatile bool     s_stop_req  = false;
static volatile int      s_energy    = 0;
static volatile bool     s_wake_flag = false;
static const char *      s_status    = "";

static srmodel_list_t         *s_models      = nullptr;
static model_iface_data_t     *s_wn_data     = nullptr;
static const esp_wn_iface_t   *s_wn_iface    = nullptr;
static TaskHandle_t            s_task        = nullptr;

static const char *SR_PARTITION = "model";

// ---- audio task -----------------------------------------------------------

static volatile int s_dbg_phase = 0;   // where in the loop we are (for status)
static volatile uint32_t s_dbg_chunks = 0;  // audio chunks processed

static void audio_task(void *)
{
    const int chunk_samples = s_wn_iface->get_samp_chunksize(s_wn_data);
    const int chunk_bytes   = chunk_samples * sizeof(int16_t);

    int16_t *buf = (int16_t *)heap_caps_malloc(chunk_bytes,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        s_status = "feed buf alloc failed";
        s_active = false;
        vTaskDelete(NULL);
        return;
    }

    s_status = "listening";
    s_active = true;

    while (!s_stop_req) {
        // Deliberate yield up top so LVGL on core 1 always gets a slice
        // between our WakeNet chunks. Without this the heap free-block
        // count on the UI side climbs (redraw dirty regions accumulate
        // faster than they're serviced) until the watch feels frozen even
        // though we never actually block anything.
        vTaskDelay(pdMS_TO_TICKS(5));

        s_dbg_phase = 1;
        size_t got = instance.mic.readBytes((char *)buf, chunk_bytes);
        if (got < (size_t)chunk_bytes) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        s_dbg_phase = 2;
        // Cheap RMS-ish energy proxy for the KITT bars.
        uint32_t acc = 0;
        for (int i = 0; i < chunk_samples; i++) {
            int v = buf[i];
            if (v < 0) v = -v;
            acc += (uint32_t)v;
        }
        int avg = chunk_samples ? (int)(acc / chunk_samples) : 0;
        int lvl = (avg * 100) / 4096;
        if (lvl > 100) lvl = 100;
        s_energy = lvl;

        s_dbg_phase = 3;
        wakenet_state_t r = s_wn_iface->detect(s_wn_data, buf);
        s_dbg_chunks++;
        s_dbg_phase = 4;

        if (r == WAKENET_DETECTED) {
            s_wake_flag = true;
            s_status = "wake!";
        }
    }

    // Clean exit
    if (buf) heap_caps_free(buf);
    if (s_wn_iface && s_wn_data) s_wn_iface->destroy(s_wn_data);
    s_wn_data  = nullptr;
    s_wn_iface = nullptr;
    if (s_models) esp_srmodel_deinit(s_models);
    s_models = nullptr;
    s_energy = 0;
    s_active = false;
    s_task   = nullptr;
    vTaskDelete(NULL);
}

// ---- public API -----------------------------------------------------------

bool wake_word_start()
{
    if (s_active) return true;

    // WakeNet on this fork lives in ~15 KB internal + ~200 KB PSRAM at load
    // time. The fork's baseline runtime has ~40 KB free SRAM but only ~25 KB
    // in a single contiguous block — enough for WakeNet's peak alloc during
    // model init if we free radios first, since the WiFi/BT drivers hoard
    // internal SRAM even when idle. Same guard as ble_scan_manager, in
    // reverse (see [[twatch-threat-radar]]).
    s_status = "freeing radios";
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    esp_wifi_deinit();
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_disable();
    }
    esp_bt_controller_deinit();
    vTaskDelay(pdMS_TO_TICKS(120));

    s_status    = "loading models";
    s_wake_flag = false;
    s_energy    = 0;

    s_models = esp_srmodel_init(SR_PARTITION);
    if (!s_models) {
        s_status = "partition 'model' missing";
        return false;
    }

    char *wn_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    if (!wn_name) {
        s_status = "no wakenet model in partition";
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    s_wn_iface = esp_wn_handle_from_name(wn_name);
    if (!s_wn_iface) {
        s_status = "wn iface not found";
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        return false;
    }

    // Snapshot heap right before the big alloc so the failure path can tell
    // the user how much contiguous SRAM was available at the moment.
    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t heap_big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    s_wn_data = s_wn_iface->create(wn_name, DET_MODE_90);
    if (!s_wn_data) {
        static char err_buf[64];
        snprintf(err_buf, sizeof(err_buf),
                 "WN alloc fail (free %uK max %uK)",
                 (unsigned)(heap_free / 1024), (unsigned)(heap_big / 1024));
        s_status = err_buf;
        esp_srmodel_deinit(s_models);
        s_models   = nullptr;
        s_wn_iface = nullptr;
        return false;
    }

    s_stop_req = false;
    // Pin to core 0 with the lowest above-idle priority. LVGL runs on core
    // 1 at priority 5; keeping this at 1 means we never race it for the
    // scheduler even during the CPU-heavy WakeNet detect() calls. Real-
    // world cost: audio processing has plenty of headroom (16 kHz × 512
    // samples = ~32 ms per chunk, of which detect() eats ~2-4 ms).
    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "wake_word", 4096,
                                            NULL, 1, &s_task, 0);
    if (ok != pdPASS) {
        s_status = "task spawn failed";
        s_wn_iface->destroy(s_wn_data);
        s_wn_data  = nullptr;
        esp_srmodel_deinit(s_models);
        s_models   = nullptr;
        s_wn_iface = nullptr;
        return false;
    }

    return true;
}

void wake_word_stop()
{
    if (!s_active && !s_task) return;
    s_stop_req = true;
    for (int i = 0; i < 200 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    s_status = "";
}

bool wake_word_is_active()             { return s_active; }
int  wake_word_get_energy()            { return s_active ? s_energy : 0; }

// Reports phase + chunks so a "stuck" state on the console is diagnosable:
//   phase 1 = blocked in mic.readBytes  (I2S / audio driver problem)
//   phase 2 = in the RMS energy calc     (shouldn't stall, it's ~1 KB)
//   phase 3 = in WakeNet detect()        (model inference is slow)
//   phase 4 = idle between iterations
// Chunks should climb ~30/s while listening; frozen chunks = frozen loop.
const char *wake_word_status_text()
{
    if (s_active) {
        static char buf[64];
        snprintf(buf, sizeof(buf), "%s p%d ch%lu",
                 s_status ? s_status : "?", s_dbg_phase,
                 (unsigned long)s_dbg_chunks);
        return buf;
    }
    return s_status ? s_status : "";
}

bool wake_word_pending_detection()     { return s_wake_flag; }
void wake_word_consume_detection()     { s_wake_flag = false; }
