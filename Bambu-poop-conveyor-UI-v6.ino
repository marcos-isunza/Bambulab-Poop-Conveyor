/*
 * =====================================================================================
 * BAMBU POOP CONVEYOR (ESP32) – README
 * =====================================================================================
 *
 * Overview
 * --------
 * Firmware for an ESP32 that runs a two-motor conveyor to carry away purge waste
 * ("poop") from a Bambu Lab 3D printer. The conveyor starts when the printer purges
 * (detected over MQTT), when a motion sensor fires, when a physical button is pressed,
 * or when a web/Home Assistant call hits the /run endpoint. A 128x32 OLED, three status
 * LEDs and a built-in web UI show and control what is happening.
 *
 * Version: see `version` below (1.6). File name: Bambu-poop-conveyor-UI-v6.ino
 *
 *
 * Table of contents
 * -----------------
 *   1. Hardware and wiring
 *   2. Required libraries and build notes
 *   3. First-time setup (AP mode and config page)
 *   4. Trigger sources
 *   5. Motor sequence (state machine)
 *   6. Status LEDs
 *   7. OLED screens
 *   8. Buttons
 *   9. Web endpoints
 *  10. MQTT / Bambu printer integration
 *  11. Persistent settings (Preferences)
 *  12. Logging
 *  13. Code map
 *  14. Known issues and things to be aware of
 *
 *
 * 1. Hardware and wiring
 * ----------------------
 *   Board      ESP32 (uses ledcAttachChannel, so Arduino-ESP32 core 3.x is required)
 *   Motor driver  L298N dual H-bridge driving two DC motors
 *   Display    SSD1306 OLED, 128x32, I2C address 0x3C
 *   Sensor     Digital motion sensor (optional, HIGH = motion)
 *   Inputs     Two momentary push buttons to GND (internal pull-ups are used)
 *
 *   Signal                    GPIO   Notes
 *   ------------------------  -----  ---------------------------------------------
 *   OLED SDA                  5      I2C data
 *   OLED SCL                  13     I2C clock
 *   Green LED                 19     Ready / WiFi connected
 *   Yellow LED                18     Waiting / setup / MQTT problem
 *   Red LED                   4      Motor running / MQTT connect failed
 *   Motion sensor             22     Only used when "Use motion sensor" is on
 *   Manual trigger button     12     INPUT_PULLUP, LOW = pressed
 *   Reset (AP mode) button    2      INPUT_PULLUP, LOW = pressed, hold 3 s
 *   Motor 1 IN1 / IN2         23 / 21  L298N IN1 / IN2
 *   Motor 1 enable (PWM)      15     L298N ENA
 *   Motor 2 IN1 / IN2         16 / 17  L298N IN3 / IN4
 *   Motor 2 enable (PWM)      14     L298N ENB
 *
 *   Both motors always turn in the same direction (Forward or Reverse, configurable).
 *   Each motor has its own PWM speed (0–255): motor 1 default 240, motor 2 default 140.
 *   PWM runs at 5 kHz, 8-bit resolution.
 *
 *
 * 2. Required libraries and build notes
 * -------------------------------------
 *   Built into the ESP32 core: WiFi, WebServer, WiFiClientSecure, Preferences, Update,
 *   DNSServer, Wire, time.
 *   Install from Library Manager: PubSubClient, ArduinoJson (v6 API – uses
 *   DynamicJsonDocument), Adafruit GFX Library, Adafruit SSD1306.
 *
 *   Notes
 *   - The MQTT buffer is set to 40,000 bytes (client.setBufferSize) because Bambu
 *     "pushall" reports are large. Pick a board/partition with enough free heap.
 *   - Each incoming MQTT message allocates a 40,000 byte JSON document.
 *
 *
 * 3. First-time setup (AP mode and config page)
 * ---------------------------------------------
 *   On boot the firmware loads saved settings. If no WiFi SSID and password are saved
 *   (or WiFi fails to connect in 25 s) it starts a setup access point:
 *
 *     SSID: BambuConveyor        Password: 12345678
 *     Captive portal: any URL redirects to http://192.168.4.1/config
 *
 *   Steps
 *     1. Connect to the BambuConveyor WiFi network.
 *     2. Open the config page (it should open automatically) and fill in:
 *          - WiFi SSID and password
 *          - Bambu printer IP address
 *          - Bambu printer access code (LAN mode access code from the printer screen)
 *          - Bambu printer serial number
 *          - Printer model (X1, P1 or A1)
 *          - Motor timing, speed and direction
 *          - Optional: motion sensor mode, debug mode
 *     3. Press "Save settings and reboot". The ESP32 stores the values and restarts.
 *
 *   To get back into AP mode later, hold the reset button (GPIO 2) for 3 seconds.
 *   The OLED shows a countdown and progress bar while you hold it. Saved settings are
 *   kept; only the WiFi/MQTT connection is dropped and the setup AP is started.
 *
 *
 * 4. Trigger sources
 * ------------------
 *   Any trigger starts the motor sequence, but only if the system is idle (not
 *   waiting, not running, not in the post-run delay).
 *
 *   a) Printer event over MQTT (default mode, motion sensor off)
 *        Fires when ANY of these is true:
 *          - stg_cur == 4   (changing filament)
 *          - stg_cur == 14  (cleaning nozzle tip)
 *          - mc_print_sub_stage == 4 and stg_cur != -1
 *        For the sub-stage case an extra 75 s is added to the wait time so the purge
 *        lands on the conveyor before it starts (additionalWaitTime).
 *
 *   b) Motion sensor (setting "Use motion sensor")
 *        When enabled, MQTT-based detection is disabled and a HIGH on GPIO 22 starts
 *        the sequence. The auto "pushall" polling is also disabled.
 *
 *   c) Manual trigger button (GPIO 12)
 *        Debounced (50 ms). Skips the wait time and starts the motors immediately.
 *        Manual runs also skip the post-run delay.
 *
 *   d) Web/Home Assistant: GET /run, or the "Activate motor" button on /control.
 *
 *
 * 5. Motor sequence (state machine)
 * ---------------------------------
 *   Idle
 *     |  trigger (MQTT / motion / web)
 *     v
 *   Waiting        motorWaiting    for motorWaitTime + additionalWaitTime (ms)
 *     |
 *     v
 *   Running        motorRunning    for motorRunTime (ms); both motors on
 *     |
 *     v
 *   Delay after    delayAfterRunning  for delayAfterRun (ms); triggers ignored
 *     |
 *     v
 *   Idle
 *
 *   The manual button skips "Waiting" and "Delay after" (goes Idle → Running → Idle).
 *
 *   Defaults (ms): run 10000, wait 5000, delay after run 120000.
 *   All timing is non-blocking (millis based) so the web server and MQTT keep working.
 *   Stopping a motor sets its IN pins LOW and its PWM enable duty to 0.
 *
 *
 * 6. Status LEDs
 * --------------
 *   Green   solid  WiFi connected / idle and ready (off while waiting to run)
 *   Yellow  blink  WiFi connecting; waiting to start the motor; MQTT disconnected 5 s+
 *           solid  AP (setup) mode
 *   Red     solid  Motor running, or MQTT connection attempt failed
 *   All off        During reset to AP mode
 *
 *
 * 7. OLED screens
 * ---------------
 *   Boot                 "Poop Conveyor / Booting..."
 *   WiFi                 "Connecting...", then the IP address, or a failure warning
 *   AP mode              "AP Mode" and the AP IP address
 *   MQTT                 "MQTT Connected to Bambu printer" (shown 5 s on boot/reconnect)
 *                        or a connection failure
 *   Event detected       "Printer event detected / Waiting..." (MQTT trigger)
 *                        "Motion sensor event detected / Waiting..." (motion trigger)
 *                        "Motor / Manual trigger" (/run endpoint)
 *   Motor running        Conveyor icon, "Running!", big seconds countdown, progress bar
 *   Motor stopped        "Motor stopped / DONE" with check icon
 *   Delay complete       "Delay after run complete" for 2 s, then the idle info screens
 *   Idle info (MQTT)     Rotates every 4 s while connected and nothing else is happening:
 *                          Page 1: state + percent, time remaining, progress bar
 *                          Page 2: job name (21 chars max) and layer count
 *                        Page 2 only appears while a job is printing/paused/preparing.
 *                        "Waiting for data" shows until the first printer report.
 *                        Not used in motion sensor mode.
 *   Idle (motion mode)   "Using Motion Sensor Mode", refreshed every 5 s
 *   Reset hold           "Reset to AP:" with countdown and progress bar
 *
 *   The display is flashed (inverted for 120 ms) when the motor starts.
 *   oledShow() picks a large font for one short line (10 chars or fewer), centers text
 *   and can draw a 16x16 icon on the left. Icons are PROGMEM bitmaps: iconConveyor,
 *   iconMotor, iconCheck, iconWarn.
 *
 *
 * 8. Buttons
 * ----------
 *   Manual trigger (GPIO 12)  Short press → run the conveyor now.
 *                             Ignored while waiting, running or in the post-run delay.
 *   Reset (GPIO 2)            Hold 3 s → stop motors, disconnect MQTT/WiFi, start AP mode.
 *                             Works at any time. Release early to cancel.
 *
 *
 * 9. Web endpoints (port 80)
 * --------------------------
 *   GET  /         Config page (same as /config). In AP mode, unknown URLs redirect
 *                  to /config (captive portal).
 *   GET  /config   Settings form. POST saves settings and reboots.
 *   GET  /control  Page with an "Activate motor" button. POST starts the sequence.
 *   GET  /run      Starts the sequence with the OLED/LED feedback. Intended for Home
 *                  Assistant or other automations.
 *   GET  /status   JSON: { "motor_running": true|false } for Home Assistant polling.
 *   GET  /logs     Last 200 log entries as an HTML table.
 *   POST /update   OTA firmware upload (multipart file field). Reboots on success, e.g.:
 *                    curl -F "firmware=@build/Bambu-poop-conveyor-UI-v6.ino.bin" \
 *                         http://<device-ip>/update
 *
 *   None of these endpoints require authentication. Keep the device on a trusted network.
 *
 *
 * 10. MQTT / Bambu printer integration
 * ------------------------------------
 *   Connection  TLS on port 8883 (certificate validation is off: setInsecure()).
 *               Client ID "BambuConveyor", user "bblp", password = printer access code.
 *   Subscribe   device/<serial>/report
 *   Publish     device/<serial>/request  {"pushing":{"sequence_id":N,"command":"pushall"},
 *                                         "user_id":"2222222"}
 *   Pushall     Sent once on connect and then every 30 s for X1 or every 5 min for
 *               P1/A1 (not sent in motion sensor mode). Can be overridden on the
 *               config page ("Printer status request interval", 5–3600 s, 0 = auto). It asks the printer for a full
 *               status report.
 *   Parsed      print.stg_cur (stage) and print.mc_print_sub_stage (sub stage) for
 *               triggers; print.gcode_state, mc_percent, mc_remaining_time, layer_num,
 *               total_layer_num and subtask_name for the OLED info screens. Reports
 *               can be partial, so only fields that are present are updated.
 *   Reconnect   If disconnected, retry every 15 s (RECONNECT_INTERVAL). After 5 s the
 *               yellow LED starts blinking.
 *   getStageInfo() maps stage numbers to readable names (-100 = connection issue,
 *   -1 = idle, 0 = printing, 4 = changing filament, 14 = cleaning nozzle tip, etc.).
 *
 *
 * 11. Persistent settings (Preferences, namespace "my-config")
 * ------------------------------------------------------------
 *   Key               Type    Meaning
 *   ssid, password    string  WiFi credentials
 *   mqtt_server       string  Printer IP address
 *   mqtt_password     string  Printer access code
 *   serial_number     string  Printer serial number
 *   printer_model     string  X1, P1 or A1 (controls pushall interval)
 *   motorRunTime      int     Run duration (ms)
 *   motorWaitTime     int     Wait before running (ms)
 *   delayAfterRun     int     Lock-out after a run (ms)
 *   useMotionSensor   bool    Motion sensor mode instead of MQTT detection
 *   motorDirection    int     0 = Forward, 1 = Reverse
 *   dutyCycle         int     Motor 1 PWM (0–255)
 *   dutyCycle2        int     Motor 2 PWM (0–255)
 *   debug             bool    Verbose serial and log output
 *   pushIntervalSec   int     Pushall interval in seconds (0 = auto by printer model)
 *   gmtOffset_sec     int     Timezone offset in hours (despite the name), default -6
 *
 *
 * 12. Logging
 * -----------
 *   addLogEntry() stores the last 200 events in a RAM ring buffer with a timestamp. View
 *   them at /logs. Logs are lost on reboot. Timestamps need NTP (pool.ntp.org),
 *   synced once after WiFi connects. Debug mode adds more log lines and serial output
 *   (115200 baud) but reduces performance.
 *
 *
 * 13. Code map
 * ------------
 *   OLED helpers       initOLED, oledShow, drawDigitalNumber, drawProgressBar,
 *                      flashScreen, startMotorUI
 *   Settings/globals   WiFi and MQTT credentials, pins, timing, PWM, state flags
 *   Utilities          getStageInfo, addLogEntry, syncTime, formatDateTime
 *   Web handlers       handleConfig, handleControl, handleManualRun, handleMotorStatus,
 *                      handleLogs, handleFirmwareUpload, handleRoot
 *   MQTT               mqttCallback, connectToMqtt, publishPushAllMessage,
 *                      sendPushAllCommand
 *   Idle info screens  updateInfoScreen, drawProgressPage, drawJobPage, gcodeStateLabel
 *   WiFi               connectToWiFi, startWiFiAPMode, resetToAPMode
 *   setup()            I2C scan, pin and PWM init, load settings, OLED, WiFi/AP, MQTT,
 *                      web routes
 *   loop()             Web/DNS → reset button → (stop here if AP mode) → manual button
 *                      → motion sensor → auto pushall → motor state machine and OLED
 *                      countdown → post-run screen → LED blinking → MQTT reconnect
 *
 *
 * 14. Known issues and things to be aware of
 * ------------------------------------------
 *   These come from a code review.
 *
 *   Fixed in this version
 *   - Timezone: the config page now has a "Timezone offset (hours)" field, so saving no
 *     longer resets it to 0. The value is clamped to -12..14. Note that the name
 *     gmtOffset_sec is misleading: it holds hours.
 *   - Defaults: globals and the Preferences fallbacks now agree (run 10000 ms, debug off).
 *   - Config POST uses strlcpy() with buffer sizes, so long inputs are truncated instead
 *     of overflowing memory. Motor speeds are clamped to 0–255.
 *   - Config page hover colors had a double "#" (invalid CSS).
 *   - Motor stop now also sets the PWM enable duty to 0.
 *   - The post-run screen no longer returns early, so LEDs and MQTT keep running.
 *   - POST /control now behaves like /run (resets extra wait, LEDs, OLED message).
 *   - The MQTT port setting (mqtt_port, default 8883) is now actually used.
 *
 *   Still open
 *   - daylightOffset_sec (3600) is always applied, so the clock may be 1 hour ahead
 *     outside daylight saving time. Left unchanged to avoid changing deployed behavior.
 *
 *   Security
 *   - No authentication on any web endpoint, including /update (OTA) and /config.
 *   - The config page pre-fills the saved WiFi password and printer access code, and
 *     contains hard-coded access code and serial number options in the dropdowns
 *     (handleConfig). Remove them before sharing the source.
 *   - MQTT TLS certificate is not verified (setInsecure()).
 *   - The setup AP uses a fixed, well-known password ("12345678").
 *
 *   Hardware
 *   - GPIO 2, 5, 12 and 15 are ESP32 strapping pins. Buttons or loads on them can
 *     interfere with boot on some boards (especially GPIO 12 pulled HIGH at reset).
 *     Wiring is deployed, so pins are intentionally unchanged.
 *
 *
 * Change history
 * --------------
 */
// Poop Conveyor - Patched OLED UI & Motor Countdown
// Original base code provided by user
// Patched: By Marcos, improved OLED UI, icons, countdown, progress bar, flash on motor start
// Version update (visual patch)
// Added a button for manual trigger
// Added the feature to reset (enter AP mode) after pressing button for 3 sconds
// Added OLED messages when using motion sensor
char version[10] = "1.6";

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <DNSServer.h>
#include <time.h>

// ---------- OLED includes ----------
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_SDA 5
#define OLED_SCL 13

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Forward declarations (OLED helpers & UI)
void initOLED();
void oledShow(const String &line1, const String &line2 = "", const String &line3 = "", const unsigned char* icon = nullptr);
void drawDigitalNumber(int x, int y, int seconds);
void drawProgressBar(int x, int y, int w, int h, float progress);
void flashScreen();
void startMotorUI(); // called when motor starts

// ---------- 16x16 icons (monochrome bitmaps, 16x16 => 32 bytes each) ----------
const unsigned char iconConveyor [] PROGMEM = {
  0x00,0x00,0x00,0x00,0xFF,0xC0,0x81,0x80,
  0xBD,0x80,0xA5,0x80,0xBD,0x80,0x81,0x80,
  0xFF,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

const unsigned char iconMotor [] PROGMEM = {
  0x00,0x00,0x0E,0x00,0x11,0x80,0x21,0x40,
  0x42,0x20,0x42,0x20,0x21,0x40,0x11,0x80,
  0x0E,0x00,0x00,0x00,0xFF,0xFF,0x81,0x81,
  0x81,0x81,0xFF,0xFF,0x00,0x00,0x00,0x00
};

const unsigned char iconCheck [] PROGMEM = {
  0x00,0x00,0x00,0x80,0x01,0xC0,0x03,0xE0,
  0x07,0xF0,0x0E,0x70,0x1C,0x30,0x38,0x18,
  0x70,0x0C,0xE0,0x06,0xC0,0x03,0x80,0x01,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

const unsigned char iconWarn [] PROGMEM = {
  0x08,0x00,0x1C,0x00,0x3E,0x00,0x7F,0x00,
  0x7F,0x00,0x7F,0x00,0x3E,0x00,0x1C,0x00,
  0x0C,0x00,0x0C,0x00,0x00,0x00,0x0C,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

// ---- OLED helper implementation ----
void initOLED() {
    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("OLED init failed!");
        return;
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Poop Conveyor");
    display.println("Booting...");
    display.display();
    delay(800);
}

// improved, backward-compatible oledShow()
// optional icon parameter (16x16)
void oledShow(const String &line1, const String &line2, const String &line3, const unsigned char* icon) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    // Decide size: if single short line -> larger text
    int lines = 0;
    if (line1.length()) lines++;
    if (line2.length()) lines++;
    if (line3.length()) lines++;

    // choose text size heuristically
    int textSize = (lines == 1 && line1.length() <= 10) ? 2 : 1;
    display.setTextSize(textSize);

    int iconOffsetX = 0;
    if (icon != nullptr) {
        // draw icon left-centered vertically
        int yIcon = (SCREEN_HEIGHT - 16) / 2;
        display.drawBitmap(0, yIcon, icon, 16, 16, SSD1306_WHITE);
        iconOffsetX = 20; // leave some margin after icon
    }

    // helper to center a single-line string in available width (after icon)
    auto drawCentered = [&](const String &s, int y) {
        int16_t x1, y1;
        uint16_t w, h;
        display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
        int x = iconOffsetX + (SCREEN_WIDTH - iconOffsetX - w) / 2;
        if (x < iconOffsetX) x = iconOffsetX;
        display.setCursor(x, y);
        display.print(s);
    };

    if (textSize == 2) {
        // top-centered
        if (line1.length()) drawCentered(line1, 6);
    } else {
        // maximize vertical spacing for up to 3 lines
        int startY = 0;
        if (lines == 1) startY = 10;
        else if (lines == 2) startY = 4;
        else startY = 0;

        if (line1.length()) drawCentered(line1, startY);
        if (line2.length()) drawCentered(line2, startY + 10);
        if (line3.length()) drawCentered(line3, startY + 20);
    }

    display.display();
}

// drawDigitalNumber: show two-digit seconds with larger font
void drawDigitalNumber(int x, int y, int seconds) {
    if (seconds < 0) seconds = 0;
    char buf[8];
    int s = seconds; // seconds already provided as integer
    if (s > 99) s = 99; // cap for display
    snprintf(buf, sizeof(buf), "%02d", s);

    display.setTextSize(2);
    display.setCursor(x, y);
    display.print(buf);
    display.setTextSize(1);
}

// draw progress bar w/h at x,y with progress in [0..1]
void drawProgressBar(int x, int y, int w, int h, float progress) {
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    display.drawRect(x, y, w, h, SSD1306_WHITE);
    int innerW = w - 2;
    int fill = (int)(innerW * progress + 0.5f);
    if (fill > 0) display.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

void flashScreen() {
    display.invertDisplay(true);
    delay(120);
    display.invertDisplay(false);
}

// ---- End OLED helpers ----

// ---- SETTINGS YOU SHOULD ENTER --------------------------------------------------------------------------------------------------------------------------

// WiFi credentials
char ssid[50] = "";
char password[50] = "";

// MQTT credentials
char mqtt_server[40] = "your-bambu-printer-ip";
char mqtt_password[30] = "your-bambu-printer-accesscode";
char serial_number[35] = "your-bambu-printer-serial-number";
// Printer model selection (X1, P1, A1)
char printer_model[5] = "X1";  // Default to X1

// --------------------------------------------------------------------------------------------------------------------------------------------------------

// OPTIONAL: IF YOU WANT ACCURATE LOG TIMES UPDATE YOUR TIMEZONE HERE
int gmtOffset_sec = -6; // Default to CST (GMT-6 hours)
const int daylightOffset_sec = 3600; // Daylight savings

// GPIO Pins
const int greenLight = 19;
const int yellowLight = 18;
const int redLight = 4;

const int motionSensorPin = 22;  // Adjust the pin as needed
bool useMotionSensor = false;

const int manualTriggerPin = 12;  // Manual trigger button pin
const int resetButtonPin = 2;      // Reset button pin (enters AP mode when held for 3 seconds)

char mqtt_port[6] = "8883";
char mqtt_user[30] = "bblp";
char mqtt_topic[200];

// Poop Motor 1
int motor1Pin1 = 23;
int motorDirection = 0; // Default to 0 (Forward)
int motor1Pin2 = 21;
int enable1Pin = 15;

// Poop Motor 2 (Second motor output on L298N)
int motor2Pin1 = 16;  // Motor 2 direction pin 1 (L298N IN3)
int motor2Pin2 = 17;  // Motor 2 direction pin 2 (L298N IN4)
int enable2Pin = 14;  // Motor 2 enable pin (L298N ENB)

int motorRunTime = 10000; // 10 seconds by default (milliseconds)
int motorWaitTime = 5000; // The time to wait to run the motor.
int delayAfterRun = 120000; // Delay after motor run (ms)
int pushIntervalSec = 0;  // Seconds between "pushall" requests to the printer (0 = automatic by printer model)
int additionalWaitTime = 0; // Variable to store additional wait time for specific stages

// Setting PWM properties
const int freq = 5000;
const int pwmChannel = 0;  // PWM channel for motor 1
const int pwmChannel2 = 1; // PWM channel for motor 2
const int pwmTimer = 0;
const int resolution = 8;
int dutyCycle = 240;   // Motor 1 speed (0-255)
int dutyCycle2 = 140;  // Motor 2 speed (0-255)

const char* base64Image = "data:image/webp;base64,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";

// Debug flag
bool debug = false;
bool autoPushAllEnabled = true;
bool pushAllCommandSent = false;
bool wifiConnected = false;
bool mqttConnected = false;
bool isAPMode = false;

unsigned long lastAttemptTime = 0;
const unsigned long RECONNECT_INTERVAL = 15000;  // 15 seconds

unsigned int sequence_id = 20000;
unsigned long previousMillis = 0;
unsigned long redLightToggleTime = 0;
unsigned long greenLightToggleTime = 0;
unsigned long motorRunStartTime = 0;
unsigned long motorWaitStartTime = 0;
unsigned long delayAfterRunStartTime = 0;
unsigned long yellowLightStartTime = 0;
unsigned int yellowLightState = 0;
bool motorRunning = false;
bool motorWaiting = false;
bool delayAfterRunning = false;
bool manualMode = false;  // Flag to track if motors were triggered manually

// Manual trigger button state
int lastButtonState = HIGH;  // Button state (HIGH = not pressed, LOW = pressed with pull-up)
int currentButtonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;  // 50ms debounce delay
bool buttonPressed = false;

// Reset button state (for entering AP mode)
int lastResetButtonState = HIGH;
unsigned long resetButtonPressStartTime = 0;
const unsigned long RESET_BUTTON_HOLD_TIME = 3000;  // 3 seconds hold time to trigger reset
bool resetButtonPressed = false;

DNSServer dnsServer;

#define MAX_LOG_ENTRIES 200

struct LogEntry {
    time_t timestamp;
    String action;
};

LogEntry logs[MAX_LOG_ENTRIES];
int logIndex = 0;

// Sync time so we have proper logging
const char* ntpServer = "pool.ntp.org";

// MQTT state variables
int printer_stage = -100;
int printer_sub_stage = -100;
String printer_real_stage = "";
String gcodeState = "";
int printer_percent = -1;         // print.mc_percent (-1 = unknown)
int printer_remaining_min = -1;   // print.mc_remaining_time in minutes (-1 = unknown)
int printer_layer = -1;           // print.layer_num
int printer_total_layers = -1;    // print.total_layer_num
String printer_job = "";          // print.subtask_name

// Idle info screens (progress / job), rotated on the OLED when nothing else is happening
const unsigned long INFO_PAGE_INTERVAL = 4000;  // switch page every 4 s
const unsigned long INFO_HOLD_TIME = 5000;      // keep "MQTT Connected" / "DONE" visible this long
unsigned long infoHoldStart = 0;

// Create instances
WiFiClientSecure espClient;
PubSubClient client(espClient);
Preferences preferences;
WebServer server(80);

//Flags
bool showPostRunStatus = false;
unsigned long postRunShownAt = 0;

// Function to get the printer stage description
const char* getStageInfo(int stage) {
    switch (stage) {
        case -100: return "Connection Issue";
        case -1: return "Idle";
        case 0: return "Printing";
        case 1: return "Auto Bed Leveling";
        case 2: return "Heatbed Preheating";
        case 3: return "Sweeping XY Mech Mode";
        case 4: return "Changing Filament";
        case 5: return "M400 Pause";
        case 6: return "Paused due to filament runout";
        case 7: return "Heating Hotend";
        case 8: return "Calibrating Extrusion";
        case 9: return "Scanning Bed Surface";
        case 10: return "Inspecting First Layer";
        case 11: return "Identifying Build Plate Type";
        case 12: return "Calibrating Micro Lidar";
        case 13: return "Homing Toolhead";
        case 14: return "Cleaning Nozzle Tip";
        case 15: return "Checking Extruder Temperature";
        case 16: return "Printing was paused by the user";
        case 17: return "Pause of front cover falling";
        case 18: return "Calibrating Micro Lidar";
        case 19: return "Calibrating Extrusion Flow";
        case 20: return "Paused due to nozzle temperature malfunction";
        case 21: return "Paused due to heat bed temperature malfunction";
        case 22: return "Filament unloading";
        case 23: return "Skip step pause";
        case 24: return "Filament loading";
        case 25: return "Motor noise calibration";
        case 26: return "Paused due to AMS lost";
        case 27: return "Paused due to low speed of the heat break fan";
        case 28: return "Paused due to chamber temperature control error";
        case 29: return "Cooling chamber";
        case 30: return "Paused by the Gcode inserted by user";
        case 31: return "Motor noise showoff";
        case 32: return "Nozzle filament covered detected pause";
        case 33: return "Cutter error pause";
        case 34: return "First layer error pause";
        case 35: return "Nozzle clog pause";
        default: return "Unknown stage";
    }
}

// Function to add log entries
void addLogEntry(String action) {
    time_t now = time(nullptr);

    logs[logIndex].timestamp = now; // Store raw timestamp (UTC)
    logs[logIndex].action = action;
    logIndex = (logIndex + 1) % MAX_LOG_ENTRIES;
}

void syncTime() {
    addLogEntry("Syncing time...");
    configTime(gmtOffset_sec * 3600, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    int retries = 0;
    while (!getLocalTime(&timeinfo) && retries < 10) {
        addLogEntry("Failed to obtain time, retrying...");
        delay(1000);
        retries++;
    }

    if (retries < 10) {
        char timeString[50];
        strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
        addLogEntry("Time synchronized! ESP32 thinks current time is: " + String(timeString));
    } else {
        addLogEntry("Failed to synchronize time after multiple attempts.");
    }
}

void handleFirmwareUpload() {
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("Firmware update initiated: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { // Start OTA update
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) { // Finish OTA update
            Serial.println("Firmware update successful!");
            server.send(200, "text/html", "<h1>Update Successful! Rebooting...</h1>");
            delay(1000);
            ESP.restart();
        } else {
            Update.printError(Serial);
        }
    }
}

// Function to handle the control page
void handleControl() {
    if (server.method() == HTTP_GET) {
        String html = "<html><head>";
        html += "<style>";
        html += "body { font-family: Arial, sans-serif; background-color: #f4f4f4; margin: 0; padding: 0; text-align: center; }";
        html += ".container { max-width: 600px; margin: 50px auto; padding: 20px; background-color: #fff; box-shadow: 0 0 10px rgba(0, 0, 0, 0.1); border-radius: 8px; }";
        html += "h1 { color: #333; }";
        html += "form { margin: 20px 0; }";
        html += "input[type='submit'] { background-color: #4CAF50; color: white; padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; }";
        html += "input[type='submit']:hover { background-color: #45a049; }";
        html += "</style>";
        html += "</head><body>";
        html += "<div class='container'>";
        html += "<h1>Manual Motor Control</h1>";
        html += "<form action=\"/control\" method=\"POST\">";
        html += "<input type=\"submit\" value=\"Activate Motor\">";
        html += "</form>";
        html += "</div>";
        html += "</body></html>";
        server.send(200, "text/html", html);
    } else if (server.method() == HTTP_POST) {
        server.send(200, "text/plain", "Motor activated manually");
        manualMode = false;  // Web trigger is not manual button mode
        motorWaiting = true;
        motorWaitStartTime = millis();
        additionalWaitTime = 0;  // Reset additional wait time for manual trigger
        digitalWrite(greenLight, LOW);
        digitalWrite(yellowLight, HIGH);
        addLogEntry("Motor activated manually");
        oledShow("Motor", "Manual trigger", "", iconMotor);
    }
}

// Function to handle the /run URL (Home Assistant / automations)
void handleManualRun() {
    server.send(200, "text/plain", "Motor activated");
    manualMode = false;  // Web trigger is not manual button mode
    motorWaiting = true;
    motorWaitStartTime = millis();
    additionalWaitTime = 0;  // Reset additional wait time for manual trigger
    digitalWrite(greenLight, LOW);
    digitalWrite(yellowLight, HIGH);
    addLogEntry("Motor activated from RUN url");
    oledShow("Motor", "Manual trigger", "", iconMotor);
}

void handleConfig() {
    if (server.method() == HTTP_GET) {
        String html = "<!DOCTYPE html><html><head><title>Bambu Poop Conveyor</title>";
        html += "<style>";
        html += "body { font-family: Arial, sans-serif; background-color: #d1cdba; color: #333; text-align: center; margin: 0; padding: 0; }";
        html += ".container { max-width: 600px; margin: 5px auto; background: #fff; padding: 20px; border-radius: 5px; box-shadow: 0 0 10px rgba(0, 0, 0, 0.1); }";
        html += ".logo img { width: 100px; height: auto; margin-bottom: 0px; }";
        html += "h2 { font-size: 24px; margin-bottom: 20px; }";
        html += ".info { text-align: left; }";
        html += ".info label { display: block; margin: 10px 0 5px; font-weight: bold; }";
        html += ".info input { width: calc(100% - 20px); padding: 10px; margin-bottom: 15px; border: 1px solid #ccc; border-radius: 5px; }";
        html += "form { margin-top: 20px; }";
        html += "input[type=submit] { padding: 10px 20px; border: none; border-radius: 5px; background-color: #28a745; color: white; cursor: pointer; }";
        html += "input[type=submit]:hover { background-color: #218838; }";
        html += ".links a:hover { background-color: #0056b3; }";
        html += ".links { margin-top: 20px; }";
        html += ".links a { display: inline-block; margin: 0 10px; padding: 10px 20px; background-color: #007bff; color: white; text-decoration: none; border-radius: 5px; }";
        html += ".links a:hover { background-color: #0056b3; }";
        html += "</style>";
        html += "</head><body>";
        html += "<div class=\"logo\"><img src=\"" + String(base64Image) + "\" alt=\"Bambu Conveyor Logo\"></div>";
        html += "<div class=\"container\">";
        html += "<h2>Bambu Poop Conveyor v" + String(version) + "</h2>";
        html += "<form action=\"/config\" method=\"POST\" class=\"info\">";
        html += "<label for=\"ssid\">WiFi SSID:</label><input type=\"text\" id=\"ssid\" name=\"ssid\" value=\"" + String(ssid) + "\"><br>";
        html += "<label for=\"password\">WiFi Password:</label><input type=\"password\" id=\"password\" name=\"password\" value=\"" + String(password) + "\"><br>";
        html += "<label for=\"mqtt_server\">Bambu Printer IP Address:</label><input type=\"text\" id=\"mqtt_server\" name=\"mqtt_server\" value=\"" + String(mqtt_server) + "\"><br>";
        html += "<label for='mqtt_password'>Bambu Printer Access Code:</label>";
        html += "<select id='mqtt_password_select' onchange=\"document.getElementById('mqtt_password').value=this.value\">";
        html += "  <option value=''>-- Select code --</option>";
        html += "  <option value='2fcc0064'>2fcc0064</option>";
        html += "</select><br>";
        html += "<input type='text' id='mqtt_password' name='mqtt_password' value='" + String(mqtt_password) + "'><br>";
        html += "<label for='serial_number'>Bambu Printer Serial Number:</label>";
        html += "<select id='serial_number_select' onchange=\"document.getElementById('serial_number').value=this.value\">";
        html += "  <option value=''>-- Select serial --</option>";
        html += "  <option value='00M09D563001230'>00M09D563001230</option>";
        html += "</select><br>";
        html += "<input type='text' id='serial_number' name='serial_number' value='" + String(serial_number) + "'><br>";
        html += "<label for=\"motorRunTime\">Motor Run Time (ms):</label><input type=\"number\" id=\"motorRunTime\" name=\"motorRunTime\" value=\"" + String(motorRunTime) + "\"><br>";
        html += "<label for=\"motorWaitTime\">Motor Wait Time (ms):</label><input type=\"number\" id=\"motorWaitTime\" name=\"motorWaitTime\" value=\"" + String(motorWaitTime) + "\"><br>";
        html += "<label for=\"delayAfterRun\">Delay After Run (ms):</label><input type=\"number\" id=\"delayAfterRun\" name=\"delayAfterRun\" value=\"" + String(delayAfterRun) + "\"><br>";
        html += "<label for=\"useMotionSensor\"> Use Motion Sensor (Disables MQTT detection):</label>";
        html += "<input type=\"checkbox\" id=\"useMotionSensor\" name=\"useMotionSensor\" " + String(useMotionSensor ? "checked" : "") + "><br>";
        html += "<label for=\"printer_model\">Printer Model:</label>";
        html += "<select id=\"printer_model\" name=\"printer_model\">";
        html += "<option value=\"X1\"" + String((String(printer_model) == "X1") ? " selected" : "") + ">X1</option>";
        html += "<option value=\"P1\"" + String((String(printer_model) == "P1") ? " selected" : "") + ">P1</option>";
        html += "<option value=\"A1\"" + String((String(printer_model) == "A1") ? " selected" : "") + ">A1</option>";
        html += "</select><br>";
        html += "<label for=\"dutyCycle\">Motor 1 Speed (0-255):</label>";
        html += "<input type=\"number\" id=\"dutyCycle\" name=\"dutyCycle\" value=\"" + String(dutyCycle) + "\" min=\"0\" max=\"255\"><br>";
        html += "<label for=\"dutyCycle2\">Motor 2 Speed (0-255):</label>";
        html += "<input type=\"number\" id=\"dutyCycle2\" name=\"dutyCycle2\" value=\"" + String(dutyCycle2) + "\" min=\"0\" max=\"255\"><br>";
        html += "<label for=\"motorDirection\">Motor Direction:</label>";
        html += "<select id=\"motorDirection\" name=\"motorDirection\">";
        html += "<option value=\"0\"" + String((motorDirection == 0) ? " selected" : "") + ">Forward</option>";
        html += "<option value=\"1\"" + String((motorDirection == 1) ? " selected" : "") + ">Reverse</option>";
        html += "</select><br>";
        html += "<label for=\"pushIntervalSec\">Printer status request interval (seconds, 0 = automatic: 30 for X1, 300 for P1/A1):</label>";
        html += "<input type=\"number\" id=\"pushIntervalSec\" name=\"pushIntervalSec\" value=\"" + String(pushIntervalSec) + "\" min=\"0\" max=\"3600\"><br>";
        html += "<label for=\"gmtOffset_sec\">Timezone offset (hours from GMT, e.g. -6):</label>";
        html += "<input type=\"number\" id=\"gmtOffset_sec\" name=\"gmtOffset_sec\" value=\"" + String(gmtOffset_sec) + "\" min=\"-12\" max=\"14\"><br>";
        html += "<label for=\"debug\"> Debug Mode (Reduced performance):</label>";
        html += "<input type=\"checkbox\" id=\"debug\" name=\"debug\" " + String(debug ? "checked" : "") + "><br>";
        html += "<input type=\"submit\" value=\"Save Settings and Reboot\">";
        html += "<br>";
        html += "<div class=\"links\">";
        html += "<a href=\"/control\">Motor Manual Control Page</a>";
        html += "<a href=\"/logs\">Logs Page</a>";

        html += "</div></div></body></html>";

        server.send(200, "text/html", html);
    }
    else if (server.method() == HTTP_POST) {
        preferences.begin("my-config", false);

        // Retrieve and store the entered values
        strlcpy(ssid, server.arg("ssid").c_str(), sizeof(ssid));
        strlcpy(password, server.arg("password").c_str(), sizeof(password));
        strlcpy(mqtt_server, server.arg("mqtt_server").c_str(), sizeof(mqtt_server));
        strlcpy(mqtt_password, server.arg("mqtt_password").c_str(), sizeof(mqtt_password));
        strlcpy(serial_number, server.arg("serial_number").c_str(), sizeof(serial_number));
        strlcpy(printer_model, server.arg("printer_model").c_str(), sizeof(printer_model));

        dutyCycle = constrain(server.arg("dutyCycle").toInt(), 0, 255);
        dutyCycle2 = constrain(server.arg("dutyCycle2").toInt(), 0, 255);
        motorRunTime = server.arg("motorRunTime").toInt();
        motorWaitTime = server.arg("motorWaitTime").toInt();
        delayAfterRun = server.arg("delayAfterRun").toInt();
        useMotionSensor = server.hasArg("useMotionSensor");
        debug = server.hasArg("debug");
        motorDirection = server.arg("motorDirection").toInt();
        if (server.hasArg("pushIntervalSec")) {
            int v = server.arg("pushIntervalSec").toInt();
            pushIntervalSec = (v <= 0) ? 0 : constrain(v, 5, 3600);  // 0 = auto, otherwise 5 s to 1 h
        }
        if (server.hasArg("gmtOffset_sec")) {
            gmtOffset_sec = constrain(server.arg("gmtOffset_sec").toInt(), -12, 14);  // hours
        }

        // Store in Preferences for persistence
        preferences.putString("ssid", ssid);
        preferences.putString("password", password);
        preferences.putString("mqtt_server", mqtt_server);
        preferences.putString("mqtt_password", mqtt_password);
        preferences.putString("serial_number", serial_number);
        preferences.putInt("motorRunTime", motorRunTime);
        preferences.putInt("motorWaitTime", motorWaitTime);
        preferences.putInt("delayAfterRun", delayAfterRun);
        preferences.putBool("useMotionSensor", useMotionSensor);
        preferences.putString("printer_model", printer_model);
        preferences.putInt("motorDirection", motorDirection);
        preferences.putInt("dutyCycle", dutyCycle);
        preferences.putInt("dutyCycle2", dutyCycle2);
        preferences.putBool("debug", debug);
        preferences.putInt("gmtOffset_sec", gmtOffset_sec);
        preferences.putInt("pushIntervalSec", pushIntervalSec);

        preferences.end();

        server.send(200, "text/html", "<h1>Settings saved! This page will automatically refresh in 15 seconds...</h1><script>setTimeout(() => { window.location.href = '/config'; }, 15000);</script><br><br><a href=\"/config\">Refresh now</a>");

        delay(1000);
        ESP.restart();
    }
}

String formatDateTime(time_t timestamp) {
    struct tm timeinfo;
    time_t adjustedTime = timestamp; // Apply timezone
    localtime_r(&adjustedTime, &timeinfo);

    char buffer[25];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %I:%M:%S %p", &timeinfo); // 12-hour format with AM/PM

    return String(buffer);
}

// Handle Home Assistant status check
void handleMotorStatus() {
    String jsonResponse = "{ \"motor_running\": " + String(motorRunning ? "true" : "false") + " }";
    server.send(200, "application/json", jsonResponse);
}

void handleLogs() {
    String html = "<html><head><title>Debug Logs</title>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; background-color: #f4f4f4; margin: 0; padding: 0; text-align: center; }";
    html += ".container { max-width: 1000px; margin: 30px auto; padding: 20px; background-color: #fff; box-shadow: 0 0 10px rgba(0, 0, 0, 0.1); border-radius: 8px; }";
    html += "h1 { color: #333; }";
    html += "table { width: 100%; border-collapse: collapse; margin-top: 20px; }";
    html += "th, td { padding: 5px; border: 1px solid #ddd; text-align: left; }";
    html += "th { background-color: #f2f2f2; }";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='container'>";
    html += "<h1>Logs</h1>";
    html += "<table><tr><th>Timestamp</th><th>Action</th></tr>";

    for (int i = 0; i < MAX_LOG_ENTRIES; i++) {
        int index = (logIndex + i) % MAX_LOG_ENTRIES;
        if (logs[index].timestamp > 0) {
            html += "<tr><td>" + formatDateTime(logs[index].timestamp) + "</td><td>" + logs[index].action + "</td></tr>";
        }
    }

    html += "</table></div></body></html>";
    server.send(200, "text/html", html);
}

// MQTT callback function
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    DynamicJsonDocument doc(40000);
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error) {
        if (debug) {
            Serial.print(F("deserializeJson() failed: "));
            Serial.println(error.c_str());
            addLogEntry("deserializeJson() failed: ");
            addLogEntry(error.c_str());
        }
        return;
    }

    if (doc.containsKey("print") && doc["print"].containsKey("stg_cur")) {
        printer_stage = doc["print"]["stg_cur"].as<int>();
    }

    if (doc.containsKey("print") && doc["print"].containsKey("mc_print_sub_stage")) {
        printer_sub_stage = doc["print"]["mc_print_sub_stage"].as<int>();
    }

    // Extra info for the OLED (printer reports are often partial, so only update what is present)
    if (doc.containsKey("print")) {
        JsonObject p = doc["print"];
        if (p.containsKey("gcode_state")) gcodeState = p["gcode_state"].as<String>();
        if (p.containsKey("mc_percent")) printer_percent = p["mc_percent"].as<int>();
        if (p.containsKey("mc_remaining_time")) printer_remaining_min = p["mc_remaining_time"].as<int>();
        if (p.containsKey("layer_num")) printer_layer = p["layer_num"].as<int>();
        if (p.containsKey("total_layer_num")) printer_total_layers = p["total_layer_num"].as<int>();
        if (p.containsKey("subtask_name")) printer_job = p["subtask_name"].as<String>();
    }

    if (!useMotionSensor && !motorWaiting && !motorRunning && !delayAfterRunning &&
        (printer_stage == 4 || printer_stage == 14 || (printer_sub_stage == 4 && printer_stage != -1))) {
        manualMode = false;  // MQTT trigger is not manual button mode
        motorWaiting = true;
        motorWaitStartTime = millis();
        addLogEntry("Status 4 or 14 detected! Running conveyor!!!");

        if (debug) {
            Serial.println("Status 4 or 14 detected! Running conveyor!!!");
        }

        if (printer_sub_stage == 4 && printer_stage != -1) {
            additionalWaitTime = 75000;
        } else {
            additionalWaitTime = 0;
        }

        yellowLightStartTime = millis();
        yellowLightState = HIGH;
        digitalWrite(yellowLight, yellowLightState);

        // OLED notification
        oledShow("Printer", "Event detected", "Waiting...", iconWarn);
    }

    if (debug && !useMotionSensor) {
        Serial.println("Bambu Poop Conveyor v" + String(version) +
               " | Wifi: " + WiFi.localIP().toString() +
               " | Current Print Stage: " + String(getStageInfo(printer_stage)) +
               " | Sub stage: " + String(getStageInfo(printer_sub_stage)));
        addLogEntry("MQTT Callback - Getting data from printer. Data: Current print stage: " + String(getStageInfo(printer_stage)) + " | Current sub print stage: " + String(getStageInfo(printer_sub_stage)));
    }
}

void publishPushAllMessage() {
    if (client.connected()) {
        char publish_topic[128];
        sprintf(publish_topic, "device/%s/request", serial_number);

        DynamicJsonDocument doc(1024);
        doc["pushing"]["sequence_id"] = sequence_id;
        doc["pushing"]["command"] = "pushall";
        doc["user_id"] = "2222222";

        String jsonMessage;
        serializeJson(doc, jsonMessage);

        bool success = client.publish(publish_topic, jsonMessage.c_str());

        if (success) {
            if (debug) {
                Serial.print("Message successfully sent to: ");
                Serial.println(publish_topic);
                addLogEntry("MQTT message sent from publishPushAllMessage");
            }
        } else {
            if (debug) Serial.println("Failed to send message.");
            addLogEntry("Failed to send MQTT push all - publishPushAllMessage");
        }

        sequence_id++;
    } else {
        if (debug) {
            Serial.println("Not connected to MQTT broker!");
            addLogEntry("Not connected to MQTT broker! - publishPushAllMessage");
        }
    }
}


// Function to connect to MQTT
void connectToMqtt() {
    if (!client.connected()) {
        if (debug) {
            Serial.print("Connecting to MQTT...");
            addLogEntry("Connecting to MQTT...");
        }
        if (client.connect("BambuConveyor", mqtt_user, mqtt_password)) {
            Serial.println("Connected to Bambu printer");
            addLogEntry("Connected to Bambu printer");
            sprintf(mqtt_topic, "device/%s/report", serial_number);
            client.subscribe(mqtt_topic);
            publishPushAllMessage();
            digitalWrite(redLight, LOW);
            digitalWrite(yellowLight, LOW);
            if (debug) {
                Serial.print("Red light off");
                addLogEntry("Red light off");
                Serial.print("Yellow light off");
                addLogEntry("Yellow light off");
            }
            // OLED notification
            oledShow("MQTT Connected", "to Bambu printer", "", iconCheck);
            infoHoldStart = millis();
        } else {
            if (debug) {
                Serial.print("Failed: ");
                Serial.print(client.state());
                Serial.println(" try again in 5 seconds");
                addLogEntry("Failed to connect to MQTT (Bambu Printer), trying again in 5 seconds");
            }
            lastAttemptTime = millis();
            digitalWrite(redLight, HIGH);
            addLogEntry("RED light turned on");
            // OLED notification
            oledShow("Failed to connect to", "MQTT (Bambu Printer)", "", iconWarn);
        }
    }
}

// Function to send a push all command
void sendPushAllCommand() {
    if (client.connected() && !pushAllCommandSent) {
        String mqtt_topic_request = "device/";
        mqtt_topic_request += serial_number;
        mqtt_topic_request += "/request";

        StaticJsonDocument<128> doc;
        doc["pushing"]["sequence_id"] = String(sequence_id++);
        doc["pushing"]["command"] = "pushall";
        doc["user_id"] = "2222222";

        String payload;
        serializeJson(doc, payload);
        if (debug) {
                Serial.println("MQTT Callback sent - Sendpushallcommand");
                addLogEntry("MQTT Callback sent - Sendpushallcommand");
        }
        client.publish(mqtt_topic_request.c_str(), payload.c_str());
        pushAllCommandSent = true;
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("Scanning I2C...");
    Wire.begin(OLED_SDA, OLED_SCL);

    for (byte i = 1; i < 127; i++) {
        Wire.beginTransmission(i);
        if (Wire.endTransmission() == 0) {
            Serial.print("I2C device found at 0x");
            Serial.println(i, HEX);
        }
    }
    // Initialize logs
    for (int i = 0; i < MAX_LOG_ENTRIES; i++) {
        logs[i].timestamp = 0;
    }
    // Initialize GPIO pins
    pinMode(motor1Pin1, OUTPUT);
    pinMode(motor1Pin2, OUTPUT);
    pinMode(enable1Pin, OUTPUT);
    pinMode(motor2Pin1, OUTPUT);
    pinMode(motor2Pin2, OUTPUT);
    pinMode(enable2Pin, OUTPUT);
    pinMode(yellowLight, OUTPUT);
    pinMode(redLight, OUTPUT);
    pinMode(greenLight, OUTPUT);
    pinMode(motionSensorPin, INPUT);
    pinMode(manualTriggerPin, INPUT_PULLUP);  // Button with internal pull-up resistor
    pinMode(resetButtonPin, INPUT_PULLUP);    // Reset button with internal pull-up resistor

    client.setBufferSize(40000);

    // Configure LED PWM functionalities (preserve original usage)
    // Note: original code used ledcAttachChannel(enable1Pin, freq, resolution, pwmChannel);
    // If your board uses ledcAttachPin/ledcSetup, keep those as in your environment.
    ledcAttachChannel(enable1Pin, freq, resolution, pwmChannel);
    ledcWrite(enable1Pin, dutyCycle);
    // Configure PWM for motor 2
    ledcAttachChannel(enable2Pin, freq, resolution, pwmChannel2);
    ledcWrite(enable2Pin, dutyCycle2);

    // Start Preferences storage
    preferences.begin("my-config", false);

    // Load all stored values from Preferences
    String storedSSID = preferences.getString("ssid", "");
    String storedPassword = preferences.getString("password", "");
    String storedMqttServer = preferences.getString("mqtt_server", "");
    String storedMqttPassword = preferences.getString("mqtt_password", "");
    String storedSerialNumber = preferences.getString("serial_number", "");
    useMotionSensor = preferences.getBool("useMotionSensor", false);
    String storedPrinterModel = preferences.getString("printer_model", "X1");  // Default "X1" if missing
    debug = preferences.getBool("debug", debug);

    storedSSID.toCharArray(ssid, sizeof(ssid));
    storedPassword.toCharArray(password, sizeof(password));
    storedMqttServer.toCharArray(mqtt_server, sizeof(mqtt_server));
    storedMqttPassword.toCharArray(mqtt_password, sizeof(mqtt_password));
    storedSerialNumber.toCharArray(serial_number, sizeof(serial_number));
    storedPrinterModel.toCharArray(printer_model, sizeof(printer_model));
    motorRunTime = preferences.getInt("motorRunTime", motorRunTime);
    motorWaitTime = preferences.getInt("motorWaitTime", motorWaitTime);
    delayAfterRun = preferences.getInt("delayAfterRun", delayAfterRun);
    motorDirection = preferences.getInt("motorDirection", 0);
    gmtOffset_sec = preferences.getInt("gmtOffset_sec", gmtOffset_sec);
    pushIntervalSec = preferences.getInt("pushIntervalSec", pushIntervalSec);
    dutyCycle = preferences.getInt("dutyCycle", dutyCycle);
    dutyCycle2 = preferences.getInt("dutyCycle2", dutyCycle2);

    preferences.end();

    // initialize OLED (non intrusive)
    initOLED();
    oledShow("Booting...", "Starting WiFi");

    // Decide if we should connect to WiFi or enter AP mode
    if (strlen(ssid) > 0 && strlen(password) > 0) {
        connectToWiFi();
    } else {
        startWiFiAPMode();
    }

    delay(2000);

    // Set up MQTT if WiFi is connected
    if (WiFi.status() == WL_CONNECTED) {
        client.setServer(mqtt_server, atoi(mqtt_port)); // Default 8883
        espClient.setInsecure();
        client.setCallback(mqttCallback);
        sprintf(mqtt_topic, "device/%s/report", serial_number);
        connectToMqtt();
    }

    // Set up Web Server routes
    server.on("/", handleConfig);
    server.on("/control", handleControl);
    server.on("/config", handleConfig);
    server.on("/logs", handleLogs);
    // Register Home Assistant API endpoints
    server.on("/run", handleManualRun);
    server.on("/status", handleMotorStatus);
    server.on("/update", HTTP_POST, []() {
        server.send(200, "text/plain", "Upload complete!");
    }, handleFirmwareUpload);

    // Start Web Server
    server.begin();
    Serial.println("Web server started.");
    addLogEntry("Web server started.");

    if (!client.connected()) {
        sendPushAllCommand();
    }

    // Show initial status based on mode
    if (useMotionSensor) {
        oledShow("Using Motion", "Sensor Mode", "", iconCheck);
    } else if (WiFi.status() == WL_CONNECTED && client.connected()) {
        oledShow("MQTT Connected", "to Bambu printer", "", iconCheck);
        infoHoldStart = millis();
    }
}

void handleRoot() {
    server.sendHeader("Location", "/config", true);
    server.send(302, "text/plain", "Redirecting...");
}

void resetToAPMode() {
    // Stop any running motors first
    if (motorRunning) {
        digitalWrite(motor1Pin1, LOW);
        digitalWrite(motor1Pin2, LOW);
        digitalWrite(motor2Pin1, LOW);
        digitalWrite(motor2Pin2, LOW);
        ledcWrite(enable1Pin, 0);
        ledcWrite(enable2Pin, 0);
        motorRunning = false;
    }

    // Reset motor states
    motorWaiting = false;
    delayAfterRunning = false;
    manualMode = false;

    // Disconnect from MQTT
    if (client.connected()) {
        client.disconnect();
    }

    // Disconnect from WiFi
    WiFi.disconnect(true);
    delay(500);

    // Turn off all lights
    digitalWrite(greenLight, LOW);
    digitalWrite(yellowLight, LOW);
    digitalWrite(redLight, LOW);

    // OLED notification
    oledShow("Resetting to", "AP Mode...", "", iconWarn);
    delay(1000);

    // Now start AP mode
    startWiFiAPMode();
    addLogEntry("Reset to AP mode triggered by button");
}

void startWiFiAPMode() {
    Serial.println("Starting WiFi AP Mode...");
    addLogEntry("Starting WiFi AP Mode...");

    isAPMode = true;

    WiFi.mode(WIFI_AP);
    WiFi.softAP("BambuConveyor", "12345678");  // Open WiFi AP with password

    dnsServer.start(53, "*", WiFi.softAPIP()); // Captive portal redirection
    server.onNotFound(handleRoot);  // Redirect users to /config

    Serial.print("Access Point IP: ");
    Serial.println(WiFi.softAPIP());

    // Flash yellow light to indicate setup mode
    digitalWrite(yellowLight, HIGH);

    // OLED update for AP mode
    oledShow("AP Mode", WiFi.softAPIP().toString(), "", nullptr);
}

void connectToWiFi() {
    const int maxRetries = 20;  // Set a limit for retries
    int retryCount = 0;

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    unsigned long startAttemptTime = millis();
    oledShow("WiFi", "Connecting...");
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 25000) {  // Try for ~25 sec
        if (debug) {
            Serial.print('.');
            addLogEntry("Connecting to WiFi... Attempt " + String(retryCount + 1));
        }

        digitalWrite(yellowLight, HIGH);
        delay(500);
        digitalWrite(yellowLight, LOW);
        delay(500);

        retryCount++;
        if (retryCount >= maxRetries) {
            Serial.println("\nExceeded max WiFi connection attempts, rebooting ESP32...");
            addLogEntry("Exceeded max WiFi connection attempts, rebooting ESP32...");
            delay(2000); // Small delay before reboot
            ESP.restart();
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        digitalWrite(greenLight, HIGH);
        syncTime();
        isAPMode = false;  // Ensure AP mode is OFF when connected
        Serial.println("\nWiFi connected. IP: " + WiFi.localIP().toString());
        addLogEntry("Connected to WiFi: " + WiFi.localIP().toString());
        oledShow("Connected to WiFi", WiFi.localIP().toString(), "", nullptr);
    } else {
        Serial.println("\nWiFi failed to connect. Switching to AP mode.");
        startWiFiAPMode();  // Start AP mode if WiFi fails
        addLogEntry("WiFi failed to connect. Switching to AP mode.");
        oledShow("WiFi failed to connect", "", "", iconWarn);
    }
}

// Loop function
unsigned long lastMQTTDisconnectTime = 0;
bool mqttReconnecting = false;

void startMotorUI() {
    // Called when motor begins running. Save motorRunStartTime already set by motor control logic.
    flashScreen();
    // initial render will happen in loop
}

// Short label for the printer's gcode_state
const char* gcodeStateLabel() {
    if (gcodeState == "RUNNING") return "Printing";
    if (gcodeState == "PAUSE") return "Paused";
    if (gcodeState == "PREPARE") return "Preparing";
    if (gcodeState == "FINISH") return "Finished";
    if (gcodeState == "FAILED") return "Failed";
    if (gcodeState == "IDLE") return "Idle";
    return "Printer";
}

// Page 0: state + percent, remaining time, progress bar
void drawProgressPage() {
    char line[32];
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    if (printer_percent >= 0) snprintf(line, sizeof(line), "%s %d%%", gcodeStateLabel(), printer_percent);
    else snprintf(line, sizeof(line), "%s", gcodeStateLabel());
    display.print(line);

    display.setCursor(0, 11);
    if (printer_remaining_min > 0) {
        if (printer_remaining_min >= 60) snprintf(line, sizeof(line), "%dh %02dm left", printer_remaining_min / 60, printer_remaining_min % 60);
        else snprintf(line, sizeof(line), "%dm left", printer_remaining_min);
        display.print(line);
    }

    float progress = (printer_percent >= 0) ? printer_percent / 100.0f : 0.0f;
    drawProgressBar(2, 22, 124, 8, progress);
    display.display();
}

// Page 1: layer count + job name (truncated to the 21 characters that fit)
void drawJobPage() {
    char line[32];
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    display.print("Job");

    display.setCursor(0, 11);
    String job = printer_job.length() ? printer_job : String("-");
    if (job.length() > 21) job = job.substring(0, 21);
    display.print(job);

    display.setCursor(0, 22);
    if (printer_layer >= 0 && printer_total_layers > 0) {
        snprintf(line, sizeof(line), "Layer %d/%d", printer_layer, printer_total_layers);
        display.print(line);
    }
    display.display();
}

// Idle OLED: rotate progress and job pages. Only called when nothing else owns the screen.
void updateInfoScreen() {
    static unsigned long lastPageSwitch = 0;
    static unsigned long lastDraw = 0;
    static int page = 0;
    unsigned long now = millis();

    if (gcodeState.length() == 0 && printer_percent < 0 && printer_job.length() == 0) {
        // Connected, but no report received yet
        if (now - lastDraw >= 1000) {
            lastDraw = now;
            oledShow("Printer", "Waiting for data", "", nullptr);
        }
        return;
    }

    // Job page is only useful while there is a job
    bool hasJob = printer_job.length() > 0 && (gcodeState == "RUNNING" || gcodeState == "PAUSE" || gcodeState == "PREPARE");
    if (!hasJob) page = 0;
    else if (now - lastPageSwitch >= INFO_PAGE_INTERVAL) {
        page = (page + 1) % 2;
        lastPageSwitch = now;
        lastDraw = 0;  // redraw immediately
    }

    if (now - lastDraw >= 1000 || lastDraw == 0) {
        lastDraw = now;
        if (page == 0) drawProgressPage();
        else drawJobPage();
    }
}

// Loop
void loop() {
    server.handleClient();
    dnsServer.processNextRequest();  // Handle captive portal redirects

    unsigned long currentMillis = millis();
    static unsigned long disconnectedTime = 0;

    // Reset button detection (works even in AP mode)
    int resetButtonReading = digitalRead(resetButtonPin);

    if (resetButtonReading == LOW) {  // Button is pressed (LOW with pull-up)
        if (lastResetButtonState == HIGH) {
            // Button was just pressed, record the time
            resetButtonPressStartTime = currentMillis;
            resetButtonPressed = true;
        } else if (resetButtonPressed && (currentMillis - resetButtonPressStartTime >= RESET_BUTTON_HOLD_TIME)) {
            // Button has been held for the required time
            resetButtonPressed = false;
            resetToAPMode();
            return;  // Exit loop to allow AP mode to take over
        } else if (resetButtonPressed) {
            // Button is being held, show countdown on OLED (if not in AP mode and not running motor)
            if (!isAPMode && !motorRunning) {
                unsigned long holdTime = currentMillis - resetButtonPressStartTime;
                int remainingMs = RESET_BUTTON_HOLD_TIME - holdTime;
                int remainingSec = (remainingMs + 500) / 1000;
                if (remainingSec < 0) remainingSec = 0;

                display.clearDisplay();
                display.setTextSize(1);
                display.setCursor(0, 0);
                display.print("Reset to AP:");
                drawDigitalNumber(88, 0, remainingSec);
                display.setTextSize(1);
                drawProgressBar(2, 22, 124, 8, (float)holdTime / (float)RESET_BUTTON_HOLD_TIME);
                display.display();
            }
        }
    } else {
        // Button is not pressed
        if (lastResetButtonState == LOW && resetButtonPressed) {
            // Button was released before hold time completed
            resetButtonPressed = false;
        }
    }
    lastResetButtonState = resetButtonReading;

    if (isAPMode) return;  // Skip all WiFi/MQTT logic if in AP mode

    // Determine push interval based on printer model
    // Custom interval from the config page, or automatic: 30 sec for X1, 5 min for others
    unsigned long pushInterval = (pushIntervalSec > 0) ? (unsigned long)pushIntervalSec * 1000UL
                                                       : ((strcmp(printer_model, "X1") == 0) ? 30000UL : 300000UL);

    // Manual trigger button detection with debouncing
    int reading = digitalRead(manualTriggerPin);

    // Check if button state changed (noise or press)
    if (reading != lastButtonState) {
        lastDebounceTime = millis();  // Reset debounce timer
    }

    // If enough time has passed since last state change, consider it stable
    if ((millis() - lastDebounceTime) > debounceDelay) {
        // If button state has changed and is now pressed (LOW with pull-up)
        if (reading != currentButtonState) {
            currentButtonState = reading;

            // Button was just pressed (transition to LOW)
            if (currentButtonState == LOW && !motorWaiting && !motorRunning && !delayAfterRunning) {
                buttonPressed = true;
                manualMode = true;  // Set manual mode flag
                addLogEntry("Manual trigger button pressed");
                // Skip wait time and start motors immediately
                motorWaiting = false;
                motorRunning = true;
                motorRunStartTime = millis();
                digitalWrite(yellowLight, LOW);
                digitalWrite(redLight, HIGH);

                if (debug) Serial.println(String("Manual Mode - Moving ") + (motorDirection == 0 ? "Forward" : "Reverse"));

                // Apply duty cycle before enabling motors
                ledcWrite(enable1Pin, dutyCycle);
                ledcWrite(enable2Pin, dutyCycle2);

                // Set motor 1 direction
                if (motorDirection == 0) {
                    digitalWrite(motor1Pin1, LOW);
                    digitalWrite(motor1Pin2, HIGH);
                } else {
                    digitalWrite(motor1Pin1, HIGH);
                    digitalWrite(motor1Pin2, LOW);
                }
                // Set motor 2 direction (same as motor 1)
                if (motorDirection == 0) {
                    digitalWrite(motor2Pin1, LOW);
                    digitalWrite(motor2Pin2, HIGH);
                } else {
                    digitalWrite(motor2Pin1, HIGH);
                    digitalWrite(motor2Pin2, LOW);
                }

                addLogEntry("Manual Mode | MOTOR 1 STARTED | Duty Cycle: " + String(dutyCycle) + " | MOTOR 2 STARTED | Duty Cycle: " + String(dutyCycle2) + " | Direction: " + (motorDirection == 0 ? "Forward" : "Reverse"));

                // OLED notify motor started
                startMotorUI();
            }
        }
    }

    lastButtonState = reading;  // Save current state for next iteration

    if (useMotionSensor && digitalRead(motionSensorPin) == HIGH && !motorWaiting && !motorRunning && !delayAfterRunning) {
        manualMode = false;  // Motion sensor trigger is not manual button mode
        motorWaitStartTime = millis();
        yellowLightStartTime = millis();
        yellowLightState = HIGH;
        digitalWrite(yellowLight, yellowLightState);
        addLogEntry("Motion detected, starting conveyor");
        motorWaiting = true;
        oledShow("Motion sensor", "event detected", "Waiting...", iconWarn);
    }

    // Auto PushAll based on printer model interval
    if (!useMotionSensor && autoPushAllEnabled && client.connected() && (currentMillis - previousMillis >= pushInterval)) {
        previousMillis = currentMillis;
        if (debug) {
            Serial.println("Requesting pushAll...");
            addLogEntry("Requesting pushAll... Push interval: " + String(pushInterval) + " Push interval: " + String(currentMillis - previousMillis));
        }
        publishPushAllMessage();
    }

    // Motor waiting -> start motor
    if (motorWaiting && millis() - motorWaitStartTime >= (motorWaitTime + additionalWaitTime)) {
        motorWaiting = false;
        motorRunning = true;
        motorRunStartTime = millis();
        digitalWrite(yellowLight, LOW);
        digitalWrite(redLight, HIGH);
        if (debug) Serial.println(String("Moving ") + (motorDirection == 0 ? "Forward" : "Reverse"));
        // Apply duty cycle before enabling motor 1
        ledcWrite(enable1Pin, dutyCycle);
        // Apply duty cycle before enabling motor 2
        ledcWrite(enable2Pin, dutyCycle2);

        // Set motor 1 direction
        if (motorDirection == 0) {
            digitalWrite(motor1Pin1, LOW);
            digitalWrite(motor1Pin2, HIGH);
        } else {
            digitalWrite(motor1Pin1, HIGH);
            digitalWrite(motor1Pin2, LOW);
        }
        // Set motor 2 direction (same as motor 1)
        if (motorDirection == 0) {
            digitalWrite(motor2Pin1, LOW);
            digitalWrite(motor2Pin2, HIGH);
        } else {
            digitalWrite(motor2Pin1, HIGH);
            digitalWrite(motor2Pin2, LOW);
        }
        addLogEntry("Conveyor Running | MOTOR 1 STARTED | Duty Cycle: " + String(dutyCycle) + " | MOTOR 2 STARTED | Duty Cycle: " + String(dutyCycle2) + " | Direction: " + (motorDirection == 0 ? "Forward" : "Reverse"));

        // OLED notify motor started by calling UI starter (non-blocking)
        startMotorUI();
    }

    // Motor running UI: render countdown + progress bar
    if (motorRunning) {
        unsigned long elapsed = millis() - motorRunStartTime;
        if (elapsed >= (unsigned long)motorRunTime) {
            // This branch handled below in motor stopping logic; keep rendering until motor stops.
        } else {
            unsigned long remainingMs = (unsigned long)motorRunTime - elapsed;
            int remainingSec = (int)((remainingMs + 500) / 1000); // round to nearest second
            float progress = (float)elapsed / (float)motorRunTime;

            display.clearDisplay();

            // Icon left
            display.drawBitmap(0, 0, iconConveyor, 16, 16, SSD1306_WHITE);

            // Title
            display.setTextSize(1);
            display.setCursor(20, 0);
            display.print("Running!");

            // Big seconds (right)
            drawDigitalNumber(88, 0, remainingSec);

            // Progress bar along bottom
            drawProgressBar(2, 22, 124, 8, progress);

            display.display();
        }
    }

    // Motor running logic: stop motor
    if (motorRunning && millis() - motorRunStartTime >= (unsigned long)motorRunTime) {
        motorRunning = false;

        // Stop motor 1
        digitalWrite(motor1Pin1, LOW);
        digitalWrite(motor1Pin2, LOW);
        // Stop motor 2
        digitalWrite(motor2Pin1, LOW);
        digitalWrite(motor2Pin2, LOW);
        ledcWrite(enable1Pin, 0);
        ledcWrite(enable2Pin, 0);
        digitalWrite(redLight, LOW);
        digitalWrite(yellowLight, LOW);
        digitalWrite(greenLight, HIGH);
        addLogEntry("Motors stopped");

        // For manual mode, skip delay and go straight to default screen
        if (manualMode) {
            manualMode = false;  // Reset flag
            delayAfterRunning = false;
            // Show default screen immediately after brief "done" message
            oledShow("Motor stopped", "DONE", "", iconCheck);
            showPostRunStatus = true;
            postRunShownAt = millis();
        } else {
            // Normal mode: use delay after run
            delayAfterRunning = true;
            delayAfterRunStartTime = millis();
            // OLED notify motor stopped (brief)
            oledShow("Motor stopped", "DONE", "", iconCheck);
            infoHoldStart = millis();
        }
    }

    // Delay after run logic
    if (delayAfterRunning && millis() - delayAfterRunStartTime >= (unsigned long)delayAfterRun) {
        delayAfterRunning = false;
        if (debug) Serial.println("Delay after run complete");
        addLogEntry("Delay after run complete");
        // OLED notify delay complete
        oledShow("Delay after", "run complete", "", nullptr);
        showPostRunStatus = true;        // allow next screen to display
        postRunShownAt = millis();       // timestamp
    }

    // --- Post-run notification logic ---
    if (showPostRunStatus) {
        // wait 2 seconds after "run complete" is shown
        if (millis() - postRunShownAt > 2000) {
            // Show appropriate status based on mode
            if (useMotionSensor) {
                oledShow("Using Motion", "Sensor Mode", "", iconCheck);
            }
            // In MQTT mode updateInfoScreen() takes over from here.

            // only show this one time
            showPostRunStatus = false;
        }
        // No early return: LED blinking and client.loop() must keep running.
        // The idle-status block below is already gated on !showPostRunStatus.
    }

    // Show idle status when using motion sensor and system is idle
    static unsigned long lastIdleStatusUpdate = 0;
    if (useMotionSensor && !motorWaiting && !motorRunning && !delayAfterRunning && !showPostRunStatus) {
        // Update idle status every 5 seconds to ensure it stays visible
        if (currentMillis - lastIdleStatusUpdate > 5000) {
            oledShow("Using Motion", "Sensor Mode", "", iconCheck);
            lastIdleStatusUpdate = currentMillis;
        }
    }

    // Idle info screens (MQTT mode): progress/job rotation once the connect/DONE message has been seen
    if (!useMotionSensor && client.connected() && !motorWaiting && !motorRunning && !showPostRunStatus &&
        !resetButtonPressed && (millis() - infoHoldStart >= INFO_HOLD_TIME)) {
        updateInfoScreen();
    }

    // Handle yellow light flashing based on MQTT/WiFi status
    if (motorWaiting) {
        digitalWrite(greenLight, LOW);
        if (currentMillis - yellowLightStartTime >= 500) {
            yellowLightStartTime = currentMillis;
            yellowLightState = !yellowLightState;
            digitalWrite(yellowLight, yellowLightState);
        }
    }

    if (!useMotionSensor && !client.connected()) {
        if (disconnectedTime == 0) {
            disconnectedTime = millis();  // Mark the time of disconnection
        }

        if (millis() - disconnectedTime >= 5000) {  // Flash only if disconnected for 5+ seconds
            if (currentMillis - yellowLightStartTime >= 500) {
                yellowLightStartTime = currentMillis;
                yellowLightState = !yellowLightState;
                digitalWrite(yellowLight, yellowLightState);
                if (debug) {
                    addLogEntry("MQTT disconnect detected, yellow light flashing due to 5s disconnection.");
                }
            }
        }

        if (millis() - lastAttemptTime >= RECONNECT_INTERVAL) {
            connectToMqtt();
            lastAttemptTime = millis();
        }
    }

    client.loop();
}
