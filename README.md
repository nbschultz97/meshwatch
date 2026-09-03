# meshwatch

A Meshtastic client for Garmin watches. Phone-free, internet-free, just the
watch and a Meshtastic node over BLE.

The watch is the display. The node is the radio. You get the node list, the
text channels, and the position stream from the mesh, right on the wrist.

## What it does

- Scans for a paired Meshtastic node over BLE (service UUID
  `6ba1b218-15a8-461f-9fa8-5dcae273eafd`)
- Maintains a live node DB: short name, long name, battery, last-heard age,
  SNR, hop count
- Decodes text messages on the primary channel
- Reads canned messages and freeform messages from paired nodes
- Decodes incoming position packets (lat/lon/alt/time)
- Survives app exit, watch reboot, and battery death — the demo roster
  reappears on first launch and is replaced the moment a real config sync
  lands

## What it does not do

- No mesh routing. The watch is a leaf; all routing happens on the radio.
- No store, no phone, no Connect sync. Pairing is over BLE just-works.
- No solver, no telemetry, no detection logic. The watch displays what the
  mesh sends.
- No on-watch node configuration. You configure nodes in the Meshtastic app
  or via the CLI; the watch reads the result.

## Supported watches

Any round AMOLED or 64-color MIP Garmin watch running Connect IQ 3.0 or
later with Bluetooth Low Energy. There is no `tactix` product id in CIQ — the
whole tactix line ships under its fenix / epix sibling.

| Device id            | Display        | Notes                                |
| -------------------- | -------------- | ------------------------------------ |
| `fenix6xpro`         | 280×280 MIP    | tactix Delta class                   |
| `fenix7x`            | 260×260 MIP    | tactix 7 (non-AMOLED)                |
| `fenix7xpro`         | 260×260 MIP    |                                      |
| `fenix7xpronowifi`   | 260×260 MIP    |                                      |
| `epix2pro51mm`       | 416×416 AMOLED | tactix 7 AMOLED (the original dev target) |
| `fenix847mm`         | 454×454 AMOLED | tactix 8 / fenix 8 Pro 47            |
| `fenix8solar51mm`    | 416×416 AMOLED | fenix 8 Solar 51                     |
| `descentmk351mm`     | 416×416 AMOLED | Descent Mk3 51                       |
| `marq2`              | 416×416 AMOLED | MARQ Gen 2                           |
| `marq2aviator`       | 416×416 AMOLED | MARQ Gen 2 Aviator                   |

If your watch is on this list, drop the `.prg` on it. If it isn't but it's
CIQ 3.0+ with BLE, the same source will likely compile for it — add your
device id to `manifest.xml` and try `monkeyc -d <your id>`.

## Install

### 1. Build (or grab a release)

```bash
# Pick the device id from the table above
monkeyc -d epix2pro51mm -f monkey.jungle \
        -o bin/meshwatch.prg \
        -y ~/.Garmin/developer_key.der
```

CI on this repo builds every supported device on every push; pick up the
artifact for your watch from the Actions run.

### 2. Drop on the watch

Connect the watch over USB. It mounts as a mass-storage device. Copy
`meshwatch.prg` into `GARMIN/Apps/`. Eject. The app appears in the activity
list.

### 3. Pair

On the watch: open meshwatch. It will scan for any node advertising the
Meshtastic service UUID. The first node that responds becomes the paired
device. Subsequent launches reconnect automatically — the bond survives
reboot.

On the node: BLE must be enabled (default). Pairing mode "just works" is
recommended; a fixed PIN also works.

## How it works (for the curious)

Speaks the Meshtastic BLE client API. The protobufs are hand-decoded in
`source/Proto.mc` (see comments there for the field-number map) — only the
fields the watch uses, no runtime protobuf library.

Sync flow on connect: write `ToRadio{want_config_id}` → read `fromRadio`
until empty → `fromNum` notify signals new data. The `MyNodeInfo` +
`NodeInfo` packets populate the roster; `MeshPacket`/`Data` decodes into
the message log; `Position` packets land in the position stream.

## Project layout

```
manifest.xml              app id, products, BLE permission
monkey.jungle             build config
source/HeliographApp.mc   AppBase entry point
source/MainView.mc        the card view, nodes / messages / position modes
source/MainDelegate.mc    button map (UP/DOWN to scroll, START to send)
source/MeshClient.mc      BLE scan, pair, fromRadio/ToRadio framing
source/GatewayClient.mc   alternate pairing path for non-standard nodes
source/NodeStore.mc       in-memory + Storage-backed node DB
source/Proto.mc           hand-decoded Meshtastic protobuf (field-number map)
source/Layout.mc          per-device row pitch + accent color
resources/strings/        app name
resources/drawables/      launcher icon
tools/witness.py          PC-side mesh witness: connect a meshtastic node
                          over USB serial, log every text + node the watch
                          would see. Lets you prove the BLE -> mesh path
                          from the other side without a watch on hand.
```

## Build matrix

| Device            | Status   | Notes |
| ----------------- | -------- | ----- |
| `epix2pro51mm`    | BUILD OK | Primary dev target |
| `fenix6xpro`      | BUILD OK | tactix Delta class |
| `fenix7x*`        | BUILD OK | 260×260 MIP family |
| `fenix847mm`      | BUILD OK | tactix 8 / fenix 8 Pro 47 |
| `fenix8solar51mm` | BUILD OK | fenix 8 Solar 51 |
| `descentmk351mm`  | BUILD OK | |
| `marq2*`          | BUILD OK | MARQ Gen 2 |

## Troubleshooting

**The watch shows "scanning..." forever.**
The watch can pair with one node at a time. If the node is already paired
to your phone or another device, the watch can't get the GATT service.

**The watch pairs but the node list is empty.**
The first config sync after pair takes a few seconds. If you see "live"
but no nodes, the partner node has nothing to advertise yet — send a packet
from another node, or wait for a position beacon.

**Layout looks wrong on my watch.**
If your device id isn't in the supported list above, add it to
`manifest.xml` and rebuild. Pixel offsets in `MainView.mc` were tuned for
the AMOLED matrix; 280×280 MIP will look tight but readable.

**`monkeyc` fails with "device does not support API level".**
Pull the device profile for your watch through the Connect IQ SDK Manager.

## License

MIT. See `LICENSE`.

## Contributing

Open an issue with a watch model + Connect IQ version. Layout bugs are the
most common PR; pixel offsets in `MainView.mc` are the first place to look.
