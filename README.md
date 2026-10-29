# Bambu Poop Conveyor (ESP32)

Firmware for an ESP32 that runs a two-motor conveyor to carry away purge waste ("poop") from a Bambu Lab 3D printer.

The conveyor starts when:

- the printer purges (detected over MQTT),
- a motion sensor fires,
- a physical button is pressed, or
- a web / Home Assistant call hits the `/run` endpoint.

A 128×32 OLED, three status LEDs and a built-in web UI show and control what is happening.

- **Version:** 1.6
- **Sketch:** `Bambu-poop-conveyor-UI-v6.ino`
- **Board:** ESP32 (Arduino-ESP32 core 3.x required)

---

## Table of Contents

1. [Hardware and Wiring](#1-hardware-and-wiring)
2. [Libraries and Build Notes](#2-libraries-and-build-notes)
3. [First-Time Setup](#3-first-time-setup)
4. [Trigger Sources](#4-trigger-sources)
5. [Motor Sequence](#5-motor-sequence)
6. [Status LEDs](#6-status-leds)
7. [OLED Screens](#7-oled-screens)
8. [Buttons](#8-buttons)
9. [Web Endpoints](#9-web-endpoints)
10. [MQTT / Bambu Printer Integration](#10-mqtt--bambu-printer-integration)
11. [Persistent Settings](#11-persistent-settings)
12. [Logging](#12-logging)
13. [Code Map](#13-code-map)
14. [Known Issues and Security Notes](#14-known-issues-and-security-notes)
15. [Change History](#change-history)

---

## 1. Hardware and Wiring

| Part | Details |
|---|---|
| Board | ESP32 (uses `ledcAttachChannel`, so core 3.x is required) |
| Motor driver | L298N dual H-bridge driving two DC motors |
| Display | SSD1306 OLED, 128×32, I2C address `0x3C` |
| Sensor | Digital motion sensor (optional, HIGH = motion) |
| Inputs | Two momentary push buttons to GND (internal pull-ups used) |

### Pin map

| Signal | GPIO | Notes |
|---|:---:|---|
| OLED SDA | 5 | I2C data |
| OLED SCL | 13 | I2C clock |
| Green LED | 19 | Ready / WiFi connected |
| Yellow LED | 18 | Waiting / setup / MQTT problem |
| Red LED | 4 | Motor running / MQTT connect failed |
| Motion sensor | 22 | Only used when "Use motion sensor" is on |
| Manual trigger button | 12 | `INPUT_PULLUP`, LOW = pressed |
| Reset (AP mode) button | 2 | `INPUT_PULLUP`, LOW = pressed, hold 3 s |
| Motor 1 IN1 / IN2 | 23 / 21 | L298N IN1 / IN2 |
| Motor 1 enable (PWM) | 15 | L298N ENA |
| Motor 2 IN1 / IN2 | 16 / 17 | L298N IN3 / IN4 |
| Motor 2 enable (PWM) | 14 | L298N ENB |

### Motor behavior

- Both motors always turn in the same direction (Forward or Reverse, configurable).
- Each motor has its own PWM speed (0–255). Defaults: motor 1 = **240**, motor 2 = **140**.
- PWM runs at 5 kHz, 8-bit resolution.

---

## 2. Libraries and Build Notes

**Built into the ESP32 core:** WiFi, WebServer, WiFiClientSecure, Preferences, Update, DNSServer, Wire, time.

**Install from Library Manager:**

- PubSubClient
- ArduinoJson (**v6 API** – uses `DynamicJsonDocument`)
- Adafruit GFX Library
- Adafruit SSD1306

**Notes**

- The MQTT buffer is set to 40,000 bytes (`client.setBufferSize`) because Bambu `pushall` reports are large. Pick a board/partition with enough free heap.
- Each incoming MQTT message allocates a 40,000-byte JSON document.

---

## 3. First-Time Setup

On boot the firmware loads saved settings. If no WiFi SSID and password are saved (or WiFi fails to connect within 25 s), it starts a setup access point:

| Item | Value |
|---|---|
| SSID | `BambuConveyor` |
| Password | `12345678` |
| Captive portal | Any URL redirects to `http://192.168.4.1/config` |

### Steps

1. Connect to the `BambuConveyor` WiFi network.
2. Open the config page (it should open automatically) and fill in:
   - WiFi SSID and password
   - Bambu printer IP address
   - Bambu printer access code (LAN mode access code from the printer screen)
   - Bambu printer serial number
   - Printer model (X1, P1 or A1)
   - Motor timing, speed and direction
   - Optional: motion sensor mode, debug mode
3. Press **Save settings and reboot**. The ESP32 stores the values and restarts.

### Getting back into AP mode

Hold the reset button (GPIO 2) for 3 seconds. The OLED shows a countdown and progress bar while you hold it. Saved settings are kept; only the WiFi/MQTT connection is dropped and the setup AP is started.

---

## 4. Trigger Sources

Any trigger starts the motor sequence, but only if the system is idle (not waiting, not running, not in the post-run delay).

### a) Printer event over MQTT (default, motion sensor off)

Fires when **any** of these is true:

- `stg_cur == 4` (changing filament)
- `stg_cur == 14` (cleaning nozzle tip)
- `mc_print_sub_stage == 4` and `stg_cur != -1`

For the sub-stage case, an extra 75 s is added to the wait time (`additionalWaitTime`) so the purge lands on the conveyor before it starts.

### b) Motion sensor

When "Use motion sensor" is enabled, MQTT-based detection is disabled and a HIGH on GPIO 22 starts the sequence. Automatic `pushall` polling is also disabled.

### c) Manual trigger button (GPIO 12)

Debounced (50 ms). Skips the wait time and starts the motors immediately. Manual runs also skip the post-run delay.

### d) Web / Home Assistant

`GET /run`, or the **Activate motor** button on `/control`.

---

## 5. Motor Sequence

```
Idle
  │  trigger (MQTT / motion / web)
  ▼
Waiting          motorWaiting        motorWaitTime + additionalWaitTime (ms)
  │
  ▼
Running          motorRunning        motorRunTime (ms), both motors on
  │
  ▼
Delay after      delayAfterRunning   delayAfterRun (ms), triggers ignored
  │
  ▼
Idle
```

- The manual button skips *Waiting* and *Delay after* (Idle → Running → Idle).
- Defaults (ms): run **10000**, wait **5000**, delay after run **120000**.
- All timing is non-blocking (`millis()` based), so the web server and MQTT keep working.
- Stopping a motor sets its IN pins LOW and its PWM enable duty to 0.

---

## 6. Status LEDs

| LED | State | Meaning |
|---|---|---|
| Green | Solid | WiFi connected / idle and ready (off while waiting to run) |
| Yellow | Blinking | WiFi connecting; waiting to start the motor; MQTT disconnected 5 s+ |
| Yellow | Solid | AP (setup) mode |
| Red | Solid | Motor running, or MQTT connection attempt failed |
| All | Off | During reset to AP mode |

---

## 7. OLED Screens

| Screen | Content |
|---|---|
| Boot | "Poop Conveyor / Booting..." |
| WiFi | "Connecting...", then the IP address, or a failure warning |
| AP mode | "AP Mode" and the AP IP address |
| MQTT | "MQTT Connected to Bambu printer" (5 s on boot/reconnect) or a connection failure |
| Event detected | "Printer event detected / Waiting..." (MQTT), "Motion sensor event detected / Waiting..." (motion), "Motor / Manual trigger" (`/run`) |
| Motor running | Conveyor icon, "Running!", big seconds countdown, progress bar |
| Motor stopped | "Motor stopped / DONE" with check icon |
| Delay complete | "Delay after run complete" for 2 s, then idle info screens |
| Idle info (MQTT) | Rotates every 4 s (see below) |
| Idle (motion mode) | "Using Motion Sensor Mode", refreshed every 5 s |
| Reset hold | "Reset to AP:" with countdown and progress bar |

**Idle info pages (MQTT mode)**

- Page 1: state + percent, time remaining, progress bar
- Page 2: job name (21 chars max) and layer count. Only appears while a job is printing, paused or preparing.
- "Waiting for data" shows until the first printer report.
- Not used in motion sensor mode.

**Display details**

- The display flashes (inverted for 120 ms) when the motor starts.
- `oledShow()` picks a large font for one short line (10 chars or fewer), centers text, and can draw a 16×16 icon on the left.
- Icons are PROGMEM bitmaps: `iconConveyor`, `iconMotor`, `iconCheck`, `iconWarn`.

---

## 8. Buttons

| Button | Action |
|---|---|
| Manual trigger (GPIO 12) | Short press runs the conveyor now. Ignored while waiting, running or in the post-run delay. |
| Reset (GPIO 2) | Hold 3 s: stop motors, disconnect MQTT/WiFi, start AP mode. Works at any time. Release early to cancel. |

---

## 9. Web Endpoints

All on port 80.

| Method | Path | Description |
|---|---|---|
| GET | `/` | Config page (same as `/config`). In AP mode, unknown URLs redirect to `/config` (captive portal). |
| GET | `/config` | Settings form. `POST` saves settings and reboots. |
| GET | `/control` | Page with an **Activate motor** button. `POST` starts the sequence. |
| GET | `/run` | Starts the sequence with OLED/LED feedback. Intended for Home Assistant or other automations. |
| GET | `/status` | JSON `{ "motor_running": true\|false }` for Home Assistant polling. |
| GET | `/logs` | Last 200 log entries as an HTML table. |
| POST | `/update` | OTA firmware upload (multipart file field). Reboots on success. |

OTA example:

```bash
curl -F "firmware=@build/Bambu-poop-conveyor-UI-v6.ino.bin" \
     http://<device-ip>/update
```

> **None of these endpoints require authentication.** Keep the device on a trusted network.

---

## 10. MQTT / Bambu Printer Integration

| Item | Value |
|---|---|
| Connection | TLS on port 8883 (certificate validation off: `setInsecure()`) |
| Client ID | `BambuConveyor` |
| User / password | `bblp` / printer access code |
| Subscribe | `device/<serial>/report` |
| Publish | `device/<serial>/request` |

Pushall request payload:

```json
{"pushing":{"sequence_id":N,"command":"pushall"},"user_id":"2222222"}
```

**Pushall timing**

- Sent once on connect, then every 30 s for X1 or every 5 min for P1/A1 (not sent in motion sensor mode).
- Can be overridden on the config page ("Printer status request interval", 5–3600 s, 0 = auto).

**Parsed fields**

- Triggers: `print.stg_cur` (stage) and `print.mc_print_sub_stage` (sub stage)
- OLED info screens: `print.gcode_state`, `mc_percent`, `mc_remaining_time`, `layer_num`, `total_layer_num`, `subtask_name`
- Reports can be partial, so only fields that are present are updated.

**Reconnect:** if disconnected, retries every 15 s (`RECONNECT_INTERVAL`). After 5 s the yellow LED starts blinking.

**Stage names:** `getStageInfo()` maps stage numbers to readable names (-100 = connection issue, -1 = idle, 0 = printing, 4 = changing filament, 14 = cleaning nozzle tip, etc.).

---

## 11. Persistent Settings

Stored with Preferences, namespace `my-config`.

| Key | Type | Meaning |
|---|---|---|
| `ssid`, `password` | string | WiFi credentials |
| `mqtt_server` | string | Printer IP address |
| `mqtt_password` | string | Printer access code |
| `serial_number` | string | Printer serial number |
| `printer_model` | string | X1, P1 or A1 (controls pushall interval) |
| `motorRunTime` | int | Run duration (ms) |
| `motorWaitTime` | int | Wait before running (ms) |
| `delayAfterRun` | int | Lock-out after a run (ms) |
| `useMotionSensor` | bool | Motion sensor mode instead of MQTT detection |
| `motorDirection` | int | 0 = Forward, 1 = Reverse |
| `dutyCycle` | int | Motor 1 PWM (0–255) |
| `dutyCycle2` | int | Motor 2 PWM (0–255) |
| `debug` | bool | Verbose serial and log output |
| `pushIntervalSec` | int | Pushall interval in seconds (0 = auto by printer model) |
| `gmtOffset_sec` | int | Timezone offset in **hours** (despite the name), default -6 |

---

## 12. Logging

- `addLogEntry()` stores the last 200 events in a RAM ring buffer with a timestamp. View them at `/logs`.
- Logs are lost on reboot.
- Timestamps need NTP (`pool.ntp.org`), synced once after WiFi connects.
- Debug mode adds more log lines and serial output (115200 baud) but reduces performance.

---

## 13. Code Map

| Area | Functions |
|---|---|
| OLED helpers | `initOLED`, `oledShow`, `drawDigitalNumber`, `drawProgressBar`, `flashScreen`, `startMotorUI` |
| Settings / globals | WiFi and MQTT credentials, pins, timing, PWM, state flags |
| Utilities | `getStageInfo`, `addLogEntry`, `syncTime`, `formatDateTime` |
| Web handlers | `handleConfig`, `handleControl`, `handleManualRun`, `handleMotorStatus`, `handleLogs`, `handleFirmwareUpload`, `handleRoot` |
| MQTT | `mqttCallback`, `connectToMqtt`, `publishPushAllMessage`, `sendPushAllCommand` |
| Idle info screens | `updateInfoScreen`, `drawProgressPage`, `drawJobPage`, `gcodeStateLabel` |
| WiFi | `connectToWiFi`, `startWiFiAPMode`, `resetToAPMode` |

**`setup()`:** I2C scan, pin and PWM init, load settings, OLED, WiFi/AP, MQTT, web routes.

**`loop()`:** web/DNS → reset button → (stop here if AP mode) → manual button → motion sensor → auto pushall → motor state machine and OLED countdown → post-run screen → LED blinking → MQTT reconnect.

---

## 14. Known Issues and Security Notes

These come from a code review.

### Fixed in this version

- **Timezone:** the config page now has a "Timezone offset (hours)" field, so saving no longer resets it to 0. The value is clamped to -12..14.
- **Defaults:** globals and the Preferences fallbacks now agree (run 10000 ms, debug off).
- **Config POST** uses `strlcpy()` with buffer sizes, so long inputs are truncated instead of overflowing memory. Motor speeds are clamped to 0–255.
- Config page hover colors had a double `#` (invalid CSS).
- Motor stop now also sets the PWM enable duty to 0.
- The post-run screen no longer returns early, so LEDs and MQTT keep running.
- `POST /control` now behaves like `/run` (resets extra wait, LEDs, OLED message).
- The MQTT port setting (`mqtt_port`, default 8883) is now actually used.

### Still open

- `daylightOffset_sec` (3600) is always applied, so the clock may be 1 hour ahead outside daylight saving time. Left unchanged to avoid changing deployed behavior.

### Security

- No authentication on any web endpoint, including `/update` (OTA) and `/config`.
- The config page pre-fills the saved WiFi password and printer access code, and contains hard-coded access code and serial number options in the dropdowns (`handleConfig`). **Remove them before sharing the source.**
- MQTT TLS certificate is not verified (`setInsecure()`).
- The setup AP uses a fixed, well-known password (`12345678`).

### Hardware

- GPIO 2, 5, 12 and 15 are ESP32 strapping pins. Buttons or loads on them can interfere with boot on some boards (especially GPIO 12 pulled HIGH at reset). Wiring is already deployed, so the pins are intentionally unchanged.

---

## Change History

- Patched OLED UI and motor countdown (by Marcos): improved OLED UI, icons, countdown, progress bar, screen flash on motor start
- Added a button for manual trigger
- Added reset to AP mode by holding a button for 3 seconds
- Added OLED messages when using motion sensor mode