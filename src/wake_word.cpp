#include "wake_word.h"
#include <LilyGoLib.h>
#include <Arduino.h>
#include <string.h>

// ESP-SR public headers (vendored in lib/esp-sr/).
extern "C" {
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"
}

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

// libesp_audio_front_end.a calls check_chip_config() at AFE create time to
// warn if the CPU/PSRAM/cache aren't set to the recommended values. It's
// defined in esp_sr's esp_process_sdkconfig.c — a file we deliberately don't
// compile, because it drags in ~200 CONFIG_CN_SPEECH_COMMAND_IDx macros that
// only make sense inside a full ESP-IDF Kconfig build. The function is
// purely a diagnostic no-op if the CONFIG_* aren't set, so shim it here.
extern "C" void check_chip_config(void) {}

// ---- module state ---------------------------------------------------------
//
// All of this lives on core 1's audio task; the main thread only reads
// `s_active`, `s_energy`, `s_status`, `s_wake_flag` — declared volatile /
// atomic-friendly so we don't need a mutex for the LVGL tick's polling.

static volatile bool     s_active   = false;
static volatile bool     s_stop_req = false;
static volatile int      s_energy   = 0;
static volatile bool     s_wake_flag = false;
static const char *      s_status   = "";

static srmodel_list_t   *s_models   = nullptr;
static esp_afe_sr_data_t *s_afe_data = nullptr;
static const esp_afe_sr_iface_t *s_afe_handle = nullptr;
static TaskHandle_t      s_task     = nullptr;

// The AFE partition label MUST match partitions.csv — we called it "model".
static const char *SR_PARTITION = "model";

// ---- audio task -----------------------------------------------------------

// Runs on core 1. Reads chunks from the PDM mic (via LilyGoLib's I2SClass),
// feeds them into the AFE, then polls the AFE for wake-word results. Exits
// cleanly when s_stop_req flips. Kept single-task instead of the classic
// feed/fetch split because we're only doing wake-word for now — no comm
// pipeline, no reference channel, so there's no benefit to two tasks.
static void audio_task(void *)
{
    const int feed_chunk = s_afe_handle->get_feed_chunksize(s_afe_data);
    const int feed_ch    = s_afe_handle->get_total_channel_num(s_afe_data);
    const int feed_bytes = feed_chunk * feed_ch * sizeof(int16_t);

    // Prefer PSRAM for the ~1 KB feed buffer so we don't nibble at internal
    // SRAM that the rest of the firmware needs.
    int16_t *buf = (int16_t *)heap_caps_malloc(feed_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        s_status = "feed buf alloc failed";
        s_active = false;
        vTaskDelete(NULL);
        return;
    }

    s_status = "listening";
    s_active = true;

    while (!s_stop_req) {
        // instance.mic is the LilyGoLib I2SClass — same object mic_rec uses.
        // readBytes returns raw PCM 16-bit mono little-endian at 16 kHz,
        // which is exactly what AFE wants for the "1 mic, 0 ref, 16 kHz"
        // config we asked for.
        size_t got = instance.mic.readBytes((char *)buf, feed_bytes);
        if (got == 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        // Cheap energy proxy for the KITT bars — RMS on the raw samples,
        // mapped log-ish to 0..100. Not calibrated; the UI just wants
        // something that reacts to voice vs silence.
        int samples = got / sizeof(int16_t);
        uint32_t acc = 0;
        for (int i = 0; i < samples; i++) {
            int v = buf[i];
            if (v < 0) v = -v;
            acc += (uint32_t)v;
        }
        int avg = samples ? (int)(acc / samples) : 0;
        // 32767 max → clamp at ~4096 so a normal voice hits mid-scale.
        int lvl = (avg * 100) / 4096;
        if (lvl > 100) lvl = 100;
        s_energy = lvl;

        s_afe_handle->feed(s_afe_data, buf);

        afe_fetch_result_t *r = s_afe_handle->fetch(s_afe_data);
        if (r && r->wakeup_state == WAKENET_DETECTED) {
            s_wake_flag = true;
            s_status = "wake!";
            // Don't return — keep listening after a wake so the user can
            // trigger the tile more than once without re-arming.
        }
    }

    // Clean exit path
    if (buf) heap_caps_free(buf);
    if (s_afe_handle && s_afe_data) s_afe_handle->destroy(s_afe_data);
    s_afe_data = nullptr;
    if (s_models) esp_srmodel_deinit(s_models);
    s_models = nullptr;
    s_afe_handle = nullptr;
    s_energy = 0;
    s_active = false;
    s_task = nullptr;
    vTaskDelete(NULL);
}

// ---- public API -----------------------------------------------------------

bool wake_word_start()
{
    if (s_active) return true;

    s_status = "loading models";
    s_wake_flag = false;
    s_energy = 0;

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

    // Minimal AFE config for single-mic wake-word only. We deliberately
    // turn off AEC/VAD/SE so the engine's footprint stays small — those
    // are only needed for full command recognition with echo. AGC set to
    // NO_AGC because the T-Watch's PDM path is already reasonable.
    afe_config_t cfg = {};
    cfg.aec_init                        = false;
    cfg.se_init                         = false;
    cfg.vad_init                        = false;
    cfg.wakenet_init                    = true;
    cfg.voice_communication_init        = false;
    cfg.voice_communication_agc_init    = false;
    cfg.voice_communication_agc_gain    = 15;
    cfg.vad_mode                        = VAD_MODE_3;
    cfg.wakenet_model_name              = wn_name;
    cfg.wakenet_model_name_2            = NULL;
    cfg.wakenet_mode                    = DET_MODE_2CH_90;
    cfg.afe_mode                        = SR_MODE_LOW_COST;
    cfg.afe_perferred_core              = 1;
    cfg.afe_perferred_priority          = 5;
    cfg.afe_ringbuf_size                = 50;
    cfg.memory_alloc_mode               = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg.agc_mode                        = AFE_MN_PEAK_NO_AGC;
    cfg.pcm_config.total_ch_num         = 1;
    cfg.pcm_config.mic_num              = 1;
    cfg.pcm_config.ref_num              = 0;
    cfg.pcm_config.sample_rate          = 16000;
    cfg.debug_init                      = false;

    s_afe_handle = &ESP_AFE_SR_HANDLE;
    s_afe_data   = s_afe_handle->create_from_config(&cfg);
    if (!s_afe_data) {
        s_status = "AFE alloc failed";
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        s_afe_handle = nullptr;
        return false;
    }

    s_stop_req = false;
    // Task pinned to core 1 alongside LVGL. AFE is CPU-heavy but its own
    // internal task fanout is configured through `afe_perferred_core=1`
    // above, so we stay off core 0 (WiFi/BT stack lives there).
    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "wake_word", 4096,
                                            NULL, 4, &s_task, 1);
    if (ok != pdPASS) {
        s_status = "task spawn failed";
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = nullptr;
        esp_srmodel_deinit(s_models);
        s_models = nullptr;
        s_afe_handle = nullptr;
        return false;
    }

    return true;
}

void wake_word_stop()
{
    if (!s_active && !s_task) return;
    s_stop_req = true;
    // Poll for the task to exit — bounded so we don't hang the caller.
    for (int i = 0; i < 200 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    s_status = "";
}

bool wake_word_is_active()             { return s_active; }
int  wake_word_get_energy()            { return s_active ? s_energy : 0; }
const char *wake_word_status_text()    { return s_status ? s_status : ""; }

bool wake_word_pending_detection()     { return s_wake_flag; }
void wake_word_consume_detection()     { s_wake_flag = false; }
