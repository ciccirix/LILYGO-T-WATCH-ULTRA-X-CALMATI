#include "alarm.h"
#include "usb_sd.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <time.h>
#include <stdio.h>
#include <math.h>
// LilyGoLib on arduino-esp32 3.x exposes the I2S player as ESP_I2S's
// I2SClass, whose reconfigure API is used below in chime_task().

// Defined in main.cpp — current displayed local time (RTC + UTC offset).
void clock_screen_get_local_time(struct tm *out);
// Defined in alarm_screen.cpp — swaps to the full-screen ringing overlay.
void alarm_screen_show_ringing();

#define ALARM_PATH               "/Settings/alarm.txt"
// Bumped each time the on-disk schema changes meaningfully. alarm_init()
// requires this version on the saved file; older files (no version field, or
// a lower number) are treated as if no save exists — the screen falls back
// to its "now + 1 hour" suggestion instead of loading stale defaults left
// over from earlier iterations of this code.
#define ALARM_FILE_VERSION       1
// Cap how long the haptic vibrates so a missed alarm doesn't drain the battery
// overnight. The ringing screen stays up until dismissed; only the buzz +
// chime stop.
#define ALARM_AUTO_QUIET_MS      (5 * 60 * 1000)

// Chime PCM parameters. The MAX98357A amp has no software volume control
// (gain is set by a strap pin) — peak amplitude is the only knob, so 100%
// volume means full-scale signed-16-bit samples (±32767). Volume scaling
// at runtime multiplies CHIME_PEAK by (s_volume / 100).
#define CHIME_SAMPLE_RATE        16000
#define CHIME_PEAK               32767    // out of ±32767 — full scale
#define CHIME_CHUNK_SAMPLES      256

static int      s_hour              = 7;
static int      s_minute            = 0;
static bool     s_enabled           = false;
static bool     s_configured        = false;

// Alert behaviour — persisted alongside the alarm time. Defaults represent
// a fresh device: vibrate + audio both on, volume full, snooze 5 minutes.
static bool     s_vibrate           = true;
static bool     s_audio             = true;
static uint8_t  s_volume            = 100;
static int      s_snooze_minutes    = 5;

static bool     s_ringing           = false;
static int      s_last_trigger_yday = -1;
static uint32_t s_ring_start_ms     = 0;
static uint32_t s_last_vibrate_ms   = 0;
static uint32_t s_snooze_until_ms   = 0;

// ---- persistence -----------------------------------------------------------

static void save_to_sd()
{
    if (usb_sd_is_running() || !instance.isCardReady()) return;
    if (!SD.exists("/Settings")) SD.mkdir("/Settings");
    File f = SD.open(ALARM_PATH, FILE_WRITE);
    if (!f) return;
    f.printf("version=%d\n", ALARM_FILE_VERSION);
    f.printf("enabled=%d\n", s_enabled ? 1 : 0);
    f.printf("hour=%d\n",    s_hour);
    f.printf("minute=%d\n",  s_minute);
    f.printf("vibrate=%d\n", s_vibrate ? 1 : 0);
    f.printf("audio=%d\n",   s_audio   ? 1 : 0);
    f.printf("volume=%d\n",  (int)s_volume);
    f.printf("snooze=%d\n",  s_snooze_minutes);
    f.close();
}

void alarm_init()
{
    if (!instance.isCardReady()) return;
    if (!SD.exists(ALARM_PATH)) return;
    File f = SD.open(ALARM_PATH, FILE_READ);
    if (!f) return;

    int  version = 0;
    int  hour    = s_hour;
    int  minute  = s_minute;
    bool enabled = s_enabled;
    int  vibrate = s_vibrate ? 1 : 0;
    int  audio   = s_audio   ? 1 : 0;
    int  volume  = (int)s_volume;
    int  snooze  = s_snooze_minutes;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        String key = line.substring(0, eq);
        long   v   = line.substring(eq + 1).toInt();
        if      (key == "version") version = (int)v;
        else if (key == "enabled") enabled = (v != 0);
        else if (key == "hour")    hour    = (int)v;
        else if (key == "minute")  minute  = (int)v;
        else if (key == "vibrate") vibrate = (int)v;
        else if (key == "audio")   audio   = (int)v;
        else if (key == "volume")  volume  = (int)v;
        else if (key == "snooze")  snooze  = (int)v;
    }
    f.close();

    // Ignore pre-versioned files — those were written by an earlier build
    // that auto-persisted a stale "now + 1 hour" snapshot at boot, and we
    // don't want those values surviving forever.
    if (version < ALARM_FILE_VERSION) return;

    s_hour       = hour;
    s_minute     = minute;
    s_enabled    = enabled;
    s_vibrate    = (vibrate != 0);
    s_audio      = (audio   != 0);
    // Clamp volume to 0..100; the slider can only produce that range but a
    // hand-edited file shouldn't be able to overflow the chime amplitude.
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    s_volume     = (uint8_t)volume;
    // Snooze options are fixed at 5/10/20 minutes; anything else falls back
    // to 5 so a malformed file can't leave the user with a 1-second snooze.
    s_snooze_minutes = (snooze == 10 || snooze == 20) ? snooze : 5;
    s_configured = true;
}

// ---- accessors / setters ---------------------------------------------------

int  alarm_get_hour()      { return s_hour; }
int  alarm_get_minute()    { return s_minute; }
bool alarm_is_enabled()    { return s_enabled; }
bool alarm_is_ringing()    { return s_ringing; }
bool alarm_is_configured() { return s_configured; }

bool    alarm_get_vibrate()         { return s_vibrate;        }
bool    alarm_get_audio()           { return s_audio;          }
uint8_t alarm_get_volume()          { return s_volume;         }
int     alarm_get_snooze_minutes()  { return s_snooze_minutes; }

void alarm_set_vibrate(bool on) { if (s_vibrate != on) { s_vibrate = on; save_to_sd(); } }
void alarm_set_audio(bool on)   { if (s_audio   != on) { s_audio   = on; save_to_sd(); } }

void alarm_set_volume(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (s_volume != pct) {
        s_volume = pct;
        save_to_sd();
        // Live update — the running chime task reads s_volume each chunk via
        // chime_write_tone(), so a mid-ring tweak takes effect immediately.
    }
}

void alarm_set_snooze_minutes(int minutes)
{
    if (minutes != 5 && minutes != 10 && minutes != 20) minutes = 5;
    if (s_snooze_minutes != minutes) {
        s_snooze_minutes = minutes;
        save_to_sd();
    }
}

// Returns the wall-clock time at which the alarm will next fire — usually the
// saved alarm time, but a snooze pushes it forward by however many minutes
// the user picked.
void alarm_get_next_fire_time(int *h_out, int *m_out)
{
    if (s_snooze_until_ms != 0) {
        uint32_t now_ms = millis();
        // Unsigned subtraction wraps cleanly if the deadline is in the past.
        uint32_t remaining_ms = (s_snooze_until_ms >= now_ms)
            ? (s_snooze_until_ms - now_ms) : 0u;

        struct tm now_tm;
        clock_screen_get_local_time(&now_tm);
        int total_seconds = now_tm.tm_hour * 3600 + now_tm.tm_min * 60 + now_tm.tm_sec
                          + (int)(remaining_ms / 1000);
        if (h_out) *h_out = (total_seconds / 3600) % 24;
        if (m_out) *m_out = (total_seconds / 60) % 60;
        return;
    }
    if (h_out) *h_out = s_hour;
    if (m_out) *m_out = s_minute;
}

void alarm_set(int hour, int minute, bool enabled)
{
    if (hour < 0)    hour = 0;
    if (hour > 23)   hour = 23;
    if (minute < 0)  minute = 0;
    if (minute > 59) minute = 59;
    s_hour       = hour;
    s_minute     = minute;
    s_enabled    = enabled;
    s_configured = true;
    // Allow the (possibly new) time to fire today even if today already had
    // a ring + dismiss for the previous setting.
    s_last_trigger_yday = -1;
    save_to_sd();
}

// ---- chime task ------------------------------------------------------------
//
// The MAX98357A I2S amplifier is already wired up by LilyGoLib (instance.player
// is init'd in instance.begin()). The amp's power rail (BLDO2 / POWER_SPEAK) is
// gated by powerControl() so we toggle that around the chime task to keep idle
// current low. The task generates a doorbell-style two-tone pattern in 16-bit
// PCM at 16 kHz mono and writes it to I2S until s_chime_active goes false.

static volatile bool s_chime_active = false;
static TaskHandle_t  s_chime_task   = nullptr;
// Volume override used while a borrowed chime (e.g. timer expiry) is
// playing. 0 = no override → fall back to the user's saved alarm volume.
// Read on every PCM chunk by chime_write_tone() so a caller can change it
// mid-loop, same as s_volume.
static volatile uint8_t s_chime_override_volume = 0;

static void chime_write_tone(uint32_t freq_hz, uint32_t duration_ms,
                             int16_t *buf, int buf_samples, float *phase_io)
{
    const float two_pi = 6.28318530718f;
    int total_samples = (int)((uint64_t)duration_ms * CHIME_SAMPLE_RATE / 1000u);
    float phase = phase_io ? *phase_io : 0.0f;

    while (total_samples > 0 && s_chime_active) {
        int n = (total_samples < buf_samples) ? total_samples : buf_samples;
        if (freq_hz == 0) {
            memset(buf, 0, n * sizeof(int16_t));
        } else {
            // Re-read s_volume each chunk so a mid-ring slider change takes
            // effect on the next ~16 ms boundary. Integer scale keeps the
            // peak within int16 range at volume==100. A non-zero override
            // (set by a borrowed-chime caller like the timer) wins over
            // the saved value for the lifetime of that chime.
            uint8_t override_vol = s_chime_override_volume;
            int eff_vol = override_vol ? (int)override_vol : (int)s_volume;
            int amp = (CHIME_PEAK * eff_vol) / 100;
            float dphase = two_pi * (float)freq_hz / (float)CHIME_SAMPLE_RATE;
            for (int i = 0; i < n; i++) {
                buf[i] = (int16_t)(sinf(phase) * amp);
                phase += dphase;
                if (phase >= two_pi) phase -= two_pi;
            }
        }
        instance.player.write(buf, n * sizeof(int16_t));
        total_samples -= n;
    }
    if (phase_io) *phase_io = phase;
}

static void chime_task(void *)
{
    instance.powerControl(POWER_SPEAK, true);
    vTaskDelay(pdMS_TO_TICKS(20));   // let the amp settle
    // Reconfigure the I2S TX to our 16 kHz mono chime rate. LilyGoLib brings
    // the player up in stereo/44.1 kHz by default; we drop it to 16k mono for
    // the doorbell tone. This is the new-API replacement for the legacy
    // i2s_set_clk() the earlier IDF 5.1 build used.
    instance.player.configureTX(CHIME_SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);

    int16_t chunk[CHIME_CHUNK_SAMPLES];
    float   phase = 0.0f;
    while (s_chime_active) {
        // Doorbell-style two-tone chime: high tone, short gap, low tone,
        // longer gap, then repeat.
        chime_write_tone(1200, 220, chunk, CHIME_CHUNK_SAMPLES, &phase);
        if (!s_chime_active) break;
        chime_write_tone(0,    60,  chunk, CHIME_CHUNK_SAMPLES, &phase);
        if (!s_chime_active) break;
        chime_write_tone(800,  220, chunk, CHIME_CHUNK_SAMPLES, &phase);
        if (!s_chime_active) break;
        chime_write_tone(0,    900, chunk, CHIME_CHUNK_SAMPLES, &phase);
    }

    instance.powerControl(POWER_SPEAK, false);
    s_chime_task = nullptr;
    vTaskDelete(NULL);
}

static void stop_chime()
{
    // The task exits on its next iteration check; the amp power-off happens
    // there too so we never disable BLDO2 while a write is still queued.
    s_chime_active = false;
}

static void start_chime()
{
    if (s_chime_task) return;        // already running
    s_chime_active = true;
    xTaskCreatePinnedToCore(chime_task, "alarm_chime", 4096, NULL, 1,
                            &s_chime_task, 0);
}

// ---- shared-chime API ------------------------------------------------------

void alarm_play_chime_loop(uint8_t volume_override)
{
    if (volume_override > 100) volume_override = 100;
    s_chime_override_volume = volume_override;   // 0 = use saved s_volume
    start_chime();
}

void alarm_stop_chime_loop()
{
    stop_chime();
    s_chime_override_volume = 0;   // clear so the next alarm-driven chime
                                   // reverts to the saved volume
}

// ---- ring trigger ----------------------------------------------------------

static void start_ringing()
{
    s_ringing         = true;
    s_ring_start_ms   = millis();
    s_last_vibrate_ms = 0;
    if (s_vibrate) instance.vibrator();  // immediate first buzz, if enabled
    if (s_audio)   start_chime();        // doorbell loop, if enabled
    alarm_screen_show_ringing();
}

void alarm_dismiss()
{
    s_ringing         = false;
    s_snooze_until_ms = 0;
    stop_chime();
    // s_last_trigger_yday is left at today — prevents an immediate re-fire on
    // the next tick while the minute still matches.
}

void alarm_snooze(int minutes)
{
    s_ringing         = false;
    s_snooze_until_ms = millis() + (uint32_t)minutes * 60u * 1000u;
    stop_chime();
}

void alarm_tick()
{
    uint32_t now_ms = millis();

    // Snooze re-fire takes priority over the time-of-day match.
    if (s_snooze_until_ms != 0 && now_ms >= s_snooze_until_ms) {
        s_snooze_until_ms = 0;
        start_ringing();
        return;
    }

    if (s_ringing) {
        // Buzz once every 1.5 s while still within the auto-quiet window. The
        // ringing screen stays up beyond that until the user taps DISMISS.
        if (now_ms - s_ring_start_ms < ALARM_AUTO_QUIET_MS) {
            if (s_vibrate && now_ms - s_last_vibrate_ms >= 1500) {
                s_last_vibrate_ms = now_ms;
                instance.vibrator();
            }
        } else {
            // Auto-quiet: stop the chime as well so the speaker doesn't keep
            // looping for an unattended alarm.
            stop_chime();
        }
        return;
    }

    if (!s_enabled) return;

    struct tm t;
    clock_screen_get_local_time(&t);
    mktime(&t);   // normalise the struct so tm_yday is filled in
    if (t.tm_hour == s_hour && t.tm_min == s_minute) {
        if (s_last_trigger_yday != t.tm_yday) {
            s_last_trigger_yday = t.tm_yday;
            start_ringing();
        }
    }
}
