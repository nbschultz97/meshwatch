// BREAZE node — LoRa mesh implementation.

#include "lora_mesh.h"
#include "config.h"

// How long we remember "we already relayed this frame" to break loops.
// LoRa airtime at SF12/125 is ~3 s/frame; a frame can plausibly live 15 s
// across 3 hops before TTL expires. 30 s is a safe window.
#define RELAY_DEDUP_WINDOW_MS  30000UL

// Tiny dedup ring: key = (srcId ^ seq), value = millis() when relayed.
struct DedupEntry { uint32_t key; uint32_t ms; };
static DedupEntry _dedup[32];
static uint8_t _dedupIdx = 0;

static bool alreadyRelayed(uint32_t srcId, uint8_t seq, uint32_t now) {
    uint32_t k = srcId ^ ((uint32_t)seq << 24);
    for (auto& e : _dedup) {
        if (e.key == k && (now - e.ms) < RELAY_DEDUP_WINDOW_MS) {
            return true;
        }
    }
    _dedup[_dedupIdx] = { k, now };
    _dedupIdx = (_dedupIdx + 1) % (sizeof(_dedup) / sizeof(_dedup[0]));
    return false;
}

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

    Serial.printf("OK  node=0x%08X freq=%.2fMHz sf=%d sync=0x%02X (relay=on)\n",
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
    buf[7] = 1;                    // ttl=1 (single-hop on transmit)
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
        int st = _radio.receive(buf, sizeof(buf), 0);
        if (st == RADIOLIB_ERR_NONE) {
            size_t plen = _radio.getPacketLength();
            if (plen < 2) { continue; }

            // Branch A: BREAZE frame — parse normally + consider relaying.
            if (plen >= 9 &&
                buf[0] == BREAZE_MAGIC_HI && buf[1] == BREAZE_MAGIC_LO)
            {
                uint8_t  type  = buf[2];
                uint32_t srcId = (uint32_t)buf[3]
                               | ((uint32_t)buf[4] << 8)
                               | ((uint32_t)buf[5] << 16)
                               | ((uint32_t)buf[6] << 24);
                uint8_t  ttl   = buf[7];
                uint8_t  seq   = buf[8];

                _rxOk++;
                _lastRssi = rssi;
                got++;
                if (onFrame) {
                    onFrame(type, &buf[9], plen - 9, srcId, rssi);
                }

                // Relay logic for BREAZE frames: if ttl > 1 and we're not
                // the source, retransmit with ttl-1. Dedup against loops.
                if (ttl > 1 && srcId != _nodeId) {
                    uint32_t now = millis();
                    if (!alreadyRelayed(srcId, seq, now)) {
                        buf[7] = ttl - 1;     // decrement TTL
                        // brief backoff so we don't collide with the
                        // original sender's retransmit
                        delay(50 + (random(0, 100)));
                        int rst = _radio.transmit(buf, plen);
                        if (rst == RADIOLIB_ERR_NONE) {
                            _txOk++;
                        } else {
                            _txFail++;
                        }
                    }
                }
            }
            // Branch B: foreign frame on the same air (PICKET cue,
            // Heliograph msg, anything). Wrap it as a BREAZE_TYPE_RELAY
            // and rebroadcast so peers downstream can hear it. The wrap
            // preserves the raw bytes verbatim.
            else {
                // Build a RELAY frame: [magic][type=0x10][our_id:4]
                // [ttl=2][seq][original_len:1][original_bytes...]
                uint8_t relay[260];
                if (plen > 240) { plen = 240; }   // leave room for header
                relay[0] = BREAZE_MAGIC_HI;
                relay[1] = BREAZE_MAGIC_LO;
                relay[2] = BREAZE_TYPE_RELAY;
                relay[3] = nodeIdByte(0);
                relay[4] = nodeIdByte(1);
                relay[5] = nodeIdByte(2);
                relay[6] = nodeIdByte(3);
                relay[7] = 2;                       // ttl=2 — two more hops
                relay[8] = _seq++;                  // seq
                relay[9] = (uint8_t)plen;           // original length
                memcpy(&relay[10], buf, plen);
                size_t total = 10 + plen;

                // Dedup: key off the foreign frame's first 4 bytes
                // (good enough to spot most retransmits of the same packet).
                uint32_t fkey = 0;
                for (size_t i = 0; i < plen && i < 4; i++) {
                    fkey = (fkey << 8) | buf[i];
                }
                uint32_t now = millis();
                if (!alreadyRelayed(fkey, (uint8_t)(now & 0xFF), now)) {
                    delay(50 + (random(0, 100)));
                    int rst = _radio.transmit(relay, total);
                    if (rst == RADIOLIB_ERR_NONE) {
                        _txOk++;
                    } else {
                        _txFail++;
                    }
                }
            }
        } else if (st == RADIOLIB_ERR_RX_TIMEOUT) {
            // expected; nothing on the air
        } else if (st == RADIOLIB_ERR_CRC_MISMATCH) {
            // corrupt — ignore
        } else {
            _radio.startReceive();
        }
        if (millis() >= deadline) { break; }
    }
    return got;
}
