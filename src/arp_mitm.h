#pragma once
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// ARP MitM / group-block engine (T-Watch Ultra, ESP32-S3).
//
// The watch associates to a WiFi network as a station, then injects raw
// Ethernet+ARP frames over that link (esp_wifi_internal_tx) and sniffs the
// replies (esp_wifi_internal_reg_rxcb) to map the local /24 (IP -> MAC, gateway
// included). A background task then produces one of two effects:
//
//   * MitM one host : poison victim<->gateway. With no IP forwarding this is an
//                     intercept/drop (targeted cut); pair with a sniffer to look.
//   * Group block   : poison every discovered host + the gateway -> the whole
//                     /24 loses its route and goes dark. Heals on stop.
//
// ⚠️ Authorised use only: this is an active L2 attack. Your own / lab networks,
//    or an engagement with written scope. On APs with client isolation the
//    station->station frames may not be relayed (MitM between two clients can
//    fail; the cut toward the gateway usually still works).
// ---------------------------------------------------------------------------

#define ARP_MAX_HOSTS 64
#define ARP_TARGET_ALL (-2)      // pass to arp_mitm_start() for a whole-/24 block

// Network readiness, polled by the screen.
enum ArpNet { ARP_NET_CONNECTED, ARP_NET_CONNECTING, ARP_NET_NONE };

// Kick things off: if already associated, load context and return CONNECTED;
// else try the saved WiFi creds and return CONNECTING (or NONE if we have none).
ArpNet arp_mitm_prepare();
// Poll while CONNECTING; loads context the moment it sees the link come up.
ArpNet arp_mitm_poll_net();
// "SSID  10.0.0.42  gw .1" into out (for the status line).
void arp_mitm_net_info(char *out, int out_sz);

void arp_mitm_request_sweep();   // async ARP sweep of the /24 (runs in the task)
bool arp_mitm_sweeping();
bool arp_mitm_sweep_done();

int  arp_mitm_host_count();
// Copy entry i (ip4 = last octet of the /24). Returns false if i is out of range.
bool arp_mitm_host(int i, uint8_t *ip4, uint8_t mac[6], bool *is_gw);
void arp_mitm_prefix(uint8_t out3[3]);

bool     arp_mitm_start(int target);   // host index, or ARP_TARGET_ALL
void     arp_mitm_stop();               // stop + heal + restore radio
bool     arp_mitm_is_running();
uint32_t arp_mitm_cycles();
