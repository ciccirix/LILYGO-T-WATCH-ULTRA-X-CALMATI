#include "badusb.h"

// ── BadUSB temporarily DISABLED ──────────────────────────────────────────────
// This board builds with ARDUINO_USB_MODE=0 + ARDUINO_USB_CDC_ON_BOOT=1, so the
// TinyUSB composite device is brought up AT BOOT. Registering a USB HID keyboard
// (the `static USBHIDKeyboard` this file used to hold) therefore added a HID
// interface to the boot-time USB descriptor — the suspected cause of the
// flashing regression (err31 "device not functioning" on the app port, broken
// auto-reset-to-bootloader). Stubbed out to restore the known-good CDC+MSC USB.
//
// To bring BadUSB back safely later: gate the HID behind a dedicated build/boot
// mode so a normal boot never enumerates it (see notes in git / memory).

void badusb_begin()               {}
bool badusb_ready()               { return false; }
bool badusb_run(const char *)     { return false; }
bool badusb_is_running()          { return false; }
int  badusb_progress()            { return 0; }
void badusb_stop()                {}
