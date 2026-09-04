"""Unit tests for tools/delta_common.py (logical signed spans vs SHA-256)."""

from __future__ import annotations

import hashlib
import struct
import tempfile
from pathlib import Path

import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from delta_common import logical_signed_bytes  # noqa: E402
from delta_common import logical_signed_sha256  # noqa: E402
from firmware_image import FIRMWARE_MAGIC, HEADER_SIZE  # noqa: E402


def test_logical_roundtrip_matches_manual_sha():
    payload = b"\xaa\xbb" * 100
    image_size = len(payload)
    ts = 1700000000
    ver = 0x010203
    crc = __import__("zlib").crc32(payload) & 0xFFFFFFFF
    hdr_body = struct.pack("<IIIII", FIRMWARE_MAGIC, ver, image_size, ts, crc)
    sig = b"\x00" * 64
    reserved = b"\x00" * (HEADER_SIZE - 20 - 64)
    blob = hdr_body + sig + reserved + payload
    assert len(hdr_body + sig + reserved) == HEADER_SIZE

    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "x.bin"
        p.write_bytes(blob)

        span = logical_signed_bytes(p)
        assert span == blob
        dig = logical_signed_sha256(p)
        assert dig == hashlib.sha256(span).digest()


def main() -> int:
    test_logical_roundtrip_matches_manual_sha()
    print("ok: test_delta_common")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
