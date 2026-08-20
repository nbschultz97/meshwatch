# Heliograph

Meshtastic client for Garmin watches. Pairs directly to a Meshtastic node over
BLE — no phone, no internet, no infrastructure. The watch is the display; the
node is the radio; LoRa carries the rest.

Named for the [heliograph](https://en.wikipedia.org/wiki/Heliograph): wearable,
sun-powered, off-grid signaling. Same idea, fewer mirrors.

## Status: milestone 1 (BLE recon)

- [x] Scan for a Meshtastic node (service UUID or `Meshtastic*` name)
- [x] Connect with plain just-works pairing (avoids the CIQ
      `SECURE_PAIR_BOND` watch-reboot bug against PIN-disabled nodes)
- [x] `want_config_id` handshake → drain `fromRadio` → decode node DB
- [x] Node list UI: short/long name, battery, last-heard age, hops
- [ ] Milestone 2: text messages (decode `MeshPacket`/`Data`, canned-message send)
- [ ] Milestone 3: watch-GPS position beaconing into the mesh (ATAK-visible)
- [ ] Milestone 4: alert routing (detection events from sensor nodes → vibe)

## Building

Requirements: Connect IQ SDK 9.x, OpenJDK 17+, `tactix7amoled` device profile
(via SDK Manager), developer key at `~/.Garmin/developer_key.der`.

```
monkeyc -o bin/heliograph.prg -f monkey.jungle -d tactix7amoled -y ~/.Garmin/developer_key.der
```

Deploy: copy `bin/heliograph.prg` to the watch's `Internal Storage/GARMIN/Apps`
folder over MTP (Explorer). The app appears in the activity list.

## Device-side setup

The target node must have BLE enabled (default) — pairing mode "just works"
or fixed PIN both fine for milestone 1. Tested against Heltec V3.

## Protocol notes

Speaks the Meshtastic BLE client API: service
`6ba1b218-15a8-461f-9fa8-5dcae273eafd`, protobufs hand-decoded in
`source/Proto.mc` (only the fields we need — see comments there for the
field-number map). Config sync follows the standard client flow: write
`ToRadio{want_config_id}` → read `fromRadio` until empty → `fromNum` notify
signals new data.
