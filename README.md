# meshwatch

A Meshtastic client for Garmin watches. Phone-free, internet-free — the watch
talks to a radio over BLE and shows you the mesh on your wrist.

> **Status: complete, pending a bench test.** The watch app is verified on
> ten devices. The [bridge firmware](bridge/) that makes it work against a real
> Meshtastic node compiles clean for two ESP32 targets but has not yet been run
> on hardware. You need both halves — see [How it fits
> together](#how-it-fits-together).

## Screenshots

| Nodes view | Messages view | Canned-message picker |
| :---: | :---: | :---: |
| ![nodes view with battery and last-heard, 1 new message badge](docs/screenshots/01-nodes-view.png) | ![messages view with recent text log and START to send prompt](docs/screenshots/02-messages-view.png) | ![canned-message picker showing OTW, RTB, HOLD](docs/screenshots/03-canned-picker.png) |

Screens are Connect IQ simulator renders against `epix2pro51mm` (tactix 7
AMOLED), driven by the built-in demo roster.

## How it fits together

```
Meshtastic node  <--BLE, 517-byte MTU-->  bridge  <--BLE, 20-byte frames-->  watch
```

A watch cannot talk to a Meshtastic node directly, and no amount of app-side
work changes that. **Connect IQ caps every BLE characteristic read at about 20
bytes and offers no MTU negotiation, long read, or blob read** — the SDK
contains no such symbol anywhere. Meshtastic hands you one whole protobuf per
`fromRadio` read, and a `MeshPacket` spends roughly 17 bytes on framing before
its payload. The next read dequeues the *next* packet, so the truncated tail is
gone for good. A watch reading a node directly recovers node **numbers** and
nothing else — no names, no message text.

So an ESP32 does the reading. [`bridge/`](bridge/) connects to a stock,
unmodified Meshtastic node as an ordinary BLE central at a 517-byte MTU, sees
whole packets, and re-serves them to the watch in frames that fit. The node
still does all the radio work; the bridge never touches LoRa and needs no LoRa
hardware.

That leaves the two client paths here as:

| Path | Talks to | State |
| --- | --- | --- |
| `source/GatewayClient.mc` | the bridge | **Active path.** Full names, multi-chunk message reassembly, and send all work. |
| `source/MeshClient.mc` | a node directly over BLE | Kept for reference. Connects and syncs, then the 20-byte wall truncates everything. Node numbers only. |

## What it does today

- Renders a node roster: short name, battery, last-heard age, SNR, hop count
- Renders a message log; sends canned messages (`ACK` `RGR` `OTW` `RTB`
  `HOLD` `SOS`) or freeform text via the on-screen keyboard
- Scans, bonds, and completes a Meshtastic config sync over BLE
- Hand-rolled Meshtastic protobuf decoding in `source/Proto.mc`, with no
  runtime protobuf library
- Runs on ten Garmin devices from one source tree
- Ships a demo roster on first launch, so the UI is explorable with no radio

## What it does not do

- No mesh routing — the watch is a leaf; routing lives on the radio
- No phone, no Garmin Connect sync, no internet
- No on-watch node configuration — configure in the Meshtastic app or CLI
- **No photo or image transfer.** Meshtastic has no image transport: there is
  no image port in `PortNum`, and the maintainers have said file transfer will
  not be supported over sub-GHz LoRa. An earlier build carried a
  detection-thumbnail feature; it belonged to a different project and has been
  removed rather than left in to imply a protocol feature that does not exist.

## Supported watches

Any round Garmin watch on Connect IQ 3.0+ with BLE. Note there is no `tactix`
product id in Connect IQ — the whole tactix line ships under its fenix / epix
sibling.

| Device id | Display | Notes |
| --- | --- | --- |
| `fenix6xpro` | 280x280 MIP | tactix Delta class |
| `fenix7x` | 260x260 MIP | tactix 7 (non-AMOLED) |
| `fenix7xpro` | 260x260 MIP | |
| `fenix7xpronowifi` | 260x260 MIP | |
| `epix2pro51mm` | 416x416 AMOLED | tactix 7 AMOLED — primary dev target |
| `fenix847mm` | 454x454 AMOLED | tactix 8 / fenix 8 Pro 47 |
| `fenix8solar51mm` | 416x416 AMOLED | fenix 8 Solar 51 |
| `descentmk351mm` | 416x416 AMOLED | Descent Mk3 51 |
| `marq2` | 416x416 AMOLED | MARQ Gen 2 |
| `marq2aviator` | 416x416 AMOLED | MARQ Gen 2 Aviator |

`source/Layout.mc` picks row pitch and accent colour at runtime from the
display dimensions — AMOLED gets 52 px rows and amber; 64-colour MIP gets
44 px rows and an orange that will not dither to mud.

Only `epix2pro51mm` has been visually verified. The rest build clean, but the
MIP layout has not been eyeballed on hardware.

## Install

You need both halves.

**1. Flash the bridge** onto any ESP32 sitting near your Meshtastic node:

```bash
cd bridge
pio run -e esp32dev -t upload      # or -e heltec_v3
```

See [`bridge/README.md`](bridge/README.md) for the serial log a healthy boot
produces and for pairing notes.

**2. Install the watch app.** Signed `.prg` files for all ten watches are
committed under `bin/`. Copy the one matching your watch to `GARMIN/Apps/`
over USB, then eject.

To build the watch app instead:

```bash
monkeyc -d epix2pro51mm -f monkey.jungle \
        -o bin/meshwatch-epix2pro51mm.prg \
        -y ~/.Garmin/developer_key.der
```

Requires the Connect IQ SDK (Windows or macOS only) and a developer key.

## Project layout

```
manifest.xml              app id, 10 products, BLE permission
monkey.jungle             build config
source/HeliographApp.mc   AppBase entry point
source/MainView.mc        nodes / messages pages
source/MainDelegate.mc    button map, send menu
source/MeshClient.mc      Meshtastic-direct BLE (blocked by the 20-byte wall)
source/GatewayClient.mc   20-byte bridge protocol (active path)
source/NodeStore.mc       node DB, Storage-backed
source/Proto.mc           hand-rolled Meshtastic protobuf decode
source/Layout.mc          per-device layout constants
bridge/                   ESP32 bridge firmware -- BLE central to the node,
                          BLE peripheral to the watch. See bridge/README.md
tools/witness.py          PC-side witness: connects to a Meshtastic node over
                          USB serial and logs every text and node the watch
                          would see. Proves the mesh side independently.
```

## Roadmap

Next up, in rough value order:

- **Bench-test the bridge.** It compiles for both targets and the protocol
  matches the watch byte for byte, but it has not met real hardware yet.
- Delivery ACKs (`ROUTING_APP`). The bridge already sets `want_ack` on every
  outbound message, so the receipt comes back — it just is not surfaced yet.
  "Did my message land?" is the biggest remaining UX gap.
- Position beaconing outbound, so the watch appears on everyone else's map.
- Alerts (`ALERT_APP`) with wrist vibration.
- Waypoints (`WAYPOINT_APP`).
- Channels — Meshtastic supports 8; this surfaces one.
- Telemetry (`TELEMETRY_APP`), traceroute (`TRACEROUTE_APP`).
- Visual check on the 64-colour MIP watches.

Two bigger swings, either of which would remove the second board:

1. **Fold the bridge into Meshtastic as a firmware module**, so one device runs
   Meshtastic *and* serves the CIQ-friendly GATT service. Upstreamable.
2. **Ask Meshtastic for an MTU-safe read mode** on its BLE service, which would
   let stock nodes serve Garmin watches with no bridge at all.

## Troubleshooting

**Watch stuck on "scanning".** It is looking for `meshwatch-bridge`. Check the
bridge is powered and its serial log shows `[watch] advertising`.

**Bridge stuck on `[mesh] scanning`.** It has not found a node advertising the
Meshtastic service. A node holds only one BLE client connection — if your phone
is connected to it, disconnect the phone first.

**Paired but the roster is empty.** Config sync takes a few seconds. If the
status reads `live` and nothing arrives, the node has nothing to advertise yet.

**Layout looks wrong.** If your device id is not listed above, add it to
`manifest.xml` and rebuild; pixel offsets were tuned on the AMOLED matrix.

**`monkeyc` rejects the API level.** Pull your device profile through the
Connect IQ SDK Manager.

## Contributing

A bridge implementation is the single most valuable contribution. After that,
layout fixes for the MIP watches — offsets live in `source/Layout.mc` and
`source/MainView.mc`.

Issues welcome, with a watch model and Connect IQ version.

## License

MIT — see `LICENSE`.
