#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Wake-word engine — offline "Hi ESP" detection built on Espressif's ESP-SR
// (WakeNet9), vendored into lib/esp-sr/ as precompiled static libraries.
//
// Reads the PDM microphone that LilyGoLib already brings up at boot (same
// path mic_rec.cpp uses, so the two are mutually exclusive — the caller
// stops one before starting the other). Runs the audio through the AFE
// front-end (single mic, no AEC, no VAD initially) and pushes chunks into
// WakeNet on a dedicated FreeRTOS task pinned to core 1, next to LVGL.
//
// On detection the registered callback fires from the AFE task, so keep the
// work in the callback tiny — post to a queue or set a volatile flag and
// let a UI tick pick it up. The callback runs on core 1 and MUST NOT touch
// LVGL directly.
//
// See docs/esp-sr-plan.md for the full research + decision log that led to
// this vendored setup.
// ---------------------------------------------------------------------------

// Bring the engine up: load the models from the "model" partition, allocate
// AFE + WakeNet, spawn the feed/fetch task. Returns false if the partition
// is missing (srmodels.bin not flashed yet), the model isn't in the
// partition, or memory allocation fails — the caller gets a clean "not
// started" state and the KITT UI can show the reason.
bool wake_word_start();

// Tear down. Safe to call from any task; the audio task is asked to exit
// and joined before AFE + models are freed.
void wake_word_stop();

// Fast query for the LVGL tick — no locking, cheap.
bool wake_word_is_active();

// Last audio energy the AFE handed us, mapped to 0..100 for the KITT bars.
// Meant for UI polish, not analysis; returns 0 when the engine isn't running.
int  wake_word_get_energy();

// Human-readable last status. Returns "" until wake_word_start is first
// called, then one of: "loading models", "listening", "wake!", "AFE alloc
// failed", "model not in partition", "partition 'model' missing".
const char *wake_word_status_text();

// True once since the last wake_word_consume_detection(). Cleared by that
// call. The UI checks this from its tick, so no callback plumbing needed
// for the first cut.
bool wake_word_pending_detection();
void wake_word_consume_detection();
