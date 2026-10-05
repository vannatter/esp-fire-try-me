// esp-fire-try-me
//
// Fires two animatronics' Try-Me triggers TOGETHER on a nightly schedule:
//   * 7:00pm - 9:59pm  ->  ~every 1 minute
//   * 10:00pm - 1:59am ->  every 5 minutes
//   * otherwise (2am - 7pm) -> idle
// Power is cut to the ESP + props ~2am, so there's nothing to do after that.
//
// Time comes from NTP over WiFi (auto-handles EST/EDT). The ESP's clock keeps
// running after the first sync, so a WiFi drop won't stop the schedule.
//
// Wiring: each animatronic's Try-Me goes through its own LR7843 opto-isolated
// MOSFET module (same part used on the Frankenstein lights board):
//   ESP TRY_PIN_A -> module 1 signal in,  ESP GND -> module 1 signal GND
//   ESP TRY_PIN_B -> module 2 signal in,  ESP GND -> module 2 signal GND
//   each prop's two Try-Me wires -> that module's load terminals
// A pulse closes the prop's button for PULSE_MS. Both fire at the same instant.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>

#include "secrets.h"   // WIFI_SSID / WIFI_PASSWORD

// ---- config ----------------------------------------------------------------
static const int TRY_PIN_A = 25;          // animatronic 1 -> LR7843 signal in
static const int TRY_PIN_B = 26;          // animatronic 2 -> LR7843 signal in
static const int TRIGGER_ON = HIGH;       // LR7843 is active-HIGH; set LOW for an
                                          // active-low relay module instead
static const uint32_t PULSE_MS   = 500;   // how long to "hold" each button
static const uint32_t STAGGER_MS = 250;   // when firing both, delay prop 2 after
                                          // prop 1 (prop 2 sometimes missed when
                                          // both triggered at the same instant)
static const uint32_t EVENING_MS = 60000; // 7-10pm  -> ~1 min
static const uint32_t LATE_MS    = 300000;// 10pm-2am -> 5 min
static const uint32_t JITTER_MS  = 8000;  // +0..8s random, so it isn't robotic

// EST with US daylight-saving rules (EDT in effect on Halloween).
static const char* TZ_STR = "EST5EDT,M3.2.0,M11.1.0";
static const char* NTP1 = "pool.ntp.org";
static const char* NTP2 = "time.nist.gov";

// Static IP so the Frankenstein dashboard can reach /fire at a fixed address.
// (Espressif chip — static holds fine. Quiet 71.x range, like boards 2/3.)
#define STATIC_IP   192, 168, 71, 204
#define STATIC_GW   192, 168, 68, 1
#define STATIC_MASK 255, 255, 252, 0
#define STATIC_DNS  8, 8, 8, 8
static WebServer server(80);
// ----------------------------------------------------------------------------

// ---- runtime schedule (editable from the dashboard, saved in flash) --------
// Survives the nightly power-cut via NVS, so it isn't lost at each 6pm boot.
// Hours are 0-23 (0 = midnight). The constants above are just the defaults.
struct Sched {
    bool     armed;        // master on/off for autonomous firing
    uint8_t  startHour;    // begin firing (evening cadence)
    uint8_t  lateHour;     // switch to the slower "late" cadence
    uint8_t  stopHour;     // go idle (no firing from here on)
    uint32_t eveningMs;    // evening cadence
    uint32_t lateMs;       // late cadence
};
static Sched g;
static Preferences prefs;

static void loadSched() {
    prefs.begin("tryme", true);
    g.armed     = prefs.getBool ("armed", true);
    g.startHour = prefs.getUChar("start", 19);   // 7pm
    g.lateHour  = prefs.getUChar("late",  22);   // 10pm
    g.stopHour  = prefs.getUChar("stop",   0);   // midnight
    g.eveningMs = prefs.getUInt ("evms",  EVENING_MS);
    g.lateMs    = prefs.getUInt ("latms", LATE_MS);
    prefs.end();
}
static void saveSched() {
    prefs.begin("tryme", false);
    prefs.putBool ("armed", g.armed);
    prefs.putUChar("start", g.startHour);
    prefs.putUChar("late",  g.lateHour);
    prefs.putUChar("stop",  g.stopHour);
    prefs.putUInt ("evms",  g.eveningMs);
    prefs.putUInt ("latms", g.lateMs);
    prefs.end();
}

// Is hour h within [start, end) on a clock that may wrap past midnight?
static bool hourInRange(int h, int start, int end) {
    if (start == end) return false;
    if (start <  end) return h >= start && h < end;
    return h >= start || h < end;     // window crosses midnight (e.g. 22->2)
}
// ----------------------------------------------------------------------------

// Independent pulse timers per pin so the two props can be staggered (0 = idle).
static uint32_t aOffAt = 0;        // release prop 1 at this time
static uint32_t bOnAt  = 0;        // start prop 2 at this time (staggered)
static uint32_t bOffAt = 0;        // release prop 2 at this time
static uint32_t nextFireAt = 0;    // when to fire next (0 = fire on next active tick)

static void firePins(bool a, bool b) {
    uint32_t now = millis();
    if (a) { digitalWrite(TRY_PIN_A, TRIGGER_ON); aOffAt = now + PULSE_MS; }
    if (b) {
        if (a) {
            bOnAt = now + STAGGER_MS;           // both: fire prop 2 a beat later
        } else {
            digitalWrite(TRY_PIN_B, TRIGGER_ON); // prop 2 only: fire immediately
            bOffAt = now + PULSE_MS;
        }
    }
    Serial.printf("FIRE -> prop1=%d prop2=%d\n", a, b);
}
static void fireBoth() { firePins(true, true); }

// Drive the per-pin pulse state machine; call every loop.
static void servicePulses(uint32_t now) {
    if (bOnAt && (int32_t)(now - bOnAt) >= 0) {   // staggered prop-2 start
        digitalWrite(TRY_PIN_B, TRIGGER_ON);
        bOffAt = now + PULSE_MS;
        bOnAt = 0;
    }
    if (aOffAt && (int32_t)(now - aOffAt) >= 0) { digitalWrite(TRY_PIN_A, !TRIGGER_ON); aOffAt = 0; }
    if (bOffAt && (int32_t)(now - bOffAt) >= 0) { digitalWrite(TRY_PIN_B, !TRIGGER_ON); bOffAt = 0; }
}

// /fire            -> both props
// /fire?which=1    -> prop 1 only
// /fire?which=2    -> prop 2 only
static void handleFire() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    String w = server.hasArg("which") ? server.arg("which") : "both";
    if (w == "1")      firePins(true, false);
    else if (w == "2") firePins(false, true);
    else               firePins(true, true);
    server.send(200, "application/json", "{\"fired\":\"" + w + "\"}\n");
}

static String schedJson() {
    char buf[220];
    snprintf(buf, sizeof(buf),
        "{\"armed\":%d,\"start\":%u,\"late\":%u,\"stop\":%u,"
        "\"eveningMin\":%lu,\"lateMin\":%lu}\n",
        g.armed ? 1 : 0, g.startHour, g.lateHour, g.stopHour,
        (unsigned long)(g.eveningMs / 60000UL), (unsigned long)(g.lateMs / 60000UL));
    return String(buf);
}

// GET /schedule                       -> current settings as JSON
// GET /schedule?armed=0/1&start=19&late=22&stop=0&evmin=1&latmin=5  -> set+save
static void handleSchedule() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    bool changed = false;
    if (server.hasArg("armed"))  { g.armed     = server.arg("armed") == "1"; changed = true; }
    if (server.hasArg("start"))  { g.startHour = constrain(server.arg("start").toInt(), 0, 23); changed = true; }
    if (server.hasArg("late"))   { g.lateHour  = constrain(server.arg("late").toInt(),  0, 23); changed = true; }
    if (server.hasArg("stop"))   { g.stopHour  = constrain(server.arg("stop").toInt(),  0, 23); changed = true; }
    if (server.hasArg("evmin"))  { g.eveningMs = (uint32_t)max(1L, server.arg("evmin").toInt())  * 60000UL; changed = true; }
    if (server.hasArg("latmin")) { g.lateMs    = (uint32_t)max(1L, server.arg("latmin").toInt()) * 60000UL; changed = true; }
    if (changed) saveSched();
    server.send(200, "application/json", schedJson());
}

static bool timeValid() {
    struct tm t;
    if (!getLocalTime(&t, 0)) return false;
    return t.tm_year > (2020 - 1900);
}

static void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.config(IPAddress(STATIC_IP), IPAddress(STATIC_GW),
                IPAddress(STATIC_MASK), IPAddress(STATIC_DNS));
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("WiFi connecting");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(250);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED)
        Serial.println(" connected: http://" + WiFi.localIP().toString() + "/");
    else
        Serial.println(" (no WiFi yet)");
}

void setup() {
    Serial.begin(115200);
    pinMode(TRY_PIN_A, OUTPUT); digitalWrite(TRY_PIN_A, !TRIGGER_ON);
    pinMode(TRY_PIN_B, OUTPUT); digitalWrite(TRY_PIN_B, !TRIGGER_ON);

    loadSched();   // restore the schedule saved before the last power-cut

    connectWiFi();
    configTzTime(TZ_STR, NTP1, NTP2);
    Serial.print("waiting for NTP time");
    uint32_t start = millis();
    while (!timeValid() && millis() - start < 15000) { delay(250); Serial.print("."); }
    if (timeValid()) {
        struct tm t; getLocalTime(&t);
        Serial.printf(" synced: %02d:%02d\n", t.tm_hour, t.tm_min);
    } else {
        Serial.println(" NOT synced yet (will keep retrying)");
    }
    Serial.println("Serial 'f' = manual fire test.");

    server.on("/fire", handleFire);
    server.on("/schedule", handleSchedule);
    server.on("/", []() {
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(200, "text/plain",
            "esp-fire-try-me\n/fire ?which=1|2|both\n"
            "/schedule ?armed=0/1&start=19&late=22&stop=0&evmin=1&latmin=5\n");
    });
    server.enableCORS(true);
    server.begin();
}

// Current hour (0-23). Prefers real NTP time; if time was never obtained (no
// WiFi), falls back to assuming power came on at 6pm EST -- power hits the ESP
// and props together at 6pm, so uptime advances the virtual clock from 18:00.
static int currentHour() {
    struct tm t;
    if (getLocalTime(&t, 0) && t.tm_year > (2020 - 1900)) return t.tm_hour;
    uint32_t vmin = (18 * 60UL + millis() / 60000UL) % (24 * 60UL);  // boot = 18:00
    return (int)(vmin / 60);
}

// Current firing cadence, or 0 (idle), per the editable schedule.
static uint32_t scheduleInterval() {
    if (!g.armed) return 0;
    int h = currentHour();
    if (hourInRange(h, g.startHour, g.lateHour)) return g.eveningMs;  // evening
    if (hourInRange(h, g.lateHour,  g.stopHour)) return g.lateMs;     // late
    return 0;                                                          // idle
}

void loop() {
    uint32_t now = millis();
    server.handleClient();

    // Drive the staggered per-pin pulses.
    servicePulses(now);

    // Manual test.
    if (Serial.available() && Serial.read() == 'f') fireBoth();

    // Keep trying for real time in the background (non-blocking) if not synced,
    // so the schedule snaps to true time if the network ever comes up. Meanwhile
    // scheduleInterval() runs the assume-6pm fallback -- firing never stalls.
    if (!timeValid()) {
        static uint32_t retry = 0;
        if (now - retry > 60000) {
            retry = now;
            if (WiFi.status() != WL_CONNECTED) WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            configTzTime(TZ_STR, NTP1, NTP2);
        }
    }

    uint32_t interval = scheduleInterval();
    if (interval == 0) {
        nextFireAt = 0;                                  // idle window
    } else if (nextFireAt == 0 || (int32_t)(now - nextFireAt) >= 0) {
        fireBoth();
        nextFireAt = now + interval + (uint32_t)random(0, JITTER_MS);
    }
    delay(20);
}
