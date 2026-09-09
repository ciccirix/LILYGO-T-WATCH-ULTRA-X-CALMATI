// Reader-side Crypto-1 (crapto1, bla, GPLv2). Forward cipher only.
// Vendored from nfc-tools/mfcuk src/crypto1.c; crypto1_create() replaced by a
// no-malloc crypto1_init() so the auth loop allocates nothing.

#include "crapto1.h"

void crypto1_init(struct Crypto1State *s, uint64_t key)
{
    s->odd = 0;
    s->even = 0;
    for (int i = 47; i > 0; i -= 2) {
        s->odd  = s->odd  << 1 | CR1_BIT(key, (i - 1) ^ 7);
        s->even = s->even << 1 | CR1_BIT(key, i ^ 7);
    }
}

uint8_t crypto1_bit(struct Crypto1State *s, uint8_t in, int is_encrypted)
{
    uint32_t feedin;
    uint8_t  ret = cr1_filter(s->odd);

    feedin  = ret & !!is_encrypted;
    feedin ^= !!in;
    feedin ^= LF_POLY_ODD & s->odd;
    feedin ^= LF_POLY_EVEN & s->even;
    s->even = s->even << 1 | cr1_parity(feedin);

    uint32_t x = s->odd;
    s->odd = s->even;
    s->even = x;

    return ret;
}

uint8_t crypto1_byte(struct Crypto1State *s, uint8_t in, int is_encrypted)
{
    uint8_t ret = 0;
    for (uint8_t i = 0; i < 8; ++i)
        ret |= crypto1_bit(s, CR1_BIT(in, i), is_encrypted) << i;
    return ret;
}

uint32_t crypto1_word(struct Crypto1State *s, uint32_t in, int is_encrypted)
{
    uint32_t ret = 0;
    for (uint32_t i = 0; i < 32; ++i)
        ret |= (uint32_t)crypto1_bit(s, CR1_BEBIT(in, i), is_encrypted) << (i ^ 24);
    return ret;
}

uint32_t prng_successor(uint32_t x, uint32_t n)
{
#define SWAPENDIAN(x) \
    (x = (x >> 8 & 0xff00ff) | (x & 0xff00ff) << 8, x = x >> 16 | x << 16)
    SWAPENDIAN(x);
    while (n--)
        x = x >> 1 | (x >> 16 ^ x >> 18 ^ x >> 19 ^ x >> 21) << 31;
    return SWAPENDIAN(x);
#undef SWAPENDIAN
}
