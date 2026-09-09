#pragma once
#include <stdint.h>
#include <stdbool.h>

// Crypto-1 stream cipher (MIFARE Classic). Vendored from SCR-Terminal
// (gitlab.com/sacriphanius/scr-terminal, src/hal/nfc_crypto.{cpp,h}) — a
// ChameleonMini-style table-driven implementation. Pure computation, no HW deps.
//
// NOTE: SCR ships this cipher but never wires it into a real reader auth; the
// real Mifare Classic authentication that USES it lives in our mifare.cpp.

#ifdef __cplusplus
extern "C" {
#endif

uint8_t oddparity8(uint8_t x);

void Crypto1Setup(uint8_t Key[6], uint8_t Uid[4], uint8_t CardNonce[4]);
void Crypto1Auth(uint8_t EncryptedReaderNonce[4]);
uint8_t Crypto1Byte(void);
void Crypto1ByteArray(uint8_t *Buffer, uint8_t Count);
uint8_t Crypto1Nibble(void);

#ifdef __cplusplus
}
#endif
