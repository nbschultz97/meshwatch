// BLE peripheral side: serves the Garmin watch.
//
// The watch polls the TX characteristic. Every read pops one frame off the
// queue, or returns a bare END byte when there is nothing left. Frames are
// never longer than 20 bytes, because Connect IQ silently truncates anything
// bigger and the next read would dequeue the following packet -- losing the
// tail for good. That constraint is the whole reason this firmware exists.

#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>

#define WATCH_SVC_UUID "a3c8f000-7b1e-4c9a-9f0e-1234567890ab"
#define WATCH_TX_UUID  "a3c8f001-7b1e-4c9a-9f0e-1234567890ab"
#define WATCH_RX_UUID  "a3c8f002-7b1e-4c9a-9f0e-1234567890ab"

class WatchLink {
  public:
    void begin(const char *name);
    void loop();

    bool connected() const { return connected_; }
    void setConnected(bool c) { connected_ = c; }

    // Load the next queued frame into the TX characteristic. Called from the
    // read callback, and after new data arrives so the notify carries it.
    void serveNext();
    // Poke the watch so it comes back and drains.
    void notifyIfPending();

  private:
    NimBLEServer *server_ = nullptr;
    NimBLECharacteristic *tx_ = nullptr;
    NimBLECharacteristic *rx_ = nullptr;
    bool connected_ = false;
    size_t lastQueued_ = 0;
};

extern WatchLink watchLink;
