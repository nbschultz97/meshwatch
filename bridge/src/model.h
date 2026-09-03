// Shared state between the Meshtastic side and the watch side, plus the
// wire format the watch speaks.
//
// Watch protocol (must match source/GatewayClient.mc exactly — every frame
// <= 20 bytes because Connect IQ truncates anything longer):
//
//   bridge -> watch, on the TX characteristic (read + notify)
//     0x00                                          END, queue drained
//     0x01 [num:4 LE][batt][flags][name.. NUL]      NODE
//     0x02 [id][seq | 0x80 if last][text..]         MSG chunk
//
//   watch -> bridge, on the RX characteristic (write)
//     0x01                                          resync, resend roster
//     0x02 [utf8 text..]                            send a text message
//
// battery is 0..100, or 101 to mean "externally powered" (the watch renders
// that as PWR). flags bit0 marks the node the bridge is paired to.

#pragma once
#include <Arduino.h>
#include <deque>
#include <vector>

static const uint8_t F_END  = 0x00;
static const uint8_t F_NODE = 0x01;
static const uint8_t F_MSG  = 0x02;

static const uint8_t CMD_SYNC = 0x01;
static const uint8_t CMD_SEND = 0x02;

static const uint8_t NODE_FLAG_SELF = 0x01;

static const size_t MAX_FRAME = 20;
static const size_t MAX_NODES = 32;
// Enough to hold a full roster push plus a burst of traffic. Frames are
// dropped oldest-first past this, which loses history rather than wedging.
static const size_t MAX_QUEUE = 192;

struct MeshNode {
    uint32_t num = 0;
    char name[14] = {0};   // 13 chars + NUL: what fits after a NODE header
    uint8_t battery = 0;
    bool self = false;
    bool used = false;
};

// Everything the two BLE roles share. Single-threaded by design: both the
// NimBLE host callbacks and loop() touch this, so all mutation happens from
// the Arduino task via the queue below.
class Model {
  public:
    MeshNode nodes[MAX_NODES];
    uint32_t myNum = 0;
    bool synced = false;

    // Upsert by node number. Returns the slot, or nullptr if the table is
    // full (we keep the first MAX_NODES we hear about rather than evicting;
    // a watch screen cannot show more than a handful anyway).
    MeshNode *upsert(uint32_t num) {
        if (num == 0) return nullptr;
        for (size_t i = 0; i < MAX_NODES; i++) {
            if (nodes[i].used && nodes[i].num == num) return &nodes[i];
        }
        for (size_t i = 0; i < MAX_NODES; i++) {
            if (!nodes[i].used) {
                nodes[i].used = true;
                nodes[i].num = num;
                nodes[i].name[0] = 0;
                nodes[i].battery = 0;
                nodes[i].self = false;
                return &nodes[i];
            }
        }
        return nullptr;
    }

    size_t nodeCount() const {
        size_t n = 0;
        for (size_t i = 0; i < MAX_NODES; i++) {
            if (nodes[i].used) n++;
        }
        return n;
    }

    // ---- frame queue, drained by the watch one read at a time ----

    void push(const std::vector<uint8_t> &f) {
        if (f.size() > MAX_FRAME) return;
        if (queue.size() >= MAX_QUEUE) queue.pop_front();
        queue.push_back(f);
    }

    bool pop(std::vector<uint8_t> &out) {
        if (queue.empty()) return false;
        out = queue.front();
        queue.pop_front();
        return true;
    }

    size_t queued() const { return queue.size(); }
    void clearQueue() { queue.clear(); }

    // ---- frame builders ----

    void pushNode(const MeshNode &n) {
        std::vector<uint8_t> f;
        f.push_back(F_NODE);
        f.push_back(n.num & 0xff);
        f.push_back((n.num >> 8) & 0xff);
        f.push_back((n.num >> 16) & 0xff);
        f.push_back((n.num >> 24) & 0xff);
        f.push_back(n.battery);
        f.push_back(n.self ? NODE_FLAG_SELF : 0);
        for (size_t i = 0; i < sizeof(n.name) && n.name[i] && f.size() < MAX_FRAME; i++) {
            f.push_back((uint8_t)n.name[i]);
        }
        push(f);
    }

    void pushRoster() {
        for (size_t i = 0; i < MAX_NODES; i++) {
            if (nodes[i].used) pushNode(nodes[i]);
        }
        push({F_END});
    }

    // Split a message across as many MSG frames as it takes. The watch
    // reassembles on the id byte and stops at the frame with bit7 set.
    void pushMessage(const char *text) {
        if (!text) return;
        size_t len = strlen(text);
        if (len == 0) return;
        const size_t chunk = MAX_FRAME - 3;   // type + id + seq
        uint8_t id = nextMsgId++;
        size_t off = 0;
        uint8_t seq = 0;
        while (off < len && seq < 0x7f) {
            size_t take = len - off;
            if (take > chunk) take = chunk;
            bool last = (off + take >= len);
            std::vector<uint8_t> f;
            f.push_back(F_MSG);
            f.push_back(id);
            f.push_back(seq | (last ? 0x80 : 0x00));
            for (size_t i = 0; i < take; i++) f.push_back((uint8_t)text[off + i]);
            push(f);
            off += take;
            seq++;
        }
    }

  private:
    std::deque<std::vector<uint8_t>> queue;
    uint8_t nextMsgId = 1;
};

extern Model model;
