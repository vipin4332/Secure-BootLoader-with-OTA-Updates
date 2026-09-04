"""Helpers for delta OTA: logical signed-image spans matching on-device layout."""

from __future__ import annotations

import hashlib
import struct
from pathlib import Path

from firmware_image import FIRMWARE_MAGIC, HEADER_SIZE


def logical_signed_bytes(image_path: Path) -> bytes:
    """Return the on-wire bytes that occupy flash: header + payload (image_size)."""
    data = image_path.read_bytes()
    if len(data) < HEADER_SIZE:
        raise ValueError(f"{image_path}: smaller than header ({len(data)} B)")
    magic, _ver, image_size, _ts, _crc = struct.unpack("<IIIII", data[:20])
    if magic != FIRMWARE_MAGIC:
        raise ValueError(f"{image_path}: bad magic 0x{magic:08x}")
    total = HEADER_SIZE + image_size
    if len(data) < total:
        raise ValueError(
            f"{image_path}: need {total} B for header+payload, have {len(data)}"
        )
    return data[:total]


def logical_signed_sha256(image_path: Path) -> bytes:
    """SHA-256 of logical_signed_bytes (matches bootloader delta base check)."""
    return hashlib.sha256(logical_signed_bytes(image_path)).digest()


def logical_signed_total_length(image_path: Path) -> int:
    return len(logical_signed_bytes(image_path))
