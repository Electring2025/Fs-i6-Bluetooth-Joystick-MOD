/*
 * FlySky FS-i6 PPM → Bluetooth HID Gamepad
 * 6-channel · ArduinoOTA · Deep-sleep power management
 * Target : ESP32-C3
 *
 * ── Wiring ───────────────────────────────────────────────────────
 *   3.5mm Tip    (PPM signal) → GPIO 4
 *   3.5mm Sleeve (GND)        → GND
 *   Wake button (optional)    → GPIO 9 → GND  (boot button on most C3 boards)
 *
 * ── Channel mapping ──────────────────────────────────────────────
 *   CH1 Aileron   → HID X   axis
 *   CH2 Elevator  → HID Y   axis  (inverted)
 *   CH3 Throttle  → HID Z   axis  (inverted)
 *   CH4 Rudder    → HID RZ  axis
 *   CH5 Aux1      → HID Slider1  (0–32767, no deadband)
 *   CH6 Aux2      → HID Slider2  (0–32767, no deadband)
 *
 * ── Power / sleep logic ──────────────────────────────────────────
 *   On every cold boot two parallel 10-second timeouts run:
 *     1. WiFi scan: if WIFI_SSID is not visible within 10 s → WiFi off
 *        (saves ~100 mA); OTA will not be available that session.
 *     2. BLE connect: if no host connects within 10 s → deep sleep.
 *   Once BLE is connected both timers are disarmed permanently.
 *   If BLE disconnects after a session the device deep-sleeps immediately
 *   (PC is gone / user is done) — press the wake button to restart.
 *   Wake from deep sleep: GPIO 9 low (boot button) or power cycle.
 *
 * ── OTA ──────────────────────────────────────────────────────────
 *   pio run -e esp32-c3-ota --target upload
 *   (hostname flysky-esp32c3, password ota1234)
 */

#include <Arduino.h>
#include <BleGamepad.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "driver/gpio.h"

// ── User config ───────────────────────────────────────────────────────────────

#define PPM_PIN          4
#define PPM_CHANNELS     6
#define PPM_SYNC_US      2700
#define PPM_MIN_US       900
#define PPM_MAX_US       2100
#define DEADBAND_US      20
#define AXIS_MIN        -32767
#define AXIS_MAX         32767

#define WIFI_SSID        "your-ssid"
#define WIFI_PASS        "your-password"
#define OTA_HOSTNAME     "flysky-esp32c3"
#define OTA_PASSWORD     "ota1234"

// Seconds after boot before giving up on BLE or WiFi
#define BLE_CONNECT_TIMEOUT_S   5
#define WIFI_SCAN_TIMEOUT_S     10

// GPIO used to wake from deep sleep (boot button = GPIO 9 on most C3 boards)
#define WAKE_GPIO        GPIO_NUM_9

#define PPM_DEBUG        true   // set false to silence serial in production

// ── PPM ISR state ─────────────────────────────────────────────────────────────

static volatile uint16_t ppmRaw[PPM_CHANNELS] = {1500, 1500, 1500, 1500, 1500, 1500};
static volatile uint8_t  ppmChanIdx    = 0;
static volatile uint32_t ppmLastEdge   = 0;
static volatile bool     ppmFrameReady = false;

// ── BLE ───────────────────────────────────────────────────────────────────────

static BleGamepad              bleGamepad;
static BleGamepadConfiguration bleGamepadConfig;

// ── Runtime flags ─────────────────────────────────────────────────────────────

static bool bleEverConnected = false;   // latched true on first connection
static bool otaReady         = false;   // true when WiFi + OTA initialised
static uint32_t bootMs       = 0;       // millis() at end of setup()

// ── Deep sleep helper ─────────────────────────────────────────────────────────

static void goToSleep(const char* reason)
{
#if PPM_DEBUG
    Serial.printf("[PWR] Sleeping: %s\n", reason);
    Serial.flush();
    delay(50);
#endif
    // Disable all peripherals cleanly before sleep
    detachInterrupt(digitalPinToInterrupt(PPM_PIN));
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    btStop();

    // ESP32-C3 uses GPIO wakeup — EXT0 is Xtensa-core only
    // Wake on GPIO 9 going LOW (boot button → GND)
    gpio_wakeup_enable(WAKE_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    esp_deep_sleep_start();
    // never returns
}

// ── PPM ISR ───────────────────────────────────────────────────────────────────

void IRAM_ATTR ppmISR()
{
    const uint32_t now   = micros();
    const uint32_t pulse = now - ppmLastEdge;
    ppmLastEdge = now;

    if (pulse > PPM_SYNC_US) {
        if (ppmChanIdx > 0) ppmFrameReady = true;
        ppmChanIdx = 0;
        return;
    }
    if (ppmChanIdx < PPM_CHANNELS)
        ppmRaw[ppmChanIdx++] = (uint16_t)constrain((long)pulse,
                                                    PPM_MIN_US, PPM_MAX_US);
}

// ── Axis helpers ──────────────────────────────────────────────────────────────

static int32_t usToAxis(uint16_t us, bool invert)
{
    int32_t c = (int32_t)us - 1500;
    if (abs(c) <= DEADBAND_US) c = 0;
    int32_t v = constrain(map(c, -500, 500, AXIS_MIN, AXIS_MAX),
                          AXIS_MIN, AXIS_MAX);
    return invert ? -v : v;
}

static int32_t usToSlider(uint16_t us)
{
    return constrain(map((long)us, 1000, 2000, 0, AXIS_MAX), AXIS_MIN, AXIS_MAX);
}

// ── WiFi scan + OTA init ──────────────────────────────────────────────────────
//
// Run once, non-blocking relative to the BLE timeout: we do a passive WiFi
// scan first (≤ WIFI_SCAN_TIMEOUT_S seconds), then only start the full
// WiFi stack if the SSID is actually visible.  If the SSID is absent we
// turn WiFi off immediately so it doesn't drain current.

static void initOTA()
{
    Serial.printf("[OTA] Scanning for '%s'…\n", WIFI_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Non-blocking scan — results ready in ~1-3 s typically
    WiFi.scanNetworks(/*async=*/true);

    const uint32_t scanStart = millis();
    bool ssidFound = false;

    while ((millis() - scanStart) < (WIFI_SCAN_TIMEOUT_S * 1000UL)) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_RUNNING) {
            delay(200);
            continue;
        }
        if (n <= 0) {
            // Scan error or 0 networks
            break;
        }
        for (int i = 0; i < n; i++) {
            if (WiFi.SSID(i) == WIFI_SSID) {
                ssidFound = true;
                break;
            }
        }
        WiFi.scanDelete();
        break;
    }

    if (!ssidFound) {
        Serial.println("[OTA] SSID not found — WiFi disabled for this session");
        WiFi.mode(WIFI_OFF);    // kills radio, saves ~100 mA
        return;
    }

    Serial.printf("[OTA] SSID found, connecting…");
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    const uint32_t connStart = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - connStart) < 8000UL) {
        delay(300);
        Serial.print('.');
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[OTA] Connect failed — WiFi disabled");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return;
    }

    Serial.printf("\n[OTA] IP: %s\n", WiFi.localIP().toString().c_str());

    // Reduce WiFi TX power — we only need LAN range
    esp_wifi_set_max_tx_power(44);   // 11 dBm  (default is 80 = 20 dBm)

    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        detachInterrupt(digitalPinToInterrupt(PPM_PIN));
        Serial.println("[OTA] Flashing…");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\n[OTA] Done");
    });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
        Serial.printf("[OTA] %u%%\r", p * 100 / t);
    });
    ArduinoOTA.onError([](ota_error_t e) {
        Serial.printf("[OTA] Error %u\n", e);
        attachInterrupt(digitalPinToInterrupt(PPM_PIN), ppmISR, RISING);
    });

    ArduinoOTA.begin();
    otaReady = true;
    Serial.printf("[OTA] Ready — hostname: %s\n", OTA_HOSTNAME);
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup()
{
#if PPM_DEBUG
    Serial.begin(115200);
    Serial.println("\n[PPM-BLE] FlySky FS-i6 6ch BLE Gamepad");

    // Report why we woke up (useful during development)
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_EXT0)
        Serial.println("[PWR] Woke via boot button");
    else
        Serial.println("[PWR] Cold boot / power-on");
#endif

    // ── BLE gamepad ─────────────────────────────────────────────
    bleGamepadConfig.setAxesMin(AXIS_MIN);
    bleGamepadConfig.setAxesMax(AXIS_MAX);
    bleGamepadConfig.setAutoReport(false);
    bleGamepadConfig.setControllerType(CONTROLLER_TYPE_GAMEPAD);
    bleGamepadConfig.setHidReportId(1);
    bleGamepadConfig.setWhichAxes(true,   // X   CH1 Aileron
                                  true,   // Y   CH2 Elevator
                                  true,   // Z   CH3 Throttle
                                  false,  // Rx  unused
                                  false,  // Ry  unused
                                  true,   // Rz  CH4 Rudder
                                  true,   // Sl1 CH5 Aux1
                                  true);  // Sl2 CH6 Aux2
    bleGamepadConfig.setWhichSimulationControls(false, false, false, false, false);

    bleGamepad.begin(&bleGamepadConfig);

    // ── PPM input ────────────────────────────────────────────────
    pinMode(PPM_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(PPM_PIN), ppmISR, RISING);

    // ── WiFi / OTA (parallel to BLE timeout window) ─────────────
    // initOTA() blocks for up to WIFI_SCAN_TIMEOUT_S seconds while scanning.
    // This is acceptable — BLE advertising starts independently in the
    // background via bleGamepad.begin(), so the BLE timeout clock has
    // effectively started; we just haven't checked it yet.
    initOTA();

    bootMs = millis();

#if PPM_DEBUG
    Serial.printf("[PWR] BLE connect window: %d s\n", BLE_CONNECT_TIMEOUT_S);
    Serial.println("[PPM-BLE] Advertising…");
#endif
}

// ── Loop ──────────────────────────────────────────────────────────────────────

void loop()
{
    // ── OTA handler (μs overhead when idle) ─────────────────────
    if (otaReady) ArduinoOTA.handle();

    // ── BLE connection timeout ───────────────────────────────────
    // Only active until the first successful connection.
    if (!bleEverConnected) {
        if (bleGamepad.isConnected()) {
            bleEverConnected = true;
#if PPM_DEBUG
            Serial.println("[BLE] Connected");
#endif
        } else {
            uint32_t elapsed = millis() - bootMs;
            if (elapsed >= (BLE_CONNECT_TIMEOUT_S * 1000UL)) {
                goToSleep("no BLE connection within timeout");
            }
        }
    }

    // ── Post-connection disconnect → sleep ───────────────────────
    // If we had a session and the host disconnected, go to sleep.
    // The user can wake with the boot button.
    if (bleEverConnected && !bleGamepad.isConnected()) {
        goToSleep("BLE disconnected after session");
    }

    // ── PPM frame processing ─────────────────────────────────────
    if (!ppmFrameReady) {
        delay(1);
        return;
    }

    uint16_t ch[PPM_CHANNELS];
    noInterrupts();
    memcpy(ch, (const void *)ppmRaw, sizeof(ch));
    ppmFrameReady = false;
    interrupts();

#if PPM_DEBUG
    Serial.printf("[PPM] %4u %4u %4u %4u %4u %4u µs\n",
                  ch[0], ch[1], ch[2], ch[3], ch[4], ch[5]);
#endif

    if (bleGamepad.isConnected()) {
        bleGamepad.setX     (usToAxis  (ch[0], false));
        bleGamepad.setY     (usToAxis  (ch[1], true));
        bleGamepad.setZ     (usToAxis  (ch[2], true));
        bleGamepad.setRZ    (usToAxis  (ch[3], false));
        bleGamepad.setSlider1(usToSlider(ch[4]));
        bleGamepad.setSlider2(usToSlider(ch[5]));
        bleGamepad.sendReport();
    }
}
