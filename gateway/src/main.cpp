// Heliograph gateway — proof of concept.
//
// The Garmin tactix caps every BLE read at ~22 bytes, so stock Meshtastic's
// larger packets get truncated. This firmware exposes a purpose-built BLE
// service where every frame is <= 20 bytes, so the watch can read a full node
// list and complete (reassembled) messages. For now it serves canned data;
// later the same queue is fed by real Meshtastic/LoRa traffic (here or on a Pi).
//
// Protocol
//   Service a3c8f000-7b1e-4c9a-9f0e-1234567890ab
//   TX  a3c8f001-...  READ+NOTIFY  watch reads 20B frames, one per read
//   RX  a3c8f002-...  WRITE        watch writes commands
//
//   TX frame (first byte = type):
//     0x00 END               queue drained
//     0x01 NODE  [num:4LE][batt:1][flags:1][name:13 ascii, null-pad]
//     0x02 MSG   [id:1][seq:1 (bit7=last)][text:<=17]   reassemble by id
//   RX command:
//     0x01 SYNC              (re)queue all nodes + messages, notify
//     0x02 SEND [text...]    echo the text back as an inbound message

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <deque>
#include <vector>
#include <string>

static const char* SVC_UUID = "a3c8f000-7b1e-4c9a-9f0e-1234567890ab";
static const char* TX_UUID  = "a3c8f001-7b1e-4c9a-9f0e-1234567890ab";
static const char* RX_UUID  = "a3c8f002-7b1e-4c9a-9f0e-1234567890ab";

static NimBLECharacteristic* txChar = nullptr;
static std::deque<std::vector<uint8_t>> txQueue;
static uint8_t msgId = 0;

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

static void notifyReady() {
    if (txChar) {
        uint8_t one = 1;
        txChar->setValue(&one, 1);
        txChar->notify();
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
            notifyReady();
        } else if (cmd == 0x02) {     // SEND -> echo back as inbound
            std::string body = v.substr(1);
            pushMessage("echo: " + body);
            notifyReady();
        }
    }
};

void setup() {
    Serial.begin(115200);
    Serial.println("Heliograph gateway starting");

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
    delay(1000);
}
