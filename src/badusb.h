#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// BadUSB / USB Rubber Ducky for the T-Watch Ultra (ESP32-S3, native USB-OTG).
//
// The watch enumerates as a composite CDC + MSC + HID device (the HID keyboard
// interface is registered at static-init, next to usb_sd's MSC). It stays an
// IDLE keyboard — it never emits a keystroke until the user explicitly RUNs a
// payload from the UI. Flashing is unaffected: this board is always flashed via
// forced ROM download mode (BOOT+RESET), which bypasses the app's USB entirely.
//
// A small DuckyScript subset is supported (see badusb.cpp):
//   REM, STRING, STRINGLN, DELAY, DEFAULTDELAY/DEFAULT_DELAY,
//   ENTER, TAB, ESC/ESCAPE, SPACE, BACKSPACE, UP/DOWN/LEFT/RIGHT,
//   GUI/WINDOWS [key], CTRL/CONTROL [key], ALT [key], SHIFT [key],
//   CTRL-ALT [key], CTRL-SHIFT [key], GUI-SHIFT [key]
//
// ⚠️ Authorised use only: this injects keystrokes into whatever computer the
//    watch is plugged into. Your own machines / an engagement with scope.
// ---------------------------------------------------------------------------

void badusb_begin();          // USB.begin() + keyboard.begin() (idempotent)
bool badusb_ready();

bool badusb_run(const char *script);   // execute a payload in a background task
bool badusb_is_running();
int  badusb_progress();       // 0..100 (line based)
void badusb_stop();           // abort a running payload
