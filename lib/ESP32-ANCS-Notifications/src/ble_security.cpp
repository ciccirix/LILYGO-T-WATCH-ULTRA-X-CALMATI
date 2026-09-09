
#include "ble_security.h"

#include "esp_log.h"
#include <Arduino.h>

static char LOG_TAG[] = "NotificationSecurityCallbacks";

extern volatile uint32_t g_pairing_pin;     // definito in ancs_bridge.cpp -> overlay PIN a schermo

uint32_t NotificationSecurityCallbacks::onPassKeyRequest()
{
    // IO_CAP_OUT: l'ESP32 mostra, non inserisce. Non dovrebbe arrivare qui.
    return 0;
}

void NotificationSecurityCallbacks::onPassKeyNotify(uint32_t pass_key)
{
    // Il PIN che l'utente deve digitare sull'iPhone: lo pubblichiamo per l'overlay a schermo.
    g_pairing_pin = pass_key;
    Serial.printf("[SEC] PIN da inserire su iPhone: %06u\n", (unsigned)pass_key);
}

bool NotificationSecurityCallbacks::onSecurityRequest()
{
    return true;
}

bool NotificationSecurityCallbacks::onConfirmPIN(uint32_t)
{
    return true;
}

void NotificationSecurityCallbacks::onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl)
{
    g_pairing_pin   = 0;              // nasconde l'overlay
    ble_auth_set_done(cmpl.success);  // sblocca il task client ANCS (subscribe)
    Serial.printf("[SEC] auth %s\n", cmpl.success ? "OK (bond completato)" : "FALLITA");
}
