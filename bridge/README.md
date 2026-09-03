# meshwatch bridge

Firmware for any ESP32 that makes meshwatch actually work.

```
Meshtastic node  <--BLE, 517-byte MTU-->  bridge  <--BLE, 20-byte frames-->  watch
```

## Why this has to exist

Connect IQ caps every BLE characteristic read at about 20 bytes and offers no
MTU negotiation, no long read, and no blob read — the SDK contains no such
symbol anywhere. Meshtastic hands you one whole protobuf per `fromRadio`
read, and a `MeshPacket` spends roughly 17 bytes on framing before its
payload. Worse, the next read dequeues the *next* packet, so the truncated
tail is gone for good.

A watch talking straight to a node therefore recovers node **numbers** and
nothing else — no names, no message text. That is a Garmin platform limit,
not something app code can work around.

This board does the reading instead. It connects to the node as an ordinary
BLE central at a 517-byte MTU, so it sees whole packets, then re-serves what
it learned to the watch in frames that fit.

The node keeps doing all the radio work. The bridge never touches LoRa — it
is purely a translator, and it needs no LoRa hardware.

## Hardware

Any ESP32 with BLE. Two build targets ship:

| Target | Board | Notes |
| --- | --- | --- |
| `esp32dev` | generic ESP32 devkit | cheapest thing that works |
| `heltec_v3` | Heltec WiFi LoRa 32 V3 | ESP32-S3; the LoRa radio sits unused |

Build sizes: ~608 KB flash / 36 KB RAM on `esp32dev`, ~534 KB / 30 KB on
`heltec_v3`. Both leave plenty of headroom.

## Flash it

```bash
cd bridge
pio run -e esp32dev -t upload          # or -e heltec_v3
pio device monitor                     # 115200 baud
```

On boot you should see:

```
meshwatch bridge starting
[watch] advertising as meshwatch-bridge
[mesh] scanning for a Meshtastic node
[mesh] found node aa:bb:cc:dd:ee:ff (My Node)
[mesh] connected, MTU 517
[mesh] want_config_id=1234567890
[mesh] config complete, 7 nodes
[bridge] mesh=ready watch=connected nodes=7 queued=0
```

## Use it

1. Power the bridge near your Meshtastic node. It finds and bonds to the
   first node advertising the Meshtastic service.
2. Open meshwatch on the watch. It scans for `meshwatch-bridge` and connects.
3. The roster and messages arrive within a few seconds.

Sending from the watch works through the same path: the watch writes the text,
the bridge wraps it in a `ToRadio` text packet and hands it to the node. The
bridge sets `want_ack`, which is what makes a delivery receipt come back on
`ROUTING_APP` — surfacing that on the watch is the next piece of work.

## Pairing notes

Meshtastic nodes default to **just works** pairing, which is what the bridge
expects. If your node is set to a fixed PIN, change the passkey in
`onPassKeyRequest()` in `src/mesh_link.cpp`.

A node will only hold one BLE client connection. If your phone is already
connected to it, disconnect the phone first — the bridge cannot get the GATT
service otherwise.

## Protocol

What the bridge serves the watch, matching `source/GatewayClient.mc` byte for
byte. Every frame is 20 bytes or shorter.

**Bridge to watch** — the TX characteristic `a3c8f001-…`, read and notify:

| Frame | Layout |
| --- | --- |
| `END` | `0x00` — queue drained |
| `NODE` | `0x01` `[num:4 LE]` `[batt]` `[flags]` `[name… NUL-terminated]` |
| `MSG` | `0x02` `[id]` `[seq \| 0x80 on the last chunk]` `[text…]` |

`batt` is 0–100, or 101 meaning externally powered (the watch renders `PWR`).
`flags` bit 0 marks the node the bridge is paired to. Messages longer than 17
characters split across frames sharing an `id`; the watch reassembles them.

**Watch to bridge** — the RX characteristic `a3c8f002-…`, write:

| Command | Layout |
| --- | --- |
| `SYNC` | `0x01` — resend the whole roster |
| `SEND` | `0x02` `[utf8 text…]` — broadcast a text message |

The watch drains by reading TX repeatedly until it gets `END`. The bridge
notifies whenever the queue grows, which is what brings the watch back.

## Layout

```
platformio.ini      two board targets
src/main.cpp        wiring and the status heartbeat
src/model.h         shared node table, frame queue, frame builders
src/pb.h            minimal protobuf reader/writer
src/mesh_link.*     BLE central: node discovery, config sync, decode, send
src/watch_link.*    BLE peripheral: serves frames, takes commands
```

`src/pb.h` is a hand-rolled protobuf scanner rather than nanopb — we need
about eight fields, and the field numbers are kept identical to the map in
`source/Proto.mc` so the watch and the bridge drift together. Both are
commented with the same table.

## Status

Compiles clean on both targets. **Not yet run against real hardware** — the
protocol matches the watch client byte for byte and the Meshtastic decode
mirrors the field map already proven on-device, but the end-to-end path wants
a bench test. If you run it, the serial log above is what a healthy boot looks
like; please open an issue with the log if yours diverges.

## Known gaps

- Text and node info only. Position, telemetry, waypoints and alerts are
  decoded by Meshtastic but not yet forwarded — see the roadmap in the top
  level README.
- One node, one watch. No multi-node or multi-watch handling.
- Delivery ACKs are requested (`want_ack`) but the returning `ROUTING_APP`
  packet is not yet surfaced.
- The node table holds the first 32 nodes it hears about and does not evict.
