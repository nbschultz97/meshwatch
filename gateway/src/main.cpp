// Heliograph gateway — watch bridge + LoRa mesh node on one Heltec V3.
//
// The Garmin tactix caps every BLE read at ~22 bytes, so stock Meshtastic's
// larger packets get truncated. This firmware exposes a purpose-built BLE
// service where every frame is <= 20 bytes, so the watch can read a full
// node list and complete (reassembled) messages.
//
// As of the LoRa mesh step (see lora_link.h), the same frames also flow over
// LoRa, so multiple Heltecs form a small mesh. The watch talks to ONE gateway
// over BLE; that gateway is also a mesh peer over LoRa.
//
// BLE protocol (unchanged)
//   Service a3c8f000-7b1e-4c9a-9f0e-1234567890ab
//   TX  a3c8f001-...  READ+NOTIFY  watch reads 20B frames, one per read
//   RX  a3c8f002-...  WRITE        watch writes commands
//   TX frame (first byte = type):
//     0x00 END               queue drained
//     0x01 NODE  [num:4LE][batt:1][flags:1][name:13 ascii, null-pad]
//     0x02 MSG   [id:1][seq:1 (bit7=last)][text:<=17]   reassemble by id
//     0x03 IMAGE [seq:1][16 data bytes]                 128 frames = 2048B
//   RX command:
//     0x01 SYNC              (re)queue all nodes + messages, notify
//     0x02 SEND [text...]    echo + broadcast over LoRa
//     0x03 GETIMG            stream the bundled detection thumbnail
//
// LoRa protocol (see lora_link.h)
//   [dst:1][src:1][ttl:1][hop:1][seq:1][type:1][payload...]
//   Frames are bridged: LoRa RX payload -> BLE queue; BLE SEND -> LoRa TX.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <deque>
#include <vector>
#include <string>
#include "picket_thumb.h"   // IMG_DATA[2048] = 64x64 4-bit grayscale
#include "lora_link.h"

static const char* SVC_UUID = "a3c8f000-7b1e-4c9a-9f0e-1234567890ab";
static const char* TX_UUID  = "a3c8f001-7b1e-4c9a-9f0e-1234567890ab";
static const char* RX_UUID  = "a3c8f002-7b1e-4c9a-9f0e-1234567890ab";

static NimBLECharacteristic* txChar = nullptr;
static std::deque<std::vector<uint8_t>> txQueue;
static uint8_t msgId = 0;
static LoRaLink lora;

// Canned roster — these are surfaced to the watch over BLE so it has
// something to render before any LoRa peers show up. When a LoRa peer is
// heard from, its gateway id becomes a synthetic node (see onLoRaFrame).
struct Node { uint32_t num; uint8_t batt; uint8_t flags; const char* name; };
static const Node NODES[] = {
    { 0x10000001, 88,  1, "NOAH" },
    { 0x10000002, 101, 0, "OUTSTATION01" },
    { 0x10000003, 64,  0, "PICKET-UGV" },
    { 0x10000004, 77,  0, "APERTURE-RF" },
};

static void pushNode(const Node& n) {
    std::vector<uint8_t> f(20, 0);
    f[0] = 0x01;
    f[1] = n.num & 0xff;
    f[2] = (n.num >> 8) & 0xff;
    f[3] = (n.num >> 16) & 0xff;
    f[4] = (n.num >> 24) & 0xff;
    f[5] = n.batt;
    f[6] = n.flags;
    for (int i = 0; i < 13 && n.name[i]; i++) { f[7 + i] = n.name[i]; }
    txQueue.push_back(f);
}

static void pushMessage(const std::string& text) {
    uint8_t id = msgId++;
    size_t off = 0;
    do {
        size_t n = text.size() - off;
        if (n > 17) { n = 17; }
        std::vector<uint8_t> f;
        f.push_back(0x02);
        f.push_back(id);
        bool last = (off + n) >= text.size();
        f.push_back((uint8_t)((off / 17) | (last ? 0x80 : 0x00)));
        for (size_t i = 0; i < n; i++) { f.push_back((uint8_t)text[off + i]); }
        txQueue.push_back(f);
        off += n;
    } while (off < text.size());
}

// IMAGE frame: [0x03][seq:1][16 data bytes]. 2048B / 16 = 128 frames.
static void pushImage() {
    for (int seq = 0; seq < 128; seq++) {
        std::vector<uint8_t> f;
        f.push_back(0x03);
        f.push_back((uint8_t)seq);
        for (int i = 0; i < 16; i++) { f.push_back(IMG_DATA[seq * 16 + i]); }
        txQueue.push_back(f);
    }
}

static void notifyReady() {
    if (txChar) {
        uint8_t one = 1;
        txChar->setValue(&one, 1);
        txChar->notify();
    }
}

// Convert a LoRa peer's gateway id into a synthetic mesh node so the watch
// sees it. We re-use the canned-roster format: num = 0x40XXYYZZ where XX is
// the LoRa id. Name uses the 13-char ASCII budget: "LORA-<hex>".
static void pushLoRaNodeAsBLE(uint8_t loraId, int16_t rssi) {
    std::vector<uint8_t> f(20, 0);
    f[0] = 0x01;
    uint32_t num = 0x40000000u | ((uint32_t)loraId << 16);
    f[1] = num & 0xff;
    f[2] = (num >> 8) & 0xff;
    f[3] = (num >> 16) & 0xff;
    f[4] = (num >> 24) & 0xff;
    // Battery unknown; map RSSI into 0..100 so the watch has something to show.
    uint8_t batt = (rssi < -120) ? 0 :
                   (rssi > -40)  ? 100 :
                   (uint8_t)((rssi + 120) * 100 / 80);
    f[5] = batt;
    f[6] = 0;   // flags (bit0 = isMe)
    const char* tag = "LORA-";
    for (int i = 0; i < 5; i++) { f[7 + i] = tag[i]; }
    char hex[3] = { 0, 0, 0 };
    const char* digits = "0123456789ABCDEF";
    hex[0] = digits[(loraId >> 4) & 0xF];
    hex[1] = digits[loraId & 0xF];
    f[12] = hex[0]; f[13] = hex[1];
    txQueue.push_back(std::move(f));
}

static void onLoRaFrame(uint8_t type, const uint8_t* payload, size_t len,
                        uint8_t src, int16_t rssi) {
    char buf[80];
    snprintf(buf, sizeof(buf), "[LORA RX] id=0x%02X rssi=%d type=0x%02X len=%u",
             src, rssi, type, (unsigned)len);
    Serial.println(buf);

    if (type == 0x02 && len > 0 && len <= 250) {
        // Incoming mesh message. Synthesize a short preview for the watch:
        //   "LORA-<hex>: <text>"   (truncated to 17B per BLE chunk; the
        //   chunker in pushMessage() takes care of multi-frame reassembly
        //   on the watch side).
        std::string s = "LORA-";
        const char* d = "0123456789ABCDEF";
        s += d[(src >> 4) & 0xF]; s += d[src & 0xF];
        s += ": ";
        s.append((const char*)payload, len);
        pushMessage(s);
        pushLoRaNodeAsBLE(src, rssi);
        notifyReady();
    } else if (type == 0x01) {
        // Remote node announcement -> just update the roster
        pushLoRaNodeAsBLE(src, rssi);
        notifyReady();
    } else if (type == 0x03) {
        // Image chunk over LoRa (slow but possible); push to BLE queue.
        std::vector<uint8_t> f;
        f.push_back(0x03);
        for (size_t i = 0; i < len && i < 18; i++) { f.push_back(payload[i]); }
        txQueue.push_back(std::move(f));
        notifyReady();
    }
}

class TxCallbacks : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic* c) {
        if (txQueue.empty()) {
            uint8_t end = 0x00;
            c->setValue(&end, 1);   // END frame
            return;
        }
        std::vector<uint8_t> f = txQueue.front();
        txQueue.pop_front();
        c->setValue(f.data(), f.size());
    }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c) {
        std::string v = c->getValue();
        if (v.empty()) { return; }
        uint8_t cmd = (uint8_t)v[0];
        if (cmd == 0x01) {            // SYNC
            txQueue.clear();
            for (auto& n : NODES) { pushNode(n); }
            pushMessage("OS01: motion detected N gate");
            pushMessage("PICKET: waypoint 3 reached");
            if (lora.isUp()) {
                char id[16];
                snprintf(id, sizeof(id), "GW id 0x%02X", lora.myId());
                pushMessage(id);
            }
            notifyReady();
        } else if (cmd == 0x02) {     // SEND -> echo + LoRa broadcast
            std::string body = v.substr(1);
            pushMessage("echo: " + body);
            notifyReady();
            if (lora.isUp()) {
                // Type 0x02 over LoRa carries the raw text as payload.
                lora.broadcastFrame(0x02,
                                    (const uint8_t*)body.data(),
                                    body.size());
            }
        } else if (cmd == 0x03) {     // GETIMG
            pushImage();
            notifyReady();
        }
    }
};

void setup() {
    Serial.begin(115200);
    Serial.println("\nHeliograph gateway starting");

    // Bring up the radio first so we can fail fast and surface any wiring
    // problem (Heltec V3 schematic mismatches have bitten people here).
    lora.begin();

    NimBLEDevice::init("Heliograph-GW");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    NimBLEServer* server = NimBLEDevice::createServer();
    NimBLEService* svc = server->createService(SVC_UUID);

    txChar = svc->createCharacteristic(
        TX_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    txChar->setCallbacks(new TxCallbacks());

    NimBLECharacteristic* rxChar = svc->createCharacteristic(
        RX_UUID, NIMBLE_PROPERTY::WRITE);
    rxChar->setCallbacks(new RxCallbacks());

    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(SVC_UUID);
    adv->setScanResponse(true);
    adv->start();

    Serial.println("Heliograph gateway advertising");
}

void loop() {
    // LoRa RX pump. Each ready packet fires onLoRaFrame, which pushes
    // frames onto the BLE queue (the watch sees them on its next read).
    lora.poll(onLoRaFrame);
    delay(50);
}
