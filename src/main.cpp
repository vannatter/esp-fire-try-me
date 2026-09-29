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
#include <time.h>

#include "secrets.h"   // WIFI_SSID / WIFI_PASSWORD

// ---- config ----------------------------------------------------------------
static const int TRY_PIN_A = 25;          // animatronic 1 -> LR7843 signal in
static const int TRY_PIN_B = 26;          // animatronic 2 -> LR7843 signal in
static const int TRIGGER_ON = HIGH;       // LR7843 is active-HIGH; set LOW for an
                                          // active-low relay module instead
static const uint32_t PULSE_MS   = 500;   // how long to "hold" each button
static const uint32_t EVENING_MS = 60000; // 7-10pm  -> ~1 min
static const uint32_t LATE_MS    = 300000;// 10pm-2am -> 5 min
static const uint32_t JITTER_MS  = 8000;  // +0..8s random, so it isn't robotic

// EST with US daylight-saving rules (EDT in effect on Halloween).
static const char* TZ_STR = "EST5EDT,M3.2.0,M11.1.0";
static const char* NTP1 = "pool.ntp.org";
static const char* NTP2 = "time.nist.gov";
// ----------------------------------------------------------------------------

static uint32_t pulseOffAt = 0;   // when to release the current pulse (0 = idle)
static uint32_t nextFireAt = 0;   // when to fire next (0 = fire on next active tick)

static void fireBoth() {
    digitalWrite(TRY_PIN_A, TRIGGER_ON);
    digitalWrite(TRY_PIN_B, TRIGGER_ON);
    pulseOffAt = millis() + PULSE_MS;
    Serial.println("FIRE -> both Try-Me");
}

static bool timeValid() {
    struct tm t;
    if (!getLocalTime(&t, 0)) return false;
    return t.tm_year > (2020 - 1900);
}

static void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("WiFi connecting");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println(WiFi.status() == WL_CONNECTED ? " connected" : " (no WiFi yet)");
}

void setup() {
    Serial.begin(115200);
    pinMode(TRY_PIN_A, OUTPUT); digitalWrite(TRY_PIN_A, !TRIGGER_ON);
    pinMode(TRY_PIN_B, OUTPUT); digitalWrite(TRY_PIN_B, !TRIGGER_ON);

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
}

void loop() {
    uint32_t now = millis();

    // Release a finished pulse.
    if (pulseOffAt && now >= pulseOffAt) {
        digitalWrite(TRY_PIN_A, !TRIGGER_ON);
        digitalWrite(TRY_PIN_B, !TRIGGER_ON);
        pulseOffAt = 0;
    }

    // Manual test.
    if (Serial.available() && Serial.read() == 'f') fireBoth();

    // No valid time yet -> keep trying WiFi/NTP, don't fire.
    if (!timeValid()) {
        static uint32_t retry = 0;
        if (now - retry > 30000) {
            retry = now;
            if (WiFi.status() != WL_CONNECTED) connectWiFi();
            configTzTime(TZ_STR, NTP1, NTP2);
        }
        delay(50);
        return;
    }

    struct tm t;
    getLocalTime(&t);
    int hour = t.tm_hour;

    uint32_t interval;
    if (hour >= 19 && hour < 22)        interval = EVENING_MS;   // 7pm-10pm
    else if (hour >= 22 || hour < 2)    interval = LATE_MS;      // 10pm-2am
    else { nextFireAt = 0; delay(200); return; }                // 2am-7pm idle

    if (nextFireAt == 0 || (int32_t)(now - nextFireAt) >= 0) {
        fireBoth();
        nextFireAt = now + interval + (uint32_t)random(0, JITTER_MS);
    }
    delay(20);
}
