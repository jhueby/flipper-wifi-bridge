#!/usr/bin/env python3
"""Generate otadata images that select an OTA slot at boot, to flash at 0xe000.

The ESP-IDF 2nd-stage bootloader reads the `otadata` partition (two 4 KB sectors,
each an esp_ota_select_entry_t) and boots slot `(ota_seq - 1) % ota_count`. With
two app slots: seq=1 -> ota_0 (Marauder), seq=2 -> ota_1 (this bridge).

Entry layout (32 bytes):
    uint32 ota_seq
    uint8  seq_label[20]
    uint32 ota_state   (0xFFFFFFFF = UNDEFINED; bootloader ignores it unless
                        rollback is enabled, which it isn't here)
    uint32 crc         (CRC32 over the 4 ota_seq bytes, bootloader's convention)

CRC caveat: the ESP ROM crc32_le convention vs zlib's final-XOR is easy to get
wrong, and the bootloader simply treats a bad-CRC entry as empty -> boots ota_0
(Marauder) -- a SAFE failure. So we emit the primary (plain zlib.crc32, the
community-verified value) AND an `_altcrc` fallback (xor 0xFFFFFFFF). If flashing
the primary boots Marauder instead of the bridge, flash the _altcrc one.

Usage: python make_otadata.py   # writes the .bin files next to this script
"""
import struct, zlib, os

SECTOR = 0x1000
BLANK = b"\xff"

def entry(seq, invert_crc=False):
    seq_bytes = struct.pack("<I", seq)
    crc = zlib.crc32(seq_bytes) & 0xFFFFFFFF
    if invert_crc:
        crc ^= 0xFFFFFFFF
    e = seq_bytes + (BLANK * 20) + struct.pack("<I", 0xFFFFFFFF) + struct.pack("<I", crc)
    assert len(e) == 32
    return e

def image(seq, invert_crc=False):
    # entry in sector 0, sector 1 left blank (invalid) so sector 0 wins
    sec0 = entry(seq, invert_crc) + BLANK * (SECTOR - 32)
    sec1 = BLANK * SECTOR
    return sec0 + sec1

here = os.path.dirname(os.path.abspath(__file__))
def w(name, data):
    p = os.path.join(here, name)
    with open(p, "wb") as f:
        f.write(data)
    print(f"wrote {name} ({len(data)} bytes)")

w("otadata_ota0.bin",        image(1))            # -> Marauder
w("otadata_ota1.bin",        image(2))            # -> bridge (primary)
w("otadata_ota1_altcrc.bin", image(2, True))      # -> bridge (CRC fallback)
