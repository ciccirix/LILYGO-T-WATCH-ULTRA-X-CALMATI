#ifndef BLE_SECURITY_H_
#define BLE_SECURITY_H_

#include "BLESecurity.h"

class NotificationSecurityCallbacks : public BLESecurityCallbacks
{

    uint32_t onPassKeyRequest();

    void onPassKeyNotify(uint32_t pass_key);

    bool onSecurityRequest();

    bool onConfirmPIN(uint32_t);

    void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl);
};

// Stato bonding condiviso via funzioni (evita il constant-folding LTO del volatile cross-TU)
void ble_auth_set_done(bool v);
bool ble_auth_is_done();

#endif // BLE_SECURITY_H_