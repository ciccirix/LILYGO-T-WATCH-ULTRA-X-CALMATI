#include "webpanel.h"
#include "console.h"
#include "adsb.h"
#include "mic_rec.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>

#define AP_SSID  "13-37-PANEL"
#define AP_PASS  "1337panel"          // WPA2, min 8 chars — keep the panel private

static WebServer *s_web    = nullptr;
static bool       s_running = false;

// ---- dashboard page (self-contained, dark, polls /api/status) --------------

static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>13:37 panel</title><style>
body{background:#000;color:#0f0;font-family:monospace;margin:0;padding:14px}
h1{font-size:18px;color:#0f0;margin:0 0 10px}
.card{border:1px solid #060;border-radius:8px;padding:10px;margin:8px 0}
button{background:#000;color:#0c0;border:1px solid #0a0;border-radius:18px;
 padding:9px 14px;font-family:monospace;font-size:14px;margin:3px}
button:active{background:#020}
pre{white-space:pre-wrap;color:#0c0;margin:6px 0 0}
a{color:#3cf}input{background:#010;color:#0f0;border:1px solid #060;
 border-radius:6px;padding:8px;font-family:monospace;width:60%}
.k{color:#080}
</style></head><body>
<h1>13:37 &middot; control panel</h1>
<div class=card id=stat><pre>loading...</pre></div>
<div class=card>
 <button onclick="cmd('radar on')">radar on</button>
 <button onclick="cmd('radar off')">radar off</button>
 <button onclick="cmd('mic start')">mic start</button>
 <button onclick="cmd('mic stop')">mic stop</button>
 <button onclick="cmd('buzz')">buzz</button>
</div>
<div class=card>
 <input id=ci placeholder="type a command (help)" onkeydown="if(event.key=='Enter')run()">
 <button onclick="run()">run</button>
 <pre id=out></pre>
</div>
<div class=card><b>recordings</b><pre id=recs></pre></div>
<script>
function api(p){return fetch(p).then(r=>r.text())}
function refresh(){api('/api/status').then(t=>{document.querySelector('#stat pre').textContent=t});
 api('/api/recs').then(t=>{document.getElementById('recs').innerHTML=t})}
function cmd(c){api('/api/cmd?c='+encodeURIComponent(c)).then(t=>{document.getElementById('out').textContent=t;refresh()})}
function run(){var c=document.getElementById('ci').value;if(c)cmd(c)}
refresh();setInterval(refresh,2000);
</script></body></html>)HTML";

// ---- handlers --------------------------------------------------------------

static bool gps_ok()
{
    return instance.gps.location.isValid() && instance.gps.location.age() < 5000;
}

static void handle_root()   { s_web->send_P(200, "text/html", PAGE); }

static void handle_status()
{
    String o;
    o += "uptime  "; { uint32_t s = millis() / 1000; char b[24];
        snprintf(b, sizeof(b), "%02u:%02u:%02u", (unsigned)(s/3600),
                 (unsigned)((s/60)%60), (unsigned)(s%60)); o += b; }
    o += "\nheap    "; o += String(ESP.getFreeHeap() / 1024); o += " KB";
    o += "\ntemp    "; o += String(temperatureRead(), 1); o += " C";
    o += "\nsd      "; o += (instance.isCardReady() ? "ready" : "absent");
    o += "\ngps     ";
    if (gps_ok()) { char b[64]; snprintf(b, sizeof(b), "%.5f, %.5f  %u sats",
        instance.gps.location.lat(), instance.gps.location.lng(),
        (unsigned)instance.gps.satellites.value()); o += b; }
    else o += "no fix";
    o += "\nradar   "; o += (adsb_is_running() ? "on" : "off");
    o += "  ["; o += adsb_status_text(); o += "]  ";
    o += String(adsb_count()); o += " ac  "; o += String(adsb_range_nm()); o += "nm";
    o += "\nmic     "; o += mic_rec_status_text();
    if (mic_rec_is_recording()) { o += "  "; o += String(mic_rec_bytes()/1024); o += " KB"; }
    o += "\nclients "; o += String(WiFi.softAPgetStationNum());
    o += "\n";
    s_web->send(200, "text/plain", o);
}

static void handle_cmd()
{
    if (!s_web->hasArg("c")) { s_web->send(400, "text/plain", "missing c"); return; }
    String out;
    console_exec(s_web->arg("c").c_str(), out);
    if (out.length() == 0) out = "ok\n";
    s_web->send(200, "text/plain", out);
}

static void handle_recs()
{
    String o;
    if (!instance.isCardReady()) { s_web->send(200, "text/html", "no SD"); return; }
    File dir = SD.open("/Recordings");
    if (dir) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            // f.name() may be a full path or a bare name depending on core
            // version; normalise to the basename for a clean /dl link.
            String nm = f.name();
            int sl = nm.lastIndexOf('/');
            if (sl >= 0) nm = nm.substring(sl + 1);
            o += "<a href='/dl?f=" + nm + "'>" + nm + "</a>  <span class=k>"
               + String((uint32_t)f.size() / 1024) + " KB</span><br>";
        }
        dir.close();
    }
    if (o.length() == 0) o = "(none)";
    s_web->send(200, "text/html", o);
}

static void handle_dl()
{
    if (!s_web->hasArg("f")) { s_web->send(400, "text/plain", "missing f"); return; }
    String f = s_web->arg("f");
    if (f.indexOf('/') >= 0 || f.indexOf("..") >= 0) {   // no path traversal
        s_web->send(400, "text/plain", "bad name"); return;
    }
    String path = "/Recordings/" + f;
    if (!SD.exists(path)) { s_web->send(404, "text/plain", "not found"); return; }
    File file = SD.open(path, FILE_READ);
    if (!file) { s_web->send(500, "text/plain", "open failed"); return; }
    s_web->sendHeader("Content-Disposition", "attachment; filename=" + f);
    s_web->streamFile(file, "audio/wav");
    file.close();
}

// ---- lifecycle -------------------------------------------------------------

void webpanel_start()
{
    if (s_running) return;

    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(AP_SSID, AP_PASS)) return;

    s_web = new WebServer(80);
    s_web->on("/",           handle_root);
    s_web->on("/api/status", handle_status);
    s_web->on("/api/cmd",    handle_cmd);
    s_web->on("/api/recs",   handle_recs);
    s_web->on("/dl",         handle_dl);
    s_web->onNotFound(handle_root);
    s_web->begin();

    s_running = true;
}

void webpanel_stop()
{
    if (!s_running) return;
    if (s_web) { s_web->stop(); delete s_web; s_web = nullptr; }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    s_running = false;
}

bool webpanel_is_running() { return s_running; }

void webpanel_tick()
{
    if (s_running && s_web) s_web->handleClient();
}

const char *webpanel_ssid() { return AP_SSID; }
