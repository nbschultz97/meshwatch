#include "mesh_link.h"

#include "model.h"
#include "pb.h"

MeshLink meshLink;

// Field numbers below come from meshtastic/mesh.proto and are kept identical
// to the map in source/Proto.mc so the watch and the bridge drift together:
//
//   FromRadio:     1=packet 3=my_info 4=node_info 7=config_complete_id
//   MyNodeInfo:    1=my_node_num
//   NodeInfo:      1=num 2=user 4=snr 6=device_metrics 9=hops_away
//   User:          2=long_name 3=short_name
//   DeviceMetrics: 1=battery_level
//   MeshPacket:    1=from 4=decoded
//   Data:          1=portnum 2=payload
//
// PortNum 1 is TEXT_MESSAGE_APP. Meshtastic has no image port at all, which
// is why this bridge carries text, nodes and nothing pretending to be a photo.
static const uint32_t PORTNUM_TEXT = 1;

// ---------------------------------------------------------------- callbacks

class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *dev) override {
        if (!dev->isAdvertisingService(NimBLEUUID(MT_SVC_UUID))) return;
        Serial.printf("[mesh] found node %s (%s)\n",
                      dev->getAddress().toString().c_str(),
                      dev->haveName() ? dev->getName().c_str() : "unnamed");
        // Connecting from inside a scan callback deadlocks the NimBLE host
        // task; hand the address to loop() and let it do the work.
        NimBLEDevice::getScan()->stop();
        meshLink.noteCandidate(dev->getAddress());
    }
};
static ScanCB scanCB;

class ClientCB : public NimBLEClientCallbacks {
    void onDisconnect(NimBLEClient *) override {
        Serial.println("[mesh] node disconnected");
        meshLink.noteDisconnect();
    }
    // Meshtastic nodes default to just-works pairing. A node set to a fixed
    // PIN needs that PIN here instead.
    uint32_t onPassKeyRequest() override { return 123456; }
    bool onConfirmPIN(uint32_t) override { return true; }
};
static ClientCB clientCB;

static void fromNumNotify(NimBLERemoteCharacteristic *, uint8_t *, size_t, bool) {
    meshLink.noteDataWaiting();
}

// ------------------------------------------------------------------- public

void MeshLink::begin() {
    NimBLEDevice::setMTU(517);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bond, no MITM, SC
    state_ = SCANNING;
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&scanCB, false);
    scan->setActiveScan(true);
    scan->setInterval(160);
    scan->setWindow(80);
    scan->start(0, nullptr, false);
    Serial.println("[mesh] scanning for a Meshtastic node");
}

void MeshLink::noteCandidate(const NimBLEAddress &addr) {
    addr_ = addr;
    haveAddr_ = true;
    wantConnect_ = true;
}

void MeshLink::noteDisconnect() {
    state_ = SCANNING;
    toRadio_ = fromRadio_ = fromNum_ = nullptr;
    model.synced = false;
    wantConnect_ = haveAddr_;
}

void MeshLink::noteDataWaiting() { dataWaiting_ = true; }

const char *MeshLink::stateName() const {
    switch (state_) {
        case IDLE:       return "idle";
        case SCANNING:   return "scanning";
        case CONNECTING: return "connecting";
        case SYNCING:    return "syncing";
        case READY:      return "ready";
        default:         return "failed";
    }
}

void MeshLink::loop() {
    if (wantConnect_ && millis() - lastAttempt_ > 2000) {
        lastAttempt_ = millis();
        wantConnect_ = false;
        if (!connectToNode()) {
            state_ = SCANNING;
            wantConnect_ = haveAddr_;
            NimBLEScan *scan = NimBLEDevice::getScan();
            if (!scan->isScanning()) scan->start(0, nullptr, false);
        }
        return;
    }
    if (dataWaiting_ && (state_ == SYNCING || state_ == READY)) {
        dataWaiting_ = false;
        drain();
    }
}

bool MeshLink::connectToNode() {
    state_ = CONNECTING;
    Serial.println("[mesh] connecting");

    if (client_ == nullptr) {
        client_ = NimBLEDevice::createClient();
        client_->setClientCallbacks(&clientCB, false);
        client_->setConnectionParams(12, 12, 0, 400);
        client_->setConnectTimeout(10);
    }
    if (!client_->connect(addr_)) {
        Serial.println("[mesh] connect failed");
        return false;
    }

    NimBLERemoteService *svc = client_->getService(NimBLEUUID(MT_SVC_UUID));
    if (svc == nullptr) {
        Serial.println("[mesh] no Meshtastic service on that node");
        client_->disconnect();
        return false;
    }

    toRadio_ = svc->getCharacteristic(NimBLEUUID(MT_TORADIO_UUID));
    fromRadio_ = svc->getCharacteristic(NimBLEUUID(MT_FROMRADIO_UUID));
    if (fromRadio_ == nullptr) {
        // Older/newer firmware uses the other UUID for the same thing.
        fromRadio_ = svc->getCharacteristic(NimBLEUUID(MT_FROMRADIO_ALT));
    }
    fromNum_ = svc->getCharacteristic(NimBLEUUID(MT_FROMNUM_UUID));

    if (toRadio_ == nullptr || fromRadio_ == nullptr) {
        Serial.println("[mesh] missing toRadio/fromRadio characteristic");
        client_->disconnect();
        return false;
    }
    if (fromNum_ != nullptr && fromNum_->canNotify()) {
        fromNum_->subscribe(true, fromNumNotify);
    }

    Serial.printf("[mesh] connected, MTU %u\n", client_->getMTU());
    state_ = SYNCING;
    requestConfig();
    drain();
    return true;
}

// ToRadio { want_config_id }. Kicks the node into streaming its whole state
// -- my_info, the node DB, config -- out of fromRadio.
void MeshLink::requestConfig() {
    configNonce_ = esp_random() | 1;
    pb::Writer w;
    w.varintField(3, configNonce_);
    toRadio_->writeValue((uint8_t *)w.data(), w.size(), true);
    Serial.printf("[mesh] want_config_id=%u\n", configNonce_);
}

// Read fromRadio until it comes back empty. Each read yields one whole
// FromRadio message -- this is exactly the loop the watch cannot run itself,
// because Connect IQ would hand it only the first 20 bytes of each one.
void MeshLink::drain() {
    for (int guard = 0; guard < 256; guard++) {
        std::string v = fromRadio_->readValue();
        if (v.empty()) break;
        handleFromRadio((const uint8_t *)v.data(), v.size());
    }
}

// ------------------------------------------------------------------ decode

void MeshLink::handleFromRadio(const uint8_t *buf, size_t len) {
    pb::Reader r(buf, len);
    uint32_t field, wire;
    while (r.tag(&field, &wire)) {
        if (wire == pb::WIRE_BYTES) {
            const uint8_t *d;
            size_t n;
            if (!r.bytes(&d, &n)) break;
            if (field == 3) {
                handleMyInfo(d, n);
            } else if (field == 4) {
                handleNodeInfo(d, n);
            } else if (field == 1) {
                handleMeshPacket(d, n);
            }
        } else if (wire == pb::WIRE_VARINT) {
            uint64_t v;
            if (!r.varint(&v)) break;
            if (field == 7) {  // config_complete_id
                Serial.printf("[mesh] config complete, %u nodes\n",
                              (unsigned)model.nodeCount());
                model.synced = true;
                state_ = READY;
                model.pushRoster();
            }
        } else if (!r.skip(wire)) {
            break;
        }
    }
}

void MeshLink::handleMyInfo(const uint8_t *buf, size_t len) {
    pb::Reader r(buf, len);
    uint32_t field, wire;
    while (r.tag(&field, &wire)) {
        if (field == 1 && wire == pb::WIRE_VARINT) {
            uint64_t v;
            if (!r.varint(&v)) break;
            model.myNum = (uint32_t)v;
            MeshNode *n = model.upsert(model.myNum);
            if (n) n->self = true;
        } else if (!r.skip(wire)) {
            break;
        }
    }
}

void MeshLink::handleNodeInfo(const uint8_t *buf, size_t len) {
    uint32_t num = 0;
    char shortName[14] = {0};
    char longName[14] = {0};
    uint8_t battery = 0;

    pb::Reader r(buf, len);
    uint32_t field, wire;
    while (r.tag(&field, &wire)) {
        if (field == 1 && wire == pb::WIRE_VARINT) {
            uint64_t v;
            if (!r.varint(&v)) break;
            num = (uint32_t)v;
        } else if (field == 2 && wire == pb::WIRE_BYTES) {
            const uint8_t *d;
            size_t n;
            if (!r.bytes(&d, &n)) break;
            pb::Reader ur(d, n);
            uint32_t uf, uw;
            while (ur.tag(&uf, &uw)) {
                if (uw == pb::WIRE_BYTES && (uf == 2 || uf == 3)) {
                    const uint8_t *sd;
                    size_t sn;
                    if (!ur.bytes(&sd, &sn)) break;
                    char *dst = (uf == 3) ? shortName : longName;
                    size_t cap = 13;
                    size_t k = 0;
                    for (size_t i = 0; i < sn && k < cap; i++) {
                        if (sd[i] >= 32 && sd[i] < 127) dst[k++] = (char)sd[i];
                    }
                    dst[k] = 0;
                } else if (!ur.skip(uw)) {
                    break;
                }
            }
        } else if (field == 6 && wire == pb::WIRE_BYTES) {
            const uint8_t *d;
            size_t n;
            if (!r.bytes(&d, &n)) break;
            pb::Reader mr(d, n);
            uint32_t mf, mw;
            while (mr.tag(&mf, &mw)) {
                if (mf == 1 && mw == pb::WIRE_VARINT) {
                    uint64_t v;
                    if (!mr.varint(&v)) break;
                    battery = (uint8_t)(v > 101 ? 101 : v);
                } else if (!mr.skip(mw)) {
                    break;
                }
            }
        } else if (!r.skip(wire)) {
            break;
        }
    }

    if (num == 0) return;
    MeshNode *n = model.upsert(num);
    if (n == nullptr) return;
    // Prefer the short name -- it is what fits on a watch row. Fall back to
    // the long name, then to the last four hex of the node number, which is
    // what Meshtastic itself shows for a node that has not sent a User yet.
    const char *pick = shortName[0] ? shortName : (longName[0] ? longName : nullptr);
    if (pick) {
        strncpy(n->name, pick, sizeof(n->name) - 1);
        n->name[sizeof(n->name) - 1] = 0;
    } else if (n->name[0] == 0) {
        snprintf(n->name, sizeof(n->name), "%04x", (unsigned)(num & 0xffff));
    }
    if (battery) n->battery = battery;
    if (num == model.myNum) n->self = true;

    // Once the initial sync is done, a NodeInfo means something changed --
    // push just that row rather than the whole roster.
    if (model.synced) model.pushNode(*n);
}

void MeshLink::handleMeshPacket(const uint8_t *buf, size_t len) {
    uint32_t from = 0;
    uint32_t portnum = 0;
    char text[160] = {0};
    bool haveText = false;

    pb::Reader r(buf, len);
    uint32_t field, wire;
    while (r.tag(&field, &wire)) {
        if (field == 1 && wire == pb::WIRE_FIXED32) {
            if (!r.fixed32(&from)) break;
        } else if (field == 1 && wire == pb::WIRE_VARINT) {
            uint64_t v;  // some builds varint-encode `from`
            if (!r.varint(&v)) break;
            from = (uint32_t)v;
        } else if (field == 4 && wire == pb::WIRE_BYTES) {
            const uint8_t *d;
            size_t n;
            if (!r.bytes(&d, &n)) break;
            pb::Reader dr(d, n);
            uint32_t df, dw;
            while (dr.tag(&df, &dw)) {
                if (df == 1 && dw == pb::WIRE_VARINT) {
                    uint64_t v;
                    if (!dr.varint(&v)) break;
                    portnum = (uint32_t)v;
                } else if (df == 2 && dw == pb::WIRE_BYTES) {
                    const uint8_t *pd;
                    size_t pn;
                    if (!dr.bytes(&pd, &pn)) break;
                    size_t k = 0;
                    for (size_t i = 0; i < pn && k < sizeof(text) - 1; i++) {
                        if (pd[i] >= 32 && pd[i] < 127) text[k++] = (char)pd[i];
                    }
                    text[k] = 0;
                    haveText = k > 0;
                } else if (!dr.skip(dw)) {
                    break;
                }
            }
        } else if (!r.skip(wire)) {
            break;
        }
    }

    if (portnum != PORTNUM_TEXT || !haveText) return;

    // Prefix with the sender's short name so the watch row reads like the
    // phone apps do. The watch stores the whole reassembled string.
    char line[192];
    const char *who = nullptr;
    for (size_t i = 0; i < MAX_NODES; i++) {
        if (model.nodes[i].used && model.nodes[i].num == from && model.nodes[i].name[0]) {
            who = model.nodes[i].name;
            break;
        }
    }
    if (who) {
        snprintf(line, sizeof(line), "%s: %s", who, text);
    } else {
        snprintf(line, sizeof(line), "%04x: %s", (unsigned)(from & 0xffff), text);
    }
    Serial.printf("[mesh] text: %s\n", line);
    model.pushMessage(line);
}

// ------------------------------------------------------------------ send

// ToRadio { packet: MeshPacket { to: broadcast, decoded: Data {
//   portnum: TEXT_MESSAGE_APP, payload: text } } }
//
// Unlike the watch, we are not squeezed for room here, so want_ack is set --
// that is what makes a delivery ACK come back on ROUTING_APP later.
bool MeshLink::sendText(const char *text) {
    if (state_ != READY || toRadio_ == nullptr || text == nullptr) return false;
    size_t len = strlen(text);
    if (len == 0) return false;
    if (len > 200) len = 200;

    pb::Writer data;
    data.varintField(1, PORTNUM_TEXT);
    data.bytesField(2, (const uint8_t *)text, len);

    pb::Writer packet;
    packet.fixed32Field(2, 0xffffffff);   // to: broadcast
    packet.subMessage(4, data);           // decoded
    packet.varintField(10, 1);            // want_ack

    pb::Writer toRadio;
    toRadio.subMessage(1, packet);

    bool ok = toRadio_->writeValue((uint8_t *)toRadio.data(), toRadio.size(), true);
    Serial.printf("[mesh] send \"%s\" -> %s\n", text, ok ? "ok" : "failed");
    return ok;
}
