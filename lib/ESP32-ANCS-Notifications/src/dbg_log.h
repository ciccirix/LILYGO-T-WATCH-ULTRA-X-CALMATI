#pragma once
// Self-contained debug log for the vendorized ANCS lib. On the moto display
// this pushed lines to an on-screen overlay; on the T-Watch we just forward to
// the serial monitor (header-only so the lib carries its own implementation and
// doesn't depend on the host project's src/).
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void dbg_log(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.println(buf);
}

static inline bool dbg_pop(char *out, int cap)
{
    (void)out; (void)cap;
    return false;   // no on-screen overlay on the watch
}

#ifdef __cplusplus
}
#endif
