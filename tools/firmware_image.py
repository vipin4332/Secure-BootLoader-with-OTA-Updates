"""Shared helpers for the firmware-image pipeline.

Encapsulates the on-flash FirmwareHeader_t layout in one place so the
create_image / sign_firmware / flash_layout tools all agree on the same
byte offsets. Mirrors common/firmware_format.h:

    offset  size   field
    ------  ----   --------------------------------
       0     4     magic
       4     4     version
       8     4     image_size
      12     4     timestamp
      16     4     crc32
      20    64     signature
      84   428     reserved (zero-padded)
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

HEADER_SIZE = 512
SIGNATURE_SIZE = 64
RESERVED_SIZE = HEADER_SIZE - 4 - 4 - 4 - 4 - 4 - SIGNATURE_SIZE  # 428
SIGNATURE_OFFSET = 20  # bytes from start of header
FIRMWARE_MAGIC = 0xDEADBEEF

assert RESERVED_SIZE == 428, "header arithmetic drift"


@dataclass
class FirmwareImage:
    """In-memory representation of a firmware image."""

    version: int
    timestamp: int
    payload: bytes
    signature: bytes = field(default_factory=lambda: b"\x00" * SIGNATURE_SIZE)

    @property
    def image_size(self) -> int:
        return len(self.payload)

    @property
    def payload_crc32(self) -> int:
        return zlib.crc32(self.payload) & 0xFFFFFFFF

    def header_bytes(self, signature: bytes | None = None) -> bytes:
        """Serialise the 512-byte header.

        If ``signature`` is None the current ``self.signature`` field is used.
        Pass an explicit zero-bytes signature to obtain the canonical
        signing-region header that the bootloader hashes during verify.
        """
        sig = signature if signature is not None else self.signature
        if len(sig) != SIGNATURE_SIZE:
            raise ValueError(
                f"signature must be {SIGNATURE_SIZE} bytes, got {len(sig)}"
            )
        body = struct.pack(
            "<IIIII",
            FIRMWARE_MAGIC,
            self.version,
            self.image_size,
            self.timestamp,
            self.payload_crc32,
        )
        assert len(body) == 20
        return body + sig + b"\x00" * RESERVED_SIZE

    def signing_bytes(self) -> bytes:
        """Bytes the bootloader hashes: header(zeroed-sig) + payload."""
        return self.header_bytes(b"\x00" * SIGNATURE_SIZE) + self.payload

    def signed_image(self) -> bytes:
        return self.header_bytes() + self.payload


def parse_version(spec: str) -> int:
    """Parse a semver string ``major.minor.patch`` into ``0xMMmmpp``."""
    parts = spec.split(".")
    if len(parts) != 3:
        raise ValueError(f"version must be major.minor.patch, got {spec!r}")
    major, minor, patch = (int(x) for x in parts)
    if not (0 <= major <= 0xFF and 0 <= minor <= 0xFF and 0 <= patch <= 0xFF):
        raise ValueError("each version component must fit in a byte")
    return (major << 16) | (minor << 8) | patch


def format_version(v: int) -> str:
    return f"{(v >> 16) & 0xFF}.{(v >> 8) & 0xFF}.{v & 0xFF}"


def read_payload(path: Path) -> bytes:
    data = path.read_bytes()
    if not data:
        raise ValueError(f"{path} is empty")
    return data
