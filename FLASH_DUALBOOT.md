# Dual-boot flash: Marauder (ota_0) + WiFi bridge (ota_1)

One combined flash puts **both** firmwares on the ESP32-S2 dev board. A plain reset
boots **Marauder** (the board's normal identity). A 2-second otadata write boots the
**bridge**; from the bridge you hop back to Marauder **wirelessly**.

## Layout (Arduino "default" 4 MB scheme — Marauder runs unchanged)

| Partition | Offset   | Contents                                   |
|-----------|----------|--------------------------------------------|
| bootloader| 0x1000   | shared (IDF 5.3.1 build)                    |
| part table| 0x8000   | combined `partition-table.bin`              |
| otadata   | 0xe000   | boot selector (blank = ota_0 = Marauder)    |
| ota_0     | 0x10000  | **Marauder** `*_flipper.bin` (1.06 MB)      |
| ota_1     | 0x150000 | **bridge** `flipper_wifi_bridge.bin` (0.77 MB) |
| spiffs    | 0x290000 | Marauder's filesystem (mounted by label)    |

## 0. Get the files

From the GitHub Actions run (artifact `flipper-wifi-bridge-bins`): `bootloader.bin`,
`partition-table.bin`, `flipper_wifi_bridge.bin`, `otadata_ota1.bin` (+ `_altcrc`),
`otadata_ota0.bin`.

Marauder app (v1.19.2 for the Flipper WiFi devboard):
```bash
curl -L -o marauder_flipper.bin \
  https://github.com/justcallmekoko/ESP32Marauder/releases/download/v1.19.2/esp32_marauder_v1_19_2_20261023_flipper.bin
```

## 1. Board into download mode

Hold **BOOT**, tap **RESET**, release **BOOT**. It enumerates as a COM port
(check which: `Get-PnpDevice -Class Ports`). Examples below use `COM4`.

## 2. One combined flash (otadata left erased → boots Marauder)

```bash
python -m esptool --chip esp32s2 --port COM4 --no-stub --after no_reset write_flash \
  0x1000   bootloader.bin \
  0x8000   partition-table.bin \
  0x10000  marauder_flipper.bin \
  0x150000 flipper_wifi_bridge.bin
```
`--no-stub` is REQUIRED on the S2 native USB (the stub re-enumerates and kills the
port). Tap **RESET**.

## 3. Verify Marauder

The board should boot straight into Marauder (its normal behavior). If it does,
the shared IDF bootloader boots the Arduino app fine and the layout is good.

> If Marauder does NOT boot (unlikely): flash Marauder's own bootloader instead of
> ours at 0x1000 — pull it from the Marauder web flasher's bundle — keeping the same
> `partition-table.bin` and both apps. Bootloaders are app-agnostic; the table + apps
> don't change. Marauder is always fully recoverable via the web flasher.

## 4. Boot the bridge (switch INTO ota_1)

Board in download mode again, then write the selector (8 KB, ~2 s):
```bash
python -m esptool --chip esp32s2 --port COM4 --no-stub write_flash 0xe000 otadata_ota1.bin
```
Tap RESET → LED goes **green**, SoftAP `flipper-bridge` comes up, drive it with
`flipper_rpc.py --host 192.168.4.1`.

> If it boots Marauder instead, the otadata CRC convention is the other one — flash
> `otadata_ota1_altcrc.bin` at 0xe000 instead, and note which worked.

## 5. Hop back to Marauder — WIRELESS, no USB

While on the bridge's AP:
```bash
python -c "import socket; print(socket.create_connection(('192.168.4.1',2335)).recv(64))"
```
The bridge points otadata at ota_0 and reboots → Marauder. (Equivalent over USB:
write `otadata_ota0.bin` at 0xe000, or erase 0xe000.)

## Summary of the switch

- **Marauder → bridge:** 2-second `otadata_ota1.bin` write over USB (download mode).
- **bridge → Marauder:** one wireless TCP connect to `:2335`. No cable.
- Default/blank otadata always falls back to Marauder — the safe state.
