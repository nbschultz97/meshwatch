// BREAZE node — LoRa mesh implementation.

#include "lora_mesh.h"
#include "config.h"

bool LoRaMesh::begin() {
    Serial.print("[BREAZE] init SX1262 ... ");
    int st = _radio.begin();
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("begin() failed: %d\n", st);
        _up = false;
        return false;
    }
    _radio.setFrequency(LORA_FREQUENCY_MHZ);
    _radio.setBandwidth(LORA_BANDWIDTH_KHZ);
    _radio.setSpreadingFactor(LORA_SPREADING_F);
    _radio.setCodingRate(LORA_CODING_RATE);
    _radio.setOutputPower(LORA_TX_POWER_DBM);
    _radio.setPreambleLength(LORA_PREAMBLE_LEN);
    _radio.setSyncWord(LORA_SYNC_WORD);
    _radio.setCRC(true);

    uint64_t mac = ESP.getEfuseMac();
    _nodeId = (uint32_t)(mac & 0xFFFFFFFF);

    Serial.printf("OK  node=0x%08X freq=%.2fMHz sf=%d sync=0x%02X\n",
                  _nodeId, LORA_FREQUENCY_MHZ,
                  LORA_SPREADING_F, LORA_SYNC_WORD);
    _up = true;
    return true;
}

bool LoRaMesh::_tx(uint8_t type, const uint8_t* payload, size_t len) {
    if (!_up) { return false; }
    if (len > 200) { return false; }
    uint8_t buf[256];
    buf[0] = BREAZE_MAGIC_HI;
    buf[1] = BREAZE_MAGIC_LO;
    buf[2] = type;
    buf[3] = nodeIdByte(0);
    buf[4] = nodeIdByte(1);
    buf[5] = nodeIdByte(2);
    buf[6] = nodeIdByte(3);
    buf[7] = 1;                    // ttl=1 (no relay in v1)
    buf[8] = _seq++;               // seq
    if (len > 0) { memcpy(&buf[9], payload, len); }
    size_t total = 9 + len;
    int st = _radio.transmit(buf, total);
    if (st == RADIOLIB_ERR_NONE) {
        _txOk++;
        return true;
    } else {
        Serial.printf("[BREAZE] tx failed: %d\n", st);
        _txFail++;
        return false;
    }
}

bool LoRaMesh::sendBeacon(const uint8_t* payload, size_t len) {
    return _tx(BREAZE_TYPE_BEACON, payload, len);
}

bool LoRaMesh::sendMotion(const uint8_t* payload, size_t len) {
    return _tx(BREAZE_TYPE_MOTION, payload, len);
}

bool LoRaMesh::sendTamper() {
    return _tx(BREAZE_TYPE_TAMPER, nullptr, 0);
}

int LoRaMesh::poll(uint32_t windowMs,
                   void (*onFrame)(uint8_t type, uint8_t* payload, size_t len,
                                   uint32_t srcId, int16_t rssi)) {
    if (!_up) { return 0; }
    int got = 0;
    uint32_t deadline = millis() + windowMs;
    uint8_t buf[256];
    while ((int32_t)(deadline - millis()) > 0) {
        int16_t rssi = 0;
        int st = _radio.receive(buf, sizeof(buf), 0);   // non-blocking-ish
        if (st == RADIOLIB_ERR_NONE) {
            size_t plen = _radio.getPacketLength();
            if (plen < 9) { continue; }
            if (buf[0] != BREAZE_MAGIC_HI || buf[1] != BREAZE_MAGIC_LO) {
                continue;   // not a BREAZE frame — drop silently
            }
            uint8_t  type   = buf[2];
            uint32_t srcId  = (uint32_t)buf[3]
                            | ((uint32_t)buf[4] << 8)
                            | ((uint32_t)buf[5] << 16)
                            | ((uint32_t)buf[6] << 24);
            uint8_t  ttl    = buf[7];
            // uint8_t  seq  = buf[8];   // unused in v1
            uint8_t* payload = &buf[9];
            size_t    plen2  = plen - 9;
            _rxOk++;
            _lastRssi = rssi;
            got++;
            if (onFrame) {
                onFrame(type, payload, plen2, srcId, rssi);
            }
            // (ttl/relay logic goes here in v2)
        } else if (st == RADIOLIB_ERR_RX_TIMEOUT) {
            // Expected; nothing on the air right now.
        } else if (st == RADIOLIB_ERR_CRC_MISMATCH) {
            // Corrupt packet — ignore and keep listening.
        } else {
            // Other error — kick the receiver.
            _radio.startReceive();
        }
        if (millis() >= deadline) { break; }
    }
    return got;
}
