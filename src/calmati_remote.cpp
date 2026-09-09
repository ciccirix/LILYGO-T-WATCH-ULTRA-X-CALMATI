#include "calmati_remote.h"
#include "wifi_creds.h"
#include <WiFi.h>
#include <HTTPClient.h>

#define CALMATI_SSID     "CALMATI"
#define CALMATI_PASS_DEF "c4lm4t1@root"   // fallback if not in wifi.txt/NVS
#define CALMATI_HOST     "http://192.168.4.1"

const char* calmati_ap_ssid() { return CALMATI_SSID; }

bool calmati_is_connected() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.SSID() == CALMATI_SSID;
}

bool calmati_connect(uint32_t timeout_ms) {
  if (calmati_is_connected()) return true;

  char pass[64];
  if (!wifi_creds_pass_for(CALMATI_SSID, pass, sizeof(pass)) || pass[0] == '\0')
    strncpy(pass, CALMATI_PASS_DEF, sizeof(pass));

  WiFi.mode(WIFI_STA);
  WiFi.begin((const char *)CALMATI_SSID, (const char *)pass);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeout_ms)
    delay(120);

  return WiFi.status() == WL_CONNECTED;
}

// Minimal percent-encoding for a command going into a query string. Encodes the
// characters that actually appear in Marauder commands (space, and a few others)
// and passes through the rest.
static String url_encode(const char* s) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  for (const char* p = s; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

static bool http_get(const String& url, String& out) {
  HTTPClient http;
  if (!http.begin(url)) return false;
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  int code = http.GET();
  bool ok = (code == 200);
  out = ok ? http.getString() : (String("HTTP ") + code);
  http.end();
  return ok;
}

bool calmati_send(const char* cmd, String& out) {
  if (!calmati_connect()) { out = "no AP"; return false; }
  String url = String(CALMATI_HOST) + "/cmd?c=" + url_encode(cmd);
  return http_get(url, out);
}

bool calmati_status(String& out) {
  if (!calmati_connect()) { out = "no AP"; return false; }
  return http_get(String(CALMATI_HOST) + "/status", out);
}

bool calmati_aps(String& out) {
  if (!calmati_connect()) { out = "no AP"; return false; }
  return http_get(String(CALMATI_HOST) + "/aps", out);
}

bool calmati_get(const char* path, String& out) {
  if (!calmati_connect()) { out = "no AP"; return false; }
  return http_get(String(CALMATI_HOST) + path, out);
}
