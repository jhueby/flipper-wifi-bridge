# Flipper WiFi RPC Bridge (ESP32-S2 dev board)

Turns the official Flipper Zero WiFi Dev Board into a **wireless bridge to the
Flipper's RPC interface**. The board makes its own WiFi network; your PC joins it
and controls the Flipper with `flipper_rpc.py --host 192.168.4.1` — the same client
that works over USB.

This is **v1: bridge-only** (it replaces whatever is on the board, e.g. Marauder —
recoverable any time via the Marauder web flasher). Marauder dual-boot is a planned
follow-up layout.

## How it works
- SoftAP `flipper-bridge` / `flipper1234`, TCP `:2333` at `192.168.4.1`.
- Talks to the Flipper over the dev board's UART (ESP **GPIO43/44**) using the
  Flipper **Expansion Module** protocol to open an RPC session, then tunnels raw RPC
  bytes between TCP and the Flipper.
- Status LED: **blue** = waiting / handshaking, **green** = RPC session up,
  **red** = handshake failed (is "Expansion Modules" enabled on the Flipper?).

## Build (GitHub Actions — no local ESP-IDF needed)
Push to `main` (or run the workflow manually). The `build` job compiles with
ESP-IDF v5.3.1 for `esp32s2` and uploads **`flipper-wifi-bridge-bins`** containing:
- `bootloader.bin`
- `partition-table.bin`
- `flipper_wifi_bridge.bin`
- `flash_offsets.txt` (the exact offsets below, straight from the build)

Typical ESP32-S2 offsets: bootloader `0x1000`, partition table `0x8000`,
app (factory) `0x10000`. Always trust `flash_offsets.txt` over this note.

## Flash through the Flipper (no dev-board USB-C)
1. Copy the three `.bin`s to the Flipper SD card (e.g. `/ext/esp_flasher/`).
2. On the Flipper, run **ESP Flasher** → set Bootloader/Part Table/App to the three
   files at the offsets above → **Flash**.
3. On the Flipper: **Settings → Expansion Modules → ON** (this is what lets the
   bridge open an RPC session). Leave the Flipper at its desktop.

## Use
1. Join WiFi `flipper-bridge` (pass `flipper1234`).
2. `python flipper_rpc.py --host 192.168.4.1 ping`
3. `python flipper_rpc.py --host 192.168.4.1 screenshot flip.png`

## Restore Marauder
Re-flash Marauder with its normal web flasher / ESP Flasher package. This bridge
touches only the app; nothing here is destructive beyond the app partition.

## Roadmap
- [ ] Prove v1 bridge end-to-end (ping / keys / screenshot over WiFi).
- [ ] STA mode (join home WiFi) as an option.
- [ ] Dual-boot: `factory`=Marauder + `test`=bridge, selected externally (GPIO0 is
      the download strap so it can't select; likely a Flipper-driven GPIO via a
      custom bootloader, pending the dev-board schematic).
