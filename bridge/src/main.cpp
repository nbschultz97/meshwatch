// meshwatch bridge
//
// Sits between a stock Meshtastic node and a Garmin watch, and exists for one
// reason: Connect IQ caps every BLE characteristic read at ~20 bytes with no
// MTU negotiation, no long read and no blob read. A Meshtastic MeshPacket
// spends ~17 bytes on framing before its payload, and each fromRadio read
// dequeues the next packet -- so a watch reading the node directly recovers
// node numbers and nothing else. No amount of app-side cleverness fixes that.
//
// So this board reads the node as a normal BLE central at a 517-byte MTU,
// then re-serves what it learned to the watch in <= 20-byte frames.
//
//   Meshtastic node  <--BLE (517B MTU)--  bridge  --BLE (20B frames)-->  watch
//
// Nothing here touches LoRa. The node still does all the radio work; the
// bridge is purely a translator.

#include <Arduino.h>
#include <NimBLEDevice.h>

#include "mesh_link.h"
#include "model.h"
#include "watch_link.h"

Model model;

static const char *DEVICE_NAME = "meshwatch-bridge";

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("meshwatch bridge starting");

    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    // Peripheral first, so a watch can attach and show "live" while the node
    // side is still scanning. It will see an empty roster until sync lands.
    watchLink.begin(DEVICE_NAME);
    meshLink.begin();
}

void loop() {
    meshLink.loop();
    watchLink.loop();

    static uint32_t lastLog = 0;
    if (millis() - lastLog > 10000) {
        lastLog = millis();
        Serial.printf("[bridge] mesh=%s watch=%s nodes=%u queued=%u\n",
                      meshLink.stateName(),
                      watchLink.connected() ? "connected" : "waiting",
                      (unsigned)model.nodeCount(), (unsigned)model.queued());
    }
    delay(10);
}
