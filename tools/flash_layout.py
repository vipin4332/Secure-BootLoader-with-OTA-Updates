#!/usr/bin/env python3
"""Build a 1 MB combined `flash.bin` image for QEMU / Renode.

Layout (matches common/memory_map.h):

    0x00000  Bootloader              (64 KB)
    0x10000  BootConfig              (4 KB, header + CRC, rest 0xFF)
    0x11000  Slot A (signed image)   (460 KB max, padded with 0xFF)
    0x84000  Slot B (signed image)   (optional, padded with 0xFF)
    0x100000 end

The default BootConfig record is generated here:
    slot_a_version = 0,  slot_b_version = 0
    active_slot = 'A',   boot_attempts = 0,   rollback_counter = 0
    last_good_boot = 0,  magic = BOOT_CONFIG_MAGIC,  crc32 = computed.

Optionally:
  --corrupt {slotA,slotB,bootloader}   bit-flip the first byte of a region
                                       to test the bootloader's reject path.
  --no-config                          leave the config region as 0xFF
                                       (defaults will be used at runtime).
"""

from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path

# Mirror common/memory_map.h
FLASH_TOTAL = 1 * 1024 * 1024
BOOTLOADER_OFF = 0x00000
BOOTLOADER_SIZE = 64 * 1024
CONFIG_OFF = 0x10000
CONFIG_LOGICAL_SIZE = 4 * 1024
SLOT_A_OFF = 0x11000
SLOT_B_OFF = 0x84000
SLOT_SIZE = 460 * 1024

BOOT_CONFIG_MAGIC = 0xB00710AD
SLOT_A_ID = ord("A")
SLOT_B_ID = ord("B")

CONFIG_BODY_FMT = "<II BBBB IIO"  # placeholder; real packing below


def make_default_config(active: int = SLOT_A_ID, boot_attempts: int = 0) -> bytes:
    """Pack a default BootConfig_t exactly per bootloader/include/config.h.

    Layout (24 bytes):
       0   slot_a_version (u32 LE)
       4   slot_b_version (u32 LE)
       8   active_slot    (u8)
       9   boot_attempts  (u8)
      10   rollback_count (u8)
      11   _reserved      (u8)
      12   last_good_boot (u32 LE)
      16   magic          (u32 LE)
      20   crc32          (u32 LE) - over bytes [0, 20)
    """
    body = struct.pack(
        "<II 4B II",
        0,                  # slot_a_version
        0,                  # slot_b_version
        active & 0xFF,      # active_slot
        boot_attempts & 0xFF,
        0,                  # rollback_counter
        0,                  # _reserved
        0,                  # last_good_boot
        BOOT_CONFIG_MAGIC,  # magic
    )
    assert len(body) == 20, f"body len drift: {len(body)}"
    crc = zlib.crc32(body) & 0xFFFFFFFF
    return body + struct.pack("<I", crc)


def slot_image_or_blank(path: Path | None) -> bytes:
    if path is None or not path.exists():
        return b"\xff" * SLOT_SIZE
    data = path.read_bytes()
    if len(data) > SLOT_SIZE:
        raise SystemExit(
            f"error: {path} is {len(data)} B, exceeds slot size {SLOT_SIZE}"
        )
    return data + b"\xff" * (SLOT_SIZE - len(data))


def build(args: argparse.Namespace) -> bytes:
    out = bytearray(b"\xff" * FLASH_TOTAL)

    # Bootloader.
    bl = args.bootloader.read_bytes()
    if len(bl) > BOOTLOADER_SIZE:
        raise SystemExit(
            f"error: {args.bootloader} is {len(bl)} B, exceeds "
            f"bootloader partition {BOOTLOADER_SIZE} B"
        )
    out[BOOTLOADER_OFF : BOOTLOADER_OFF + len(bl)] = bl

    # Boot config.
    if not args.no_config:
        cfg = make_default_config(
            SLOT_A_ID if args.active == "A" else SLOT_B_ID,
            boot_attempts=args.boot_attempts,
        )
        out[CONFIG_OFF : CONFIG_OFF + len(cfg)] = cfg

    # Slot A.
    slot_a = slot_image_or_blank(args.slot_a)
    out[SLOT_A_OFF : SLOT_A_OFF + SLOT_SIZE] = slot_a

    # Slot B.
    slot_b = slot_image_or_blank(args.slot_b)
    out[SLOT_B_OFF : SLOT_B_OFF + SLOT_SIZE] = slot_b

    # Optional corruption.
    if args.corrupt:
        regions = {
            "bootloader": BOOTLOADER_OFF,
            "config":     CONFIG_OFF,
            "slotA":      SLOT_A_OFF,
            "slotB":      SLOT_B_OFF,
        }
        if args.corrupt not in regions:
            raise SystemExit(f"error: --corrupt must be one of {list(regions)}")
        off = regions[args.corrupt]
        out[off] ^= 0xFF
        print(f"corrupted byte at 0x{off:06x} (region '{args.corrupt}')")

    return bytes(out)


def print_map() -> None:
    print(
        "Memory map (matches common/memory_map.h):\n"
        f"  0x{BOOTLOADER_OFF:06x}  Bootloader     (64 KB)\n"
        f"  0x{CONFIG_OFF:06x}  BootConfig     ({CONFIG_LOGICAL_SIZE//1024} KB logical)\n"
        f"  0x{SLOT_A_OFF:06x}  Slot A         (460 KB)\n"
        f"  0x{SLOT_B_OFF:06x}  Slot B         (460 KB)\n"
        f"  0x{FLASH_TOTAL:06x}  end\n"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootloader", type=Path, required=True)
    parser.add_argument("--slot-a", type=Path, default=None,
                        dest="slot_a", help="Signed image for Slot A")
    parser.add_argument("--slot-b", type=Path, default=None,
                        dest="slot_b", help="Signed image for Slot B")
    parser.add_argument("--out", type=Path, default=Path("build/flash.bin"))
    parser.add_argument("--active", choices=["A", "B"], default="A")
    parser.add_argument(
        "--boot-attempts",
        type=int,
        default=0,
        help="Initial boot_attempts in BootConfig (for rollback tests)",
    )
    parser.add_argument("--no-config", action="store_true",
                        help="Leave config region as 0xFF; runtime defaults")
    parser.add_argument("--corrupt", default=None,
                        help="Region to bit-flip: bootloader|config|slotA|slotB")
    parser.add_argument("--print-map", action="store_true")
    args = parser.parse_args()

    if args.print_map:
        print_map()

    blob = build(args)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    print(f"Wrote {args.out} ({len(blob)} B)")


if __name__ == "__main__":
    main()
