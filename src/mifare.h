#pragma once
#include <stdint.h>
#include <stdbool.h>

// Mifare / ISO14443A card reader (identification) for the T-Watch Ultra's
// ST25R3916. Reuses the same rfalNfc discover/worker path as nfc_screen, so it
// reliably brings a card up and reads its identity (UID / SAK / ATQA / type).
//
// NOTE on the dict attack (key cracking): a REAL Mifare Classic dict attack
// needs a reader-side Crypto-1 (crapto1 "encrypt-and-feed" word cipher). The
// Crypto-1 vendored from SCR-Terminal (nfc_crypto.cpp) is the CARD/emulation
// flavour and cannot encrypt the reader nonce, so it can't drive reader auth.
// This module therefore ships identification only; the auth engine lands once a
// reader-side crypto1 is in the tree.

struct MifareCard {
    uint8_t uid[10];
    uint8_t uidlen;
    uint8_t sak;
    uint8_t atqa[2];
    char    type[24];
    int     sectors;      // 16 (1K/Mini) or 40 (4K); 0 if not Classic
    bool    is_classic;
};

void mifare_reader_start();    // power NFC + begin discovery
void mifare_reader_stop();     // deactivate + power NFC off
void mifare_reader_worker();   // call from loop(); drives rfalNfcWorker
bool mifare_have_card();
bool mifare_get_card(MifareCard *out);
void mifare_forget_card();     // clear + re-arm discovery for the next tap

// --- Real Mifare Classic dictionary attack (crapto1) -------------------------
// Runs a background task that, for the card currently on the reader, tries a key
// dictionary against every sector with a REAL Crypto-1 authentication (the auth
// completes only when aT decrypts to suc3(nt) — so a wrong key never reports a
// false hit). Re-selects the card between attempts (a failed auth halts it).
// ⚠️ Experimental: the crypto core is canonical crapto1, but the ST25R3916
//    bit-level encrypted-parity TX/timing may need tuning on real hardware.
bool mifare_dict_start();      // false if no Classic card present or already running
void mifare_dict_stop();       // abort a running attack
bool mifare_dict_running();
int  mifare_dict_progress();   // 0..100
int  mifare_dict_found();      // sectors with a recovered key
int  mifare_dict_total();      // total sectors being tried
// Fetch a recovered key for sector `sec`: keytype 0=A 1=B. false if not cracked.
bool mifare_dict_key(int sec, uint8_t *keytype, uint8_t key[6]);
