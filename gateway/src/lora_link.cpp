// lora_link.cpp — SX1262 driver wrapper + simple mesh layer.

#include "lora_link.h"

// US915 sub-band 2 (channels 8-15, 903.125 - 904.875 MHz, 125 kHz BW),
// matching Meshtastic defaults for US. Other regions can be added later
// by changing the .setFrequency / .setBandwidth calls below.
#define LORA_FREQUENCY      904.6   // a midpoint sub-band-2 channel
#define LORA_BANDWIDTH      125.0
#define LORA_SPREADING      11      // SF11 (Meshtastic default long-range)
#define LORA_TX_POWER       17      // dBm — board limit is ~17 on V3
#define LORA_PREAMBLE_LEN   16
#define LORA_CODING_RATE    5       // 4/5
#define LORA_SYNC_WORD      0x2D    // private sync word (0x2D=Meshtastic default
                                    // private; 0x14 = public LoRaWAN)
#define LORA_INITIAL_TTL    3

bool LoRaLink::begin() {
    Serial.print("[LORA] init SX1262 ... ");
    int st = _radio.begin();
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("begin() failed: %d\n", st);
        _up = false;
        return false;
    }
    _radio.setFrequency(LORA_FREQUENCY);
    _radio.setBandwidth(LORA_BANDWIDTH);
    _radio.setSpreadingFactor(LORA_SPREADING);
    _radio.setCodingRate(LORA_CODING_RATE);
    _radio.setOutputPower(LORA_TX_POWER);
    _radio.setPreambleLength(LORA_PREAMBLE_LEN);
    _radio.setSyncWord(LORA_SYNC_WORD);
    _radio.setCRC(true);

    // Derive a stable gateway id from the low byte of the ESP32 MAC.
    uint64_t mac = ESP.getEfuseMac();
    _myId = (uint8_t)(mac & 0xFF);
    if (_myId == 0) { _myId = 1; }   // 0 reserved for "broadcast src"

    Serial.printf("OK  id=0x%02X freq=%.2fMHz sf=%d\n",
                  _myId, LORA_FREQUENCY, LORA_SPREADING);
    _up = true;
    return true;
}

bool LoRaLink::_seenRecently(uint8_t src, uint8_t seq, uint32_t now) {
    uint16_t k = ((uint16_t)src << 8) | seq;
    for (auto& s : _seen) {
        if (s.key == k && (now - s.ms) < 60000) {  // 60 s dedup window
            return true;
        }
    }
    _seen[_seenIdx] = { k, now };
    _seenIdx = (_seenIdx + 1) % (sizeof(_seen)/sizeof(_seen[0]));
    return false;
}

void LoRaLink::broadcastFrame(uint8_t type,
                              const uint8_t* payload, size_t len) {
    if (!_up) { return; }
    if (len > 244) { return; }   // header is 6 bytes, total cap 250
    uint8_t buf[256];
    buf[0] = 0xFF;                              // dst = broadcast
    buf[1] = _myId;                             // src
    buf[2] = LORA_INITIAL_TTL;                  // ttl
    buf[3] = 0;                                 // hop count
    buf[4] = _seq++;                            // seq
    buf[5] = type;
    memcpy(&buf[6], payload, len);
    size_t total = 6 + len;
    int st = _radio.transmit(buf, total);
    if (st == RADIOLIB_ERR_NONE) {
        _txCount++;
    } else {
        Serial.printf("[LORA] tx failed: %d\n", st);
    }
}

void LoRaLink::poll(void (*onFrame)(uint8_t type, const uint8_t* payload,
                                    size_t len, uint8_t src, int16_t rssi)) {
    if (!_up) { return; }
    uint8_t buf[256];
    int16_t rssi = 0;
    int st = _radio.receive(buf, sizeof(buf), 0);   // non-blocking
    if (st == RADIOLIB_ERR_NONE) {
        size_t got = _radio.getPacketLength();
        if (got < 6) { return; }
        uint8_t dst = buf[0];
        uint8_t src = buf[1];
        uint8_t ttl = buf[2];
        uint8_t hop = buf[3];
        uint8_t seq = buf[4];
        uint8_t type = buf[5];
        // skip our own rebroadcasts and direct packets not for us
        if (src == _myId) { return; }
        if (dst != 0xFF && dst != _myId) {
            // not for us, but rebroadcast to extend the mesh (if TTL > 0)
            if (ttl > 1) {
                buf[2] = ttl - 1;
                buf[3] = hop + 1;
                _radio.transmit(buf, got);
            }
            return;
        }
        uint32_t now = millis();
        if (_seenRecently(src, seq, now)) { return; }
        _rxCount++;
        _lastRssi = rssi;
        onFrame(type, &buf[6], got - 6, src, rssi);
        // rebroadcast so peers can hear it too
        if (ttl > 1 && dst == 0xFF) {
            buf[2] = ttl - 1;
            buf[3] = hop + 1;
            _radio.transmit(buf, got);
        }
    } else if (st != RADIOLIB_ERR_RX_TIMEOUT) {
        // CRC error or other — quietly reset RX state
        _radio.startReceive();
    }
}
