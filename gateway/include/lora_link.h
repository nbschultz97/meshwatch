// lora_link.h — SX1262 driver wrapper + simple mesh layer.
//
// One unified protocol over LoRa and BLE: same frame types (0x00 END,
// 0x01 NODE, 0x02 MSG, 0x03 IMAGE) flow in both directions. The LoRa header
// adds addressing + TTL so multiple Heltecs can relay; the BLE layer is
// point-to-point and skips the header.
//
// LoRa packet format (max 250 bytes on-air):
//   [dst:1] [src:1] [ttl:1] [hop:1] [seq:1] [type:1] [payload... <= 244]
//
//   dst:    0x00..0xFE = unicast to that gateway id, 0xFF = broadcast
//   src:    sender's gateway id (assigned at boot from ESP32 MAC low byte)
//   ttl:    starts at 3, decremented per hop, dropped at 0
//   hop:    number of times this packet has been relayed (0 on first send)
//   seq:    per-source counter; receivers drop duplicates
//   type:   same as BLE type byte (0x00..0x03); payload semantics identical
//
// Bridging:
//   BLE RX cmd 0x02 SEND -> wrap text as LoRa type 0x02, broadcast, TTL=3.
//   LoRa RX     -> strip header, push payload onto BLE txQueue (so the
//                  watch sees it as just another incoming message).
//
// SX1262 wiring (Heltec WiFi LoRa 32 V3, per pins_arduino.h):
//   NSS=8, SCK=9, MOSI=10, MISO=11, RST=12, BUSY=13, DIO1=14
// No TCXO control needed on V3 (DIO2 not wired).

#pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include <deque>
#include <vector>
#include <stdint.h>

class LoRaLink {
public:
    // Called from setup(). Returns true if the radio came up.
    bool begin();

    // True if the radio initialised successfully.
    bool isUp() const { return _up; }

    // Our gateway id (low byte of ESP32 MAC).
    uint8_t myId() const { return _myId; }

    // Wrap and broadcast a BLE frame over LoRa. type is the same as BLE's
    // first byte (0x01 NODE, 0x02 MSG, 0x03 IMAGE, ...). payload is the
    // body after the type byte. txQueue side-effect: BLE frames flow out
    // to the watch; this method lets them also flow over the air.
    void broadcastFrame(uint8_t type, const uint8_t* payload, size_t len);

    // Pump the receive queue; call this from loop(). For each ready packet,
    // invoke the onFrame callback with (type, payload, len, src, rssi).
    // (No copy needed — the callback runs on this stack.)
    void poll(void (*onFrame)(uint8_t type, const uint8_t* payload, size_t len,
                              uint8_t src, int16_t rssi));

    // Stats for the OLED / serial log.
    uint32_t txCount() const { return _txCount; }
    uint32_t rxCount() const { return _rxCount; }
    int16_t   lastRssi() const { return _lastRssi; }

private:
    // SX1262 wiring for Heltec WiFi LoRa 32 V3 (per pins_arduino.h):
    //   NSS=SS=8, SCK=9, MOSI=10, MISO=11, RST=RST_LoRa=12, BUSY=BUSY_LoRa=13,
    //   DIO1=DIO0=14. RadioLib Module() is (CS, IRQ, RST, BUSY). The first
    //   cut had RST and BUSY swapped which left the chip unresponsive
    //   (RADIOLIB_ERR_CHIP_NOT_FOUND = -2).
    SX1262 _radio = new Module(SS, 14, RST_LoRa, BUSY_LoRa);
    bool    _up = false;
    uint8_t _myId = 0;
    uint8_t _seq = 0;
    uint32_t _txCount = 0;
    uint32_t _rxCount = 0;
    int16_t  _lastRssi = 0;

    // Tiny dedup window: (src<<8)|seq -> last-seen millis. Drops rebroadcasts.
    struct DedupKey { uint16_t key; uint32_t ms; };
    DedupKey _seen[64];
    uint8_t  _seenIdx = 0;

    bool _seenRecently(uint8_t src, uint8_t seq, uint32_t now);
};
