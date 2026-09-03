#include "watch_link.h"

#include "mesh_link.h"
#include "model.h"

WatchLink watchLink;

class ServerCB : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer *) override {
        Serial.println("[watch] connected");
        watchLink.setConnected(true);
    }
    void onDisconnect(NimBLEServer *server) override {
        Serial.println("[watch] disconnected");
        watchLink.setConnected(false);
        server->startAdvertising();
    }
};
static ServerCB serverCB;

// The watch reads TX over and over to drain the queue. NimBLE invokes onRead
// before it copies the stored value into the response, so loading the next
// frame here is what the caller actually receives.
class TxCB : public NimBLECharacteristicCallbacks {
    void onRead(NimBLECharacteristic *) override { watchLink.serveNext(); }
};
static TxCB txCB;

class RxCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic *c) override {
        std::string v = c->getValue();
        if (v.empty()) return;
        uint8_t cmd = (uint8_t)v[0];

        if (cmd == CMD_SYNC) {
            Serial.println("[watch] sync requested");
            model.clearQueue();
            model.pushRoster();
        } else if (cmd == CMD_SEND) {
            char text[32] = {0};
            size_t k = 0;
            for (size_t i = 1; i < v.size() && k < sizeof(text) - 1; i++) {
                uint8_t ch = (uint8_t)v[i];
                if (ch >= 32 && ch < 127) text[k++] = (char)ch;
            }
            text[k] = 0;
            if (k > 0) {
                Serial.printf("[watch] send: %s\n", text);
                meshLink.sendText(text);
            }
        }
    }
};
static RxCB rxCB;

void WatchLink::begin(const char *name) {
    server_ = NimBLEDevice::createServer();
    server_->setCallbacks(&serverCB, false);

    NimBLEService *svc = server_->createService(WATCH_SVC_UUID);
    tx_ = svc->createCharacteristic(
        WATCH_TX_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    rx_ = svc->createCharacteristic(
        WATCH_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    tx_->setCallbacks(&txCB);
    rx_->setCallbacks(&rxCB);

    uint8_t endFrame[1] = {F_END};
    tx_->setValue(endFrame, 1);
    svc->start();

    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(WATCH_SVC_UUID);
    adv->setScanResponse(true);
    adv->setName(name);
    adv->start();
    Serial.printf("[watch] advertising as %s\n", name);
}

void WatchLink::serveNext() {
    std::vector<uint8_t> frame;
    if (model.pop(frame) && !frame.empty()) {
        tx_->setValue(frame.data(), frame.size());
    } else {
        uint8_t endFrame[1] = {F_END};
        tx_->setValue(endFrame, 1);
    }
}

// The watch only re-reads when it is nudged, so tell it whenever the queue
// grows. One notify is enough -- its drain loop keeps reading until END.
void WatchLink::notifyIfPending() {
    if (!connected_ || tx_ == nullptr) return;
    size_t q = model.queued();
    if (q > 0 && q != lastQueued_) {
        tx_->notify();
    }
    lastQueued_ = q;
}

void WatchLink::loop() { notifyIfPending(); }
