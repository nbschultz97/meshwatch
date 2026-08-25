// BREAZE node — config
//
// Cheap solar-powered sentry node: Heltec V3 + PIR + 18650, deployed in a
// Harbor Freight solar walkway light housing. Joins the PICKET mesh
// (915 MHz / SF12 / BW125 / sync 0x13) with a separate address space
// (BREAZE frames don't interfere with PICKET's HMAC cues).
//
// Pin map (Heltec WiFi LoRa 32 V3, per pins_arduino.h):
//   SX1262: NSS=8, SCK=9, MOSI=10, MISO=11, RST=12, BUSY=13, DIO1=14
//   PIR:   GPIO1 (A0 on V3) — low quiescent, HIGH on motion
//   LDR:   GPIO2 (A1 on V3) — light-dependent resistor divider
//   BATT:  GPIO4 (A2) — voltage divider on 18650 (1:2 to stay under 3.3V)
//   BUTTON:GPIO0 (BOOT) — held at boot = provisioning/OTA mode
//   LED:   GPIO35 (built-in)
#pragma once

#include <Arduino.h>

// ---- LoRa (matches PICKET exactly so BREAZE nodes are heard alongside
// PICKET cues on the same air) -------------------------------------------
#define LORA_FREQUENCY_MHZ  915.0
#define LORA_BANDWIDTH_KHZ  125.0
#define LORA_SPREADING_F    12
#define LORA_CODING_RATE    8     // 4/8
#define LORA_TX_POWER_DBM   22
#define LORA_PREAMBLE_LEN   12
#define LORA_SYNC_WORD      0x13  // PICKET's private word; foreign traffic
                                  // (Meshtastic = 0x2B, Heliograph = 0x2D)
                                  // is rejected at the SX1262 IRQ level.

// ---- Frame magic. PICKET's framer expects specific 38-byte HMAC frames;
// ours starts with 0xBEEF so it doesn't trigger their parser. -------------
#define BREAZE_MAGIC_HI     0xBE
#define BREAZE_MAGIC_LO     0xEF

// ---- Frame types --------------------------------------------------------
#define BREAZE_TYPE_BEACON  0x01  // periodic: batt, temp, hum, light, RSSI
#define BREAZE_TYPE_MOTION  0x02  // immediate: PIR trigger + counters
#define BREAZE_TYPE_TAMPER  0x03  // housing opened / accelerometer spike
#define BREAZE_TYPE_CONFIG  0x04  // gateway-issued config (ack from node)
#define BREAZE_TYPE_PING    0x05  // gateway wants a beacon; node replies
#define BREAZE_TYPE_RELAY   0x10  // wrapped foreign-protocol frame
                                 // (PICKET cue, Heliograph msg, etc.) that
                                 // we picked up and rebroadcast so peers
                                 // downstream can hear it. Each BREAZE node
                                 // extends the mesh by 1-2 hops for free.

// ---- Sleep/wake intervals -----------------------------------------------
#define BEACON_INTERVAL_MS  (5UL * 60UL * 1000UL)   // 5 min
#define RX_WINDOW_INTERVAL_MS (60UL * 1000UL)       // 1 min
#define RX_WINDOW_DURATION_MS 200UL                 // 200 ms listening per window
#define MOTION_COOLDOWN_MS  (30UL * 1000UL)         // don't re-alert within 30s
#define TAMPER_COOLDOWN_MS  (5UL  * 60UL * 1000UL)  // 5 min between tamper alerts

// ---- Sensors ------------------------------------------------------------
#define PIR_PIN            1     // GPIO1 / A0
#define LDR_PIN            2     // GPIO2 / A1 — light sensor
#define BATT_PIN           4     // GPIO4 / A2 — 18650 via 100k/100k divider
#define LED_PIN            35    // built-in LED
#define BUTTON_PIN         0     // BOOT button — held at boot = provisioning

// PIR thresholds — HC-SR312 is digital, so this is just a debounce.
#define PIR_DEBOUNCE_MS    50

// LDR thresholds (12-bit ADC: 0..4095, with 100k pull-up + LDR to GND)
#define LDR_DARK_THRESHOLD    800   // below this = "dark" (event)
#define LDR_BRIGHT_THRESHOLD  3500  // above this = "someone flashed a light"

// Battery voltage thresholds (12-bit ADC at 1:2 divider of 18650)
#define BATT_EMPTY_MV      3200  // ~0% — auto-hibernate below this
#define BATT_FULL_MV       4200  // ~100% — fully charged 18650
#define BATT_LOW_PCT       20    // transmit a low-batt warning below this

// ---- OTA / provisioning -------------------------------------------------
#define BUTTON_HOLD_MS     3000  // hold BOOT this long at boot for OTA AP
#define AP_SSID_PREFIX     "BREAZE-"
#define AP_PASS            "breaze123"  // for OTA only; nodes don't trust
                                       // network traffic without HMAC
