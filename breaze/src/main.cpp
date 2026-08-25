// BREAZE node — solar-powered sentry.
//
// Loop is simple and power-aware:
//   1. Boot → init sensors + LoRa (matches PICKET air params)
//   2. If BOOT held >=3s, stay awake in provisioning AP mode (v2: OTA)
//   3. Otherwise: deep sleep. Wake on:
//        - PIR (motion)
//        - Tamper (GPIO IRQ)
//        - RTC timer (beacon every 5 min)
//        - RX window (every 60s, 200ms — listens for a gateway config)
//   4. On wake: read sensors, build a beacon/motion/tamper payload, TX it.
//   5. Back to deep sleep.
//
// Power budget (Heltec V3 + PIR + 18650):
//   - deep sleep:  ~0.01 W (ESP32-S3 deep sleep + RTC + PIR IRQ only)
//   - beacon TX:   ~0.5 W for ~3 s every 5 min = 0.005 W avg
//   - RX window:   ~0.1 W for 200 ms every 60 s = 0.0003 W avg
//   - motion TX:   ~0.5 W for ~3 s on PIR trigger (rare)
//   - total avg:   ~0.015 W
//   - 18650 (10 Wh) → ~28 days standalone
//   - walkway solar panel (~0.2 Wh/day) → indefinite with any sun

#include <Arduino.h>
#include "config.h"
#include "lora_mesh.h"

static LoRaMesh mesh;

// Counters survive deep sleep via RTC slow memory.
RTC_DATA_ATTR static uint32_t motionTotal = 0;
RTC_DATA_ATTR static uint32_t motion24h = 0;
RTC_DATA_ATTR static uint32_t lastMotionMs = 0;
RTC_DATA_ATTR static uint32_t bootCount = 0;
RTC_DATA_ATTR static uint32_t lastBeaconMs = 0;

static uint8_t battMv = 0;        // last read; refreshed each wake
static int8_t  tempC = 0;
static uint16_t lightRaw = 0;
static bool     motionPending = false;     // set by ISR, cleared on TX
static bool     tamperPending = false;

static void IRAM_ATTR onPir() {
    motionPending = true;
}
static void IRAM_ATTR onTamper() {
    tamperPending = true;
}

// ---- Sensor helpers --------------------------------------------------

static uint16_t readBattMv() {
    // 1:2 divider on 18650 → max ~2.1V at the pin. ADC reads 0..3.3V.
    // The divider is reversed from a normal V-divider (top to GND): we
    // assume R1=R2=100k with the cell between R1 and the ADC pin. So the
    // ADC sees half the cell voltage. Convert raw ADC to mV:
    //   mv = raw * 3300 / 4095 * 2
    int raw = analogRead(BATT_PIN);
    long mv = (long)raw * 6600L / 4095L;
    if (mv < 0) { mv = 0; }
    if (mv > 9999) { mv = 9999; }
    return (uint16_t)mv;
}

static uint8_t battPct(uint16_t mv) {
    if (mv <= BATT_EMPTY_MV) { return 0; }
    if (mv >= BATT_FULL_MV)  { return 100; }
    int pct = (int)((mv - BATT_EMPTY_MV) * 100L /
                    (BATT_FULL_MV - BATT_EMPTY_MV));
    if (pct < 0) { pct = 0; }
    if (pct > 100) { pct = 100; }
    return (uint8_t)pct;
}

static int8_t readTempC() {
    // ESP32-S3 internal temp sensor is unreliable (±5°C) but fine for a
    // "is it unusually hot/cold" beacon. For accurate readings, attach a
    // BME280 on I2C and read it here instead (v2).
    return (int8_t)temperatureRead();   // Arduino-ESP32 builtin
}

static uint16_t readLightRaw() {
    return (uint16_t)analogRead(LDR_PIN);
}

// ---- Beacon / alert payload builders --------------------------------

static void buildBeacon(uint8_t* out, size_t len = 8) {
    uint16_t mv = readBattMv();
    out[0] = mv & 0xFF;
    out[1] = (mv >> 8) & 0xFF;
    out[2] = battPct(mv);
    out[3] = (uint8_t)(int8_t)readTempC();
    out[4] = lightRaw & 0xFF;
    out[5] = (lightRaw >> 8) & 0xFF;
    out[6] = (motion24h > 255) ? 255 : (uint8_t)motion24h;
    out[7] = 0;   // reserved flags
}

static void buildMotion(uint8_t* out, size_t len = 5) {
    uint16_t mv = readBattMv();
    out[0] = mv & 0xFF;
    out[1] = (mv >> 8) & 0xFF;
    out[2] = lightRaw & 0xFF;
    out[3] = (lightRaw >> 8) & 0xFF;
    out[4] = (motionTotal > 255) ? 255 : (uint8_t)motionTotal;
}

// ---- Wake handlers ---------------------------------------------------

static void handleMotionWake() {
    motionTotal++;
    motion24h++;
    lastMotionMs = millis();
    uint8_t payload[5];
    buildMotion(payload);
    mesh.sendMotion(payload);
    Serial.printf("[BREAZE] motion #%u, light=%u batt=%umv\n",
                  (unsigned)motionTotal, lightRaw, readBattMv());
}

static void handleTamperWake() {
    uint8_t payload[5];
    buildMotion(payload);   // include batt+light in tamper too
    mesh.sendTamper();
    Serial.println("[BREAZE] TAMPER");
}

static void handleBeaconWake() {
    battMv = readBattMv();
    tempC  = readTempC();
    lightRaw = readLightRaw();
    uint8_t payload[8];
    buildBeacon(payload);
    mesh.sendBeacon(payload);
    Serial.printf("[BREAZE] beacon batt=%umv temp=%dC light=%u motion24h=%u\n",
                  battMv, tempC, lightRaw, (unsigned)motion24h);
}

static void handleRxWake() {
    int got = mesh.poll(RX_WINDOW_DURATION_MS, nullptr);
    if (got > 0) {
        Serial.printf("[BREAZE] rx window: %d frame(s)\n", got);
    }
}

// ---- Sleep entry -----------------------------------------------------

static void goSleep(uint32_t ms, bool enablePir) {
    esp_sleep_enable_timer_wakeup(ms * 1000ULL);   // ms → µs
    if (enablePir) {
        // PIR signal goes HIGH on motion. Wake on HIGH.
        esp_sleep_enable_ext0_wakeup((gpio_num_t)PIR_PIN, 1);
    }
    Serial.flush();
    esp_deep_sleep_start();
    // unreachable
}

// ---- setup / loop ---------------------------------------------------

void setup() {
    Serial.begin(115200);
    bootCount++;
    delay(50);
    Serial.printf("\nBREAZE node booting (#%u)\n", (unsigned)bootCount);

    // Provisioning mode: BOOT held at boot → stay awake forever, no sleep.
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    if (digitalRead(BUTTON_PIN) == LOW) {
        // Hold for 3s to confirm. If released early, fall through to normal.
        uint32_t t0 = millis();
        while (digitalRead(BUTTON_PIN) == LOW && millis() - t0 < BUTTON_HOLD_MS) {
            delay(10);
        }
        if (millis() - t0 >= BUTTON_HOLD_MS) {
            Serial.println("[BREAZE] provisioning mode (no sleep, USB serial on)");
            // v2: start WiFi AP + web portal here. For now: just stay awake.
            while (true) {
                handleBeaconWake();
                delay(5000);
            }
        }
    }

    // PIR / LDR / batt — all analog except PIR (digital).
    pinMode(PIR_PIN, INPUT);   // HC-SR312 has its own pull-up
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Wake-on-motion: ISR sets the flag, the wake handler clears it.
    motionPending = false;
    attachInterrupt(digitalPinToInterrupt(PIR_PIN), onPir, RISING);

    if (!mesh.begin()) {
        // LoRa failed — sleep and retry next wake.
        Serial.println("[BREAZE] LoRa down; sleeping 60s and retrying");
        goSleep(60UL * 1000UL, true);
    }

    // Read sensors once for the first beacon.
    battMv   = readBattMv();
    tempC    = readTempC();
    lightRaw = readLightRaw();

    Serial.printf("[BREAZE] up: batt=%umv temp=%dC light=%u\n",
                  battMv, tempC, lightRaw);
}

void loop() {
    // We're here for one of three reasons:
    //   A) Just woke from sleep (most common)
    //   B) Fresh boot (rare — handled in setup())
    //
    // Decide what to do based on what woke us + elapsed time.
    uint32_t now = millis();
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    bool didPir   = (cause == ESP_SLEEP_WAKEUP_EXT0);
    bool didTimer = (cause == ESP_SLEEP_WAKEUP_TIMER);

    if (didPir && motionPending) {
        motionPending = false;
        handleMotionWake();
        // Re-arm PIR and sleep for the next beacon window.
        goSleep(BEACON_INTERVAL_MS, true);
        return;
    }

    if (tamperPending) {
        tamperPending = false;
        handleTamperWake();
        goSleep(TAMPER_COOLDOWN_MS, true);
        return;
    }

    // Periodic wake: beacon, then RX window, then back to sleep.
    handleBeaconWake();
    lastBeaconMs = now;

    // Brief RX window to listen for gateway pings/configs.
    handleRxWake();

    // Update 24h motion counter at midnight-ish. Cheap estimate: every 1440
    // beacons (5 min apart = 12 h), halve the counter. Drift is fine.
    if ((bootCount & 0x3F) == 0) {   // every 64th wake ~= every ~5.3 h
        motion24h = motion24h / 2;
    }

    goSleep(BEACON_INTERVAL_MS, true);
}
