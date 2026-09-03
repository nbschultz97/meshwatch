// BLE central side: talks to a stock Meshtastic node.
//
// This half has no size problem. The ESP32 negotiates a 517-byte MTU, so it
// reads whole FromRadio packets the way the phone apps do. Everything it
// learns is reduced to <= 20-byte frames for the watch by model.h.

#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>

// Meshtastic BLE service. Firmware versions have shipped two different
// fromRadio characteristic UUIDs; we try both, same as source/MeshClient.mc.
#define MT_SVC_UUID       "6ba1b218-15a8-461f-9fa8-5dcae273eafd"
#define MT_TORADIO_UUID   "f75c76d2-129e-4dad-a1dd-7866124401e7"
#define MT_FROMRADIO_UUID "8ba2bcc2-ee02-4a55-a531-c525c5e454d5"
#define MT_FROMRADIO_ALT  "2c55e69e-4993-11ed-b878-0242ac120002"
#define MT_FROMNUM_UUID   "ed9da18c-a800-4f66-a670-aa7547e34453"

class MeshLink {
  public:
    enum State { IDLE, SCANNING, CONNECTING, SYNCING, READY, FAILED };

    void begin();
    void loop();

    State state() const { return state_; }
    const char *stateName() const;
    bool ready() const { return state_ == READY; }

    // Queue a broadcast text message onto the mesh. Returns false if the
    // link is not up.
    bool sendText(const char *text);

    // Called from the scan callback when a Meshtastic node is seen.
    void noteCandidate(const NimBLEAddress &addr);
    void noteDisconnect();
    // Called from the fromNum notify: the node has data waiting.
    void noteDataWaiting();

  private:
    bool connectToNode();
    void requestConfig();
    void drain();
    void handleFromRadio(const uint8_t *buf, size_t len);
    void handleNodeInfo(const uint8_t *buf, size_t len);
    void handleMyInfo(const uint8_t *buf, size_t len);
    void handleMeshPacket(const uint8_t *buf, size_t len);

    State state_ = IDLE;
    NimBLEAddress addr_;
    bool haveAddr_ = false;
    bool wantConnect_ = false;
    bool dataWaiting_ = false;
    uint32_t lastAttempt_ = 0;
    uint32_t configNonce_ = 0;

    NimBLEClient *client_ = nullptr;
    NimBLERemoteCharacteristic *toRadio_ = nullptr;
    NimBLERemoteCharacteristic *fromRadio_ = nullptr;
    NimBLERemoteCharacteristic *fromNum_ = nullptr;
};

extern MeshLink meshLink;
