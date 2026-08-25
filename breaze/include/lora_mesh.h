// BREAZE node — LoRa mesh wrapper.
//
// Air interface matches PICKET exactly (915 MHz / SF12 / BW125 / sync 0x13)
// so a BREAZE node is heard by any SX1262 receiver on the same air. Frames
// use a different magic header (0xBEEF) so PICKET's HMAC framer ignores them
// and BREAZE doesn't try to verify PICKET's HMAC.
//
// Frame format (single-hop; mesh routing is v2):
//
//   [0xBE][0xEF][type:1][node_id:4][ttl:1][seq:1][payload...]
//
//   type:     BREAZE_TYPE_* (beacon/motion/tamper/config/ping)
//   node_id:  low 4 bytes of ESP32 MAC, unique per board
//   ttl:      hops remaining (v2 — always 1 in single-hop v1)
//   seq:      per-source counter (wraps at 255)
//
// Beacon payload (8 bytes):
//   [batt_mv:2LE][batt_pct:1][temp_c:1 signed][light_raw:2LE][motion_24h:1]
//
// Motion payload (5 bytes):
//   [batt_mv:2LE][light_raw:2LE][motion_total:1]
//
// Config payload (variable, v2):
//   [interval_min:1][pir_threshold:1][flags:1]...

#pragma once

#include <Arduino.h>
#include <RadioLib.h>
#include <stdint.h>

class LoRaMesh {
public:
    bool begin();
    bool isUp() const { return _up; }

    // Our node id (low 4 bytes of ESP32 MAC).
    uint32_t nodeId() const { return _nodeId; }
    uint8_t  nodeIdByte(size_t i) const { return (_nodeId >> (i * 8)) & 0xFF; }

    // Send a beacon. payload points to 8 bytes (batt_mv + batt_pct + temp
    // + light + motion_24h). Returns true on TX success.
    bool sendBeacon(const uint8_t* payload, size_t len = 8);

    // Send a motion alert. payload is the 5-byte motion body.
    bool sendMotion(const uint8_t* payload, size_t len = 5);

    // Send a tamper event. payload is 0 bytes (just the header).
    bool sendTamper();

    // Briefly enter RX mode for `windowMs`, fire onFrame for each valid
    // BREAZE frame received (callback runs on this stack). Returns the
    // number of frames received.
    int poll(uint32_t windowMs,
             void (*onFrame)(uint8_t type, uint8_t* payload, size_t len,
                             uint32_t srcId, int16_t rssi));

    // Last-frame stats.
    int16_t   lastRssi() const { return _lastRssi; }
    uint32_t  txOk()      const { return _txOk; }
    uint32_t  rxOk()      const { return _rxOk; }
    uint32_t  txFail()    const { return _txFail; }

private:
    // SX1262 wiring (Heltec V3, per pins_arduino.h):
    //   NSS=SS=8, SCK=9, MOSI=10, MISO=11, RST=RST_LoRa=12, BUSY=BUSY_LoRa=13,
    //   DIO1=DIO0=14. Module(CS, IRQ, RST, BUSY).
    SX1262 _radio = new Module(SS, 14, RST_LoRa, BUSY_LoRa);

    bool    _up = false;
    uint32_t _nodeId = 0;
    uint8_t  _seq = 0;
    uint32_t _txOk = 0, _txFail = 0, _rxOk = 0;
    int16_t  _lastRssi = 0;

    // Generic TX helper: writes a [magic][type][id4][ttl][seq][payload] frame.
    bool _tx(uint8_t type, const uint8_t* payload, size_t len);
};
