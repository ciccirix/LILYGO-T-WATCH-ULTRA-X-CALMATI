#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Shared command console. One text command dispatcher, reused by the web panel
// (POST a line, show the reply) and the USB serial monitor. Keeping the command
// set in one place means every transport gets the same 40+ actions for free.
// ---------------------------------------------------------------------------

// Run one command line; append human-readable output to `out`.
void console_exec(const char *line, String &out);

// Poll the USB serial port for a line and run it, echoing to Serial. Call from
// loop() alongside the other *_tick()s.
void console_serial_tick();
