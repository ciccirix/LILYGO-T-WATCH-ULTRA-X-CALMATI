#pragma once
#include <stdint.h>

// Reader-side Crypto-1 — vendored from crapto1 (bla <blapost@gmail.com>, GPLv2),
// nfc-tools/mfcuk src/crapto1.h + crypto1.c. Only the forward cipher is kept
// (create/bit/byte/word + prng_successor); the LFSR-recovery/rollback cracking
// helpers are omitted (a dict attack doesn't need them).
//
// This is the RIGHT flavour for a READER (encrypt-and-feed via crypto1_word),
// unlike the card-side nfc_crypto.cpp — see mifare.cpp for why.
//
// The bit macros / filter / parity are namespaced (CR1_ / cr1_) so they do NOT
// collide with ESP-IDF's own one-argument BIT() macro.

#ifdef __cplusplus
extern "C" {
#endif

struct Crypto1State { uint32_t odd, even; };

void     crypto1_init(struct Crypto1State *s, uint64_t key);   // no-malloc crypto1_create
uint8_t  crypto1_bit(struct Crypto1State *s, uint8_t in, int is_encrypted);
uint8_t  crypto1_byte(struct Crypto1State *s, uint8_t in, int is_encrypted);
uint32_t crypto1_word(struct Crypto1State *s, uint32_t in, int is_encrypted);
uint32_t prng_successor(uint32_t x, uint32_t n);

#define LF_POLY_ODD  (0x29CE5C)
#define LF_POLY_EVEN (0x870804)
#define CR1_BIT(x, n)   ((x) >> (n) & 1)
#define CR1_BEBIT(x, n) CR1_BIT(x, (n) ^ 24)

static inline int cr1_parity(uint32_t x)
{
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    return CR1_BIT(0x6996, x & 0xf);
}

static inline int cr1_filter(uint32_t const x)
{
    uint32_t f;
    f  = 0xf22c0 >> (x       & 0xf) & 16;
    f |= 0x6c9c0 >> (x >>  4 & 0xf) &  8;
    f |= 0x3c8b0 >> (x >>  8 & 0xf) &  4;
    f |= 0x1e458 >> (x >> 12 & 0xf) &  2;
    f |= 0x0d938 >> (x >> 16 & 0xf) &  1;
    return CR1_BIT(0xEC57E80A, f);
}

#ifdef __cplusplus
}
#endif
