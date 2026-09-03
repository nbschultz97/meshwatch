# Changelog

## Unreleased

Public release. The repo is renamed on GitHub from
`nbschultz97/heliograph` to `nbschultz97/meshwatch` and flipped from private
to public.

### Breaking changes for anyone tracking the old repo

- **Repo name**: `nbschultz97/heliograph` -> `nbschultz97/meshwatch`.
  Source tree unchanged; just a GitHub-side rename. The local manifest's
  application id is unchanged.
- **`breaze/` and `gateway/` removed.** Both were Heltec V3 firmware
  projects that grew alongside the watch app:
  - `breaze/` was a solar sentry node that spoke PICKET air params; the
    same work continues (and is more advanced) as `picket/firmware/relay/`
    in `nbschultz97/picket`. The PICKET-side firmware picked up every
    BREAZE-era fix (CAD-before-TX, hold queue, panel-sense options, PSK
    secrets) plus more.
  - `gateway/` was a Meshtastic BLE bridge; picket has its own
    `firmware/gateway/`, which speaks the PICKET HMAC protocol rather than
    the Meshtastic one.
  If you need the older form of either, pull commit `954bc50` of this repo
  from before the move.

### Watch app

- **Detection-photo feature removed.** The app carried a thumbnail receiver
  (`0x03` frames, 128 chunks x 16B into a 64x64 4bpp buffer) that had nothing
  to do with Meshtastic — the protocol has no image port at all, and upstream
  has said file transfer will not be supported over sub-GHz LoRa. Shipping it
  in a Meshtastic client implied a protocol feature that does not exist. The
  full working implementation, including the watchdog-safe BufferedBitmap
  render, moved to the private project it belongs to; nothing was lost.
- **README now states the transport situation plainly.** Connect IQ caps BLE
  characteristic reads and writes at ~20 bytes with no MTU negotiation, which
  truncates every Meshtastic packet past its header. The bridge firmware this
  app was developed against served a canned roster over a private LoRa
  protocol and never spoke Meshtastic. The README says so, rather than
  implying you can pair to a stock node and get a working client.

- **Multi-watch.** The single `epix2pro51mm` target grew into a 10-product
  manifest covering the round-AMOLED CIQ 3.0+ matrix: tactix Delta class
  (`fenix6xpro`), tactix 7 non-AMOLED (`fenix7x*`), tactix 7 AMOLED
  (`epix2pro51mm`), tactix 8 / fenix 8 Pro 47 (`fenix847mm`), fenix 8 Solar
  51 (`fenix8solar51mm`), Descent Mk3 51 (`descentmk351mm`), MARQ Gen 2
  (`marq2`, `marq2aviator`). Layout constants for row pitch and accent
  color moved into a new `source/Layout.mc` that picks the right table by
  display dimensions at runtime.
- **`minApiLevel` dropped from 5.0.0 to 3.0.0.** Nothing in the code used
  CIQ-5-only APIs; the 5.0.0 minimum was locking out the tactix Delta
  class without reason.
- **Demo roster is now generic.** The pre-launch placeholder names that
  used to read `NOAH`, `OS01`, `OS02 (pi12)`, `PICKET UGV` (the personal
  and PICKET-flavored callsigns of the original development setup) became
  `WATCH`, `ALPH`, `BRAV`, `CHAR`. Node ids moved from the `0x1000000*`
  range to `0xA0000001..4` so they cannot collide with a real Meshtastic
  node anyone might happen to have on their mesh.

### Repo hygiene

- Public release under MIT (see `LICENSE`).
- `.gitignore` updated for the trimmed tree (no more `gateway/.pio/` or
  `breaze/.pio/`).
- New `.github/workflows/ci.yml` builds the `.prg` for every supported
  device on every push and PR; matrix fails loudly if a code change
  breaks one watch class while still building on another.

## 2026-08-20 — milestone 1

The original private release: BLE connect + node DB sync for the tactix 7
AMOLED. Captured in commit history. Not part of this public release; see
the commit log if you need the pre-public code.
