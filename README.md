# esp-fire-try-me

A tiny ESP32 timer that fires **two animatronics' Try-Me triggers together** on a
nightly Halloween schedule.

## Schedule (local EST/EDT)

| Time | Cadence |
|------|---------|
| 7:00pm – 9:59pm | fire ~every **1 minute** (+0–8s jitter) |
| 10:00pm – 1:59am | fire every **5 minutes** (+jitter) |
| 2:00am – 7:00pm | idle (power is cut ~2am anyway) |

Time comes from **NTP over WiFi** and auto-handles daylight saving (EDT on
Halloween). Once synced, the ESP's own clock keeps running, so a WiFi hiccup
won't stop the schedule. If it can't get the time at all (no WiFi), it stays
idle rather than firing blindly.

## Wiring

Each animatronic's Try-Me goes through its own **LR7843 opto-isolated MOSFET
module** (same part used on the Frankenstein lights board — isolates the ESP
from the prop's ~4.7V trigger line):

```
ESP GPIO 25 ──▶ module 1 signal-in     ESP GND ──▶ module 1 signal-GND
ESP GPIO 26 ──▶ module 2 signal-in     ESP GND ──▶ module 2 signal-GND
prop 1 Try-Me (two wires) ──▶ module 1 load terminals
prop 2 Try-Me (two wires) ──▶ module 2 load terminals
```

A pulse (`PULSE_MS`, 500ms) turns the MOSFET on = the prop's button is "pressed."
Both pins pulse at the same instant, so both props fire together.

- If a prop's Try-Me is polarized, swap its two load wires if it doesn't trigger.
- Using an **active-low relay module** instead of the LR7843? Set
  `TRIGGER_ON = LOW` in `main.cpp`.

## Build & flash

```
cp src/secrets.h.example src/secrets.h   # then fill in WiFi
pio run -t upload
pio device monitor          # 115200 — shows sync + fires; type 'f' to test-fire
```

## Tuning (top of `src/main.cpp`)

- `EVENING_MS` / `LATE_MS` — the two cadences
- The hour boundaries (7pm/10pm/2am) are in `loop()`
- `PULSE_MS` — how long each button is held
- `TRY_PIN_A` / `TRY_PIN_B` — the two trigger pins
