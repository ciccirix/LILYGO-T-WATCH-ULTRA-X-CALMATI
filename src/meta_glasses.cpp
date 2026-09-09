#include "meta_glasses.h"
#include "ble_scan_manager.h"
#include "usb_sd.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <SD.h>
#include <string.h>
#include <time.h>

void clock_screen_get_local_time(struct tm *out);   // wall-clock for the log

// ─── identifier tables (ported from ESP32-Marauder C5, filters via NullPxl) ──
// Extend META_IDS with more smart-glasses maker IDs — but use REAL Bluetooth
// SIG company IDs / assigned 16-bit UUIDs, never guesses (a wrong id = false
// positives). Oakley Meta needs nothing extra: Oakley is Luxottica (0x0D53).
static const uint16_t META_IDS[] = {
    0xFD5F,   // Meta
    0xFEB7,   // Meta
    0xFEB8,   // Meta
    0x01AB,   // Meta / Facebook
    0x058E,   // Meta
    0x0D53,   // Luxottica (Ray-Ban Meta, Oakley Meta)
};
// If any of these show up in the advert it's a phone/other, NOT glasses — skip.
static const uint16_t BLOCKED_IDS[] = {
    0xFD5A, 0xFD69,   // Samsung
    0x004C,           // Apple
    0x0006,           // Microsoft
    0xFEF3,           // common phone service
};

static bool is_meta(uint16_t id)
{
    for (unsigned i = 0; i < sizeof(META_IDS) / sizeof(META_IDS[0]); i++)
        if (META_IDS[i] == id) return true;
    return false;
}
static bool is_blocked(uint16_t id)
{
    for (unsigned i = 0; i < sizeof(BLOCKED_IDS) / sizeof(BLOCKED_IDS[0]); i++)
        if (BLOCKED_IDS[i] == id) return true;
    return false;
}

// Which identifier "wins" when an advert carries several (Ray-Ban glasses send
// both the Luxottica id and a Meta id). Luxottica (0x0D53) → rayban photo;
// Quest ids → quest; anything else → generic meta. Higher = preferred.
static int meta_prio(uint16_t id)
{
    if (id == 0x0D53) return 3;                     // Luxottica → Ray-Ban / Oakley
    if (id == 0x058E || id == 0xFD5F) return 2;     // Quest
    return 1;                                       // generic Meta
}

// ─── live store ──────────────────────────────────────────────────────────────
#define META_MAX 32
#define META_RAW 62                          // max adv_data + scan_rsp
static MetaHit s_hits[META_MAX];
static uint8_t s_raw[META_MAX][META_RAW];    // last raw advert per hit (capture)
static uint8_t s_raw_len[META_MAX];
static uint8_t s_atype[META_MAX];            // GAP address type per hit
static int     s_count = 0;
static bool    s_running = false;
static volatile bool s_capture = false;      // capture-mode toggle (screen tap)
static int     s_cap_total = 0;              // records saved this session

static void fold(const uint8_t *mac, int8_t rssi, uint8_t atype, uint16_t id,
                 const uint8_t *adv, int adv_len, uint32_t now)
{
    int idx = -1;
    for (int i = 0; i < s_count; i++)
        if (memcmp(s_hits[i].mac, mac, 6) == 0) { idx = i; break; }
    if (idx < 0) {
        if (s_count >= META_MAX) return;
        idx = s_count++;
        memcpy(s_hits[idx].mac, mac, 6);
        s_hits[idx].hits = 0;
    }
    s_hits[idx].rssi    = rssi;
    s_hits[idx].id      = id;
    s_hits[idx].last_ms = now;
    if (s_hits[idx].hits < 0xFFFF) s_hits[idx].hits++;

    // Keep the raw advert + address type so capture mode can dump them later.
    s_atype[idx] = atype;
    int n = adv_len; if (n > META_RAW) n = META_RAW; if (n < 0) n = 0;
    memcpy(s_raw[idx], adv, n);
    s_raw_len[idx] = (uint8_t)n;
}

// ─── classifier ──────────────────────────────────────────────────────────────
bool meta_glasses_check(const uint8_t *mac6, int8_t rssi, uint8_t addr_type,
                        const uint8_t *adv, int adv_len, uint16_t *out_id)
{
    bool     blocked = false, match = false;
    uint16_t matched = 0;
    int      best    = 0;

    for (int pos = 0; pos + 1 < adv_len; ) {
        uint8_t len = adv[pos];
        if (len == 0) break;
        if (pos + 1 + len > adv_len) break;
        uint8_t        type = adv[pos + 1];
        const uint8_t *d    = adv + pos + 2;
        int            dlen = (int)len - 1;

        if (type == 0xFF && dlen >= 2) {                 // manufacturer company id
            uint16_t id = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
            if (is_blocked(id)) blocked = true;
            else if (is_meta(id)) { match = true; if (meta_prio(id) > best) { best = meta_prio(id); matched = id; } }
        } else if ((type == 0x02 || type == 0x03) && dlen >= 2) {  // 16-bit svc UUIDs
            for (int k = 0; k + 1 < dlen; k += 2) {
                uint16_t id = (uint16_t)d[k] | ((uint16_t)d[k + 1] << 8);
                if (is_blocked(id)) blocked = true;
                else if (is_meta(id)) { match = true; if (meta_prio(id) > best) { best = meta_prio(id); matched = id; } }
            }
        } else if (type == 0x16 && dlen >= 2) {          // service data (16-bit uuid)
            uint16_t id = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
            if (is_blocked(id)) blocked = true;
            else if (is_meta(id)) { match = true; if (meta_prio(id) > best) { best = meta_prio(id); matched = id; } }
        }
        pos += 1 + len;
    }

    if (blocked || !match) return false;      // a blocked id wins → not glasses
    if (out_id) *out_id = matched;
    fold(mac6, rssi, addr_type, matched, adv, adv_len, millis());
    return true;
}

// ─── BLE consumer (standalone tile) ──────────────────────────────────────────
static void on_scan_result(esp_ble_gap_cb_param_t *param)
{
    auto &res = param->scan_rst;
    int total = (int)res.adv_data_len + (int)res.scan_rsp_len;
    meta_glasses_check(res.bda, (int8_t)res.rssi, (uint8_t)res.ble_addr_type,
                       res.ble_adv, total);
}

bool meta_glasses_start()
{
    if (s_running) return true;
    meta_glasses_reset();
    if (!ble_scan_add(on_scan_result)) return false;
    s_running = true;
    return true;
}

void meta_glasses_stop()
{
    if (!s_running) return;
    ble_scan_remove(on_scan_result);
    s_running = false;
}

bool meta_glasses_is_running() { return s_running; }

// ─── read API ────────────────────────────────────────────────────────────────
int meta_glasses_count() { return s_count; }

int meta_glasses_get(MetaHit *out, int max)
{
    // Snapshot first (the BLE task may be folding into s_hits concurrently), then
    // sort the copy — never mutate the store from the reader.
    MetaHit tmp[META_MAX];
    int c = s_count; if (c > META_MAX) c = META_MAX;
    for (int i = 0; i < c; i++) tmp[i] = s_hits[i];
    for (int i = 0; i < c; i++)
        for (int j = i + 1; j < c; j++)
            if (tmp[j].rssi > tmp[i].rssi) { MetaHit t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t; }
    int n = c < max ? c : max;
    for (int i = 0; i < n; i++) out[i] = tmp[i];
    return n;
}

void meta_glasses_reset() { s_count = 0; }

// ─── capture mode (optician / research) ──────────────────────────────────────
void meta_glasses_set_capture(bool on)   { s_capture = on; }
bool meta_glasses_capture_enabled()      { return s_capture; }
int  meta_glasses_capture_total()        { return s_cap_total; }

static const char *atype_str(uint8_t t)
{
    switch (t) {
        case BLE_ADDR_TYPE_PUBLIC:     return "public";
        case BLE_ADDR_TYPE_RANDOM:     return "random";
        case BLE_ADDR_TYPE_RPA_PUBLIC: return "rpa-public";
        case BLE_ADDR_TYPE_RPA_RANDOM: return "rpa-random";
        default:                       return "?";
    }
}

// Pull the advertised local name (AD 0x08 short / 0x09 complete) from a raw advert.
static void adv_name(const uint8_t *adv, int len, char *out, int outsz)
{
    out[0] = 0;
    for (int pos = 0; pos + 1 < len; ) {
        uint8_t l = adv[pos]; if (!l) break;
        if (pos + 1 + l > len) break;
        uint8_t type = adv[pos + 1];
        if (type == 0x08 || type == 0x09) {
            int n = (int)l - 1; if (n > outsz - 1) n = outsz - 1;
            for (int i = 0; i < n; i++) {
                uint8_t c = adv[pos + 2 + i];
                out[i] = (c >= 32 && c < 127) ? (char)c : '.';
            }
            out[n] = 0;
            return;
        }
        pos += 1 + l;
    }
}

bool meta_glasses_save_labeled(const uint8_t *mac6, const char *label)
{
    int idx = -1;
    for (int i = 0; i < s_count; i++)
        if (memcmp(s_hits[i].mac, mac6, 6) == 0) { idx = i; break; }
    if (idx < 0) return false;

    // Snapshot the record locally — the BLE task may fold new data into this slot.
    uint8_t raw[META_RAW];
    int     rl = s_raw_len[idx]; if (rl > META_RAW) rl = META_RAW;
    memcpy(raw, s_raw[idx], rl);
    uint8_t  at  = s_atype[idx];
    int8_t   rs  = s_hits[idx].rssi;
    uint16_t id  = s_hits[idx].id;

    char name[32]; adv_name(raw, rl, name, sizeof(name));

    struct tm tmv; clock_screen_get_local_time(&tmv);
    char hdr[220];
    snprintf(hdr, sizeof(hdr),
        "\n=== %04d-%02d-%02d %02d:%02d:%02d  [%s]\n"
        "MAC %02X:%02X:%02X:%02X:%02X:%02X  type=%s  rssi=%d  id=0x%04X  name=\"%s\"\nraw ",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec, label ? label : "?",
        mac6[0], mac6[1], mac6[2], mac6[3], mac6[4], mac6[5],
        atype_str(at), (int)rs, id, name);

    char hex[2 * META_RAW + 2]; int hl = 0;
    for (int i = 0; i < rl && hl < (int)sizeof(hex) - 3; i++)
        hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X", raw[i]);
    hex[hl] = 0;

    Serial.print(hdr); Serial.println(hex);

    if (!usb_sd_is_running() && instance.isCardReady()) {
        if (!SD.exists("/meta")) SD.mkdir("/meta");
        File f = SD.open("/meta/adv_capture.txt", FILE_APPEND);
        if (!f) return false;
        f.print(hdr); f.println(hex); f.close();
    }
    s_cap_total++;
    return true;
}
