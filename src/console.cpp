#include "console.h"
#include "adsb.h"
#include "mic_rec.h"
#include "webpanel.h"
#include "tracker_ring.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include <SD.h>
#include <string.h>
#include <strings.h>   // strcasecmp

// ---- tiny helpers ----------------------------------------------------------

static bool eq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

static void fmt_hms(String &out, uint32_t ms)
{
    uint32_t s = ms / 1000;
    char b[24];
    snprintf(b, sizeof(b), "%02u:%02u:%02u",
             (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
    out += b;
}

static bool gps_ok()
{
    return instance.gps.location.isValid() && instance.gps.location.age() < 5000;
}

// ---- individual commands ---------------------------------------------------

static void cmd_ring(String &o)
{
    o += "scanning + ringing nearest tracker (see [RING] log)...\n";
    Serial.print(o);            // flush the notice before the ~14 s blocking op
    int r = tracker_ring_run();
    switch (r) {
        case TR_RING_APPLE:     o = "RESULT: Apple tracker SOUNDED (0xAF)\n"; break;
        case TR_RING_KEYFINDER: o = "RESULT: keyfinder SOUNDED (0x02)\n";     break;
        case TR_RING_NO_SOUND:  o = "RESULT: connected, no sound service\n";  break;
        default:                o = "RESULT: no ring (move closer / not separated)\n"; break;
    }
}

static void cmd_help(String &o)
{
    o += "commands:\n"
         "  help                 this list\n"
         "  status               one-line system summary\n"
         "  sys                  heap / psram / uptime / temp\n"
         "  uptime               time since boot\n"
         "  gps                  position / fix / satellites\n"
         "  wifi                 STA + AP link state\n"
         "  radar on|off         start/stop ADS-B fetch\n"
         "  radar range <nm>     20|40|80|150\n"
         "  radar list           nearest aircraft\n"
         "  mic start|stop       recorder control\n"
         "  mic status           recorder state\n"
         "  recs                 list /Recordings\n"
         "  bright <0-255>       screen brightness\n"
         "  buzz                 test the haptic\n"
         "  ring                 scan + make nearest AirTag/keyfinder sound\n"
         "  panel on|off         this web panel AP\n"
         "  reboot               restart the watch\n";
}

static void cmd_sys(String &o)
{
    o += "heap free : "; o += String(ESP.getFreeHeap() / 1024); o += " KB\n";
    o += "psram free: "; o += String(ESP.getFreePsram() / 1024); o += " KB\n";
    o += "uptime    : "; fmt_hms(o, millis()); o += "\n";
    o += "cpu temp  : "; o += String(temperatureRead(), 1); o += " C\n";
    o += "sd card   : "; o += (instance.isCardReady() ? "ready" : "absent"); o += "\n";
}

static void cmd_gps(String &o)
{
    if (!gps_ok()) {
        o += "gps: no fix";
        if (instance.gps.satellites.isValid()) {
            o += " ("; o += String(instance.gps.satellites.value()); o += " sats)";
        }
        o += "\n";
        return;
    }
    char b[96];
    snprintf(b, sizeof(b), "gps: %.5f, %.5f  alt %.0fm  %.1fkm/h  %u sats\n",
             instance.gps.location.lat(), instance.gps.location.lng(),
             instance.gps.altitude.meters(), instance.gps.speed.kmph(),
             (unsigned)instance.gps.satellites.value());
    o += b;
}

static void cmd_wifi(String &o)
{
    wl_status_t st = WiFi.status();
    o += "sta: ";
    if (st == WL_CONNECTED) {
        o += WiFi.SSID(); o += "  ip "; o += WiFi.localIP().toString();
        o += "  "; o += String(WiFi.RSSI()); o += " dBm\n";
    } else {
        o += "not connected\n";
    }
    o += "ap : ";
    if (webpanel_is_running()) {
        o += webpanel_ssid(); o += "  ip "; o += WiFi.softAPIP().toString();
        o += "  clients "; o += String(WiFi.softAPgetStationNum()); o += "\n";
    } else {
        o += "off\n";
    }
}

static void cmd_radar(char **av, int ac, String &o)
{
    if (ac < 2 || eq(av[1], "status")) {
        o += "radar: "; o += (adsb_is_running() ? "running" : "stopped");
        o += "  ["; o += adsb_status_text(); o += "]  ";
        o += String(adsb_count()); o += " aircraft  range ";
        o += String(adsb_range_nm()); o += "nm\n";
        return;
    }
    if (eq(av[1], "on"))  { adsb_start(); o += "radar started\n"; return; }
    if (eq(av[1], "off")) { adsb_stop();  o += "radar stopped\n"; return; }
    if (eq(av[1], "range") && ac >= 3) {
        int want = atoi(av[2]);
        // Cycle until it matches (small fixed set), then report.
        for (int i = 0; i < 4 && adsb_range_nm() != want; i++) adsb_cycle_range();
        o += "range = "; o += String(adsb_range_nm()); o += "nm\n";
        return;
    }
    if (eq(av[1], "list")) {
        AdsbAircraft ac_arr[12];
        int n = adsb_get(ac_arr, 12);
        if (!n) { o += "no aircraft\n"; return; }
        char b[80];
        for (int i = 0; i < n; i++) {
            snprintf(b, sizeof(b), "%-8s %5dft %3.0fkt  %4.1fnm @ %03.0f\n",
                     ac_arr[i].flight[0] ? ac_arr[i].flight : ac_arr[i].hex,
                     (int)ac_arr[i].alt_ft, ac_arr[i].gs_kt,
                     ac_arr[i].dist_nm, ac_arr[i].bearing_deg);
            o += b;
        }
        return;
    }
    o += "usage: radar on|off|range <nm>|list\n";
}

static void cmd_mic(char **av, int ac, String &o)
{
    if (ac < 2 || eq(av[1], "status")) {
        o += "mic: "; o += mic_rec_status_text();
        if (mic_rec_is_recording()) {
            o += "  "; fmt_hms(o, mic_rec_elapsed_ms());
            o += "  "; o += String(mic_rec_bytes() / 1024); o += " KB";
        }
        o += "\n";
        return;
    }
    if (eq(av[1], "start")) { mic_rec_start(); o += mic_rec_status_text(); o += "\n"; return; }
    if (eq(av[1], "stop"))  { mic_rec_stop();  o += "stopping\n"; return; }
    o += "usage: mic start|stop|status\n";
}

static void cmd_recs(String &o)
{
    if (!instance.isCardReady()) { o += "no SD card\n"; return; }
    File dir = SD.open("/Recordings");
    if (!dir) { o += "no recordings\n"; return; }
    int n = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        o += "  "; o += f.name(); o += "  ";
        o += String((uint32_t)f.size() / 1024); o += " KB\n";
        n++;
    }
    dir.close();
    if (!n) o += "  (empty)\n";
}

static void cmd_status(String &o)
{
    o += "up "; fmt_hms(o, millis());
    o += "  heap "; o += String(ESP.getFreeHeap() / 1024); o += "K";
    o += "  gps "; o += (gps_ok() ? "fix" : "--");
    o += "  radar "; o += (adsb_is_running() ? "on" : "off");
    o += "  mic "; o += (mic_rec_is_recording() ? "REC" : "idle");
    o += "\n";
}

// ---- dispatcher ------------------------------------------------------------

void console_exec(const char *line, String &out)
{
    // Copy + tokenise (max 6 args).
    char buf[128];
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *av[6];
    int   ac = 0;
    char *p = strtok(buf, " \t\r\n");
    while (p && ac < 6) { av[ac++] = p; p = strtok(nullptr, " \t\r\n"); }
    if (ac == 0) return;

    if      (eq(av[0], "help") || eq(av[0], "?")) cmd_help(out);
    else if (eq(av[0], "status"))                 cmd_status(out);
    else if (eq(av[0], "sys"))                    cmd_sys(out);
    else if (eq(av[0], "uptime"))               { fmt_hms(out, millis()); out += "\n"; }
    else if (eq(av[0], "gps"))                    cmd_gps(out);
    else if (eq(av[0], "wifi"))                   cmd_wifi(out);
    else if (eq(av[0], "radar"))                  cmd_radar(av, ac, out);
    else if (eq(av[0], "mic"))                    cmd_mic(av, ac, out);
    else if (eq(av[0], "recs"))                   cmd_recs(out);
    else if (eq(av[0], "bright") && ac >= 2)    { instance.setBrightness(constrain(atoi(av[1]), 0, 255)); out += "ok\n"; }
    else if (eq(av[0], "buzz"))                 { instance.vibrator(); out += "buzz\n"; }
    else if (eq(av[0], "ring"))                   cmd_ring(out);
    else if (eq(av[0], "panel") && ac >= 2)     { if (eq(av[1],"on")) webpanel_start(); else webpanel_stop(); out += "ok\n"; }
    else if (eq(av[0], "reboot"))               { out += "rebooting...\n"; delay(200); ESP.restart(); }
    else                                        { out += "unknown: "; out += av[0]; out += "  (try 'help')\n"; }
}

// ---- USB serial transport --------------------------------------------------

void console_serial_tick()
{
    static char line[128];
    static size_t len = 0;

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (len == 0) continue;
            line[len] = '\0';
            String out;
            console_exec(line, out);
            Serial.print(out);
            len = 0;
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }
}
