#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Microphone recorder — streams the PDM mic to a WAV file on the microSD.
//
// The PDM mic (initialised by instance.begin() at boot) is read in a small
// FreeRTOS task; the audio is handed to the main/LVGL task through a stream
// buffer and written to SD there, so the SD card is only ever touched from one
// context (matching the rest of the firmware). Files land in /Recordings as
// 16 kHz mono 16-bit PCM WAV — playable anywhere.
//
// Recording is independent of the screen: swiping away leaves it running (a
// hard cap auto-stops it so a forgotten session can't fill the card).
// ---------------------------------------------------------------------------

// Start if idle, stop (and finalise the file) if recording.
void mic_rec_toggle();
void mic_rec_start();
void mic_rec_stop();

bool     mic_rec_is_recording();
uint32_t mic_rec_elapsed_ms();
size_t   mic_rec_bytes();          // audio bytes written so far
int      mic_rec_level();          // instantaneous input level, 0..100
const char *mic_rec_last_path();   // path of the current/last file ("" if none)
const char *mic_rec_status_text(); // human-readable state / error
