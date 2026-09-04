"""Wire-format helpers for the bootloader's UART OTA protocol.

Mirrors bootloader/include/ota_client.h. Frame format (little-endian):

    [SOH(1)=0x01][OP(1)][SEQ(2)][LEN(2)][DATA(LEN)][CRC32(4)]

Response: [ACK(1)=0x06 | NAK(1)=0x15][SEQ(2)]

CRC32 covers OP || SEQ || LEN || DATA, IEEE 802.3 polynomial.
"""

from __future__ import annotations

import struct
import zlib

SOH         = 0x01
ACK         = 0x06
NAK         = 0x15

OP_START    = 0x21
OP_DATA     = 0x22
OP_END      = 0x23
OP_START_DELTA = 0x24
OP_ABORT    = 0x2F

MAX_PAYLOAD = 1024
RESPONSE_SIZE = 3   # ACK/NAK + 2-byte SEQ


def build_frame(op: int, seq: int, data: bytes = b"") -> bytes:
    """Encode a single frame. Returns the bytes to put on the wire."""
    if len(data) > MAX_PAYLOAD:
        raise ValueError(f"payload too large: {len(data)} > {MAX_PAYLOAD}")
    body = struct.pack("<BHH", op & 0xFF, seq & 0xFFFF, len(data)) + data
    crc = zlib.crc32(body) & 0xFFFFFFFF
    return bytes([SOH]) + body + struct.pack("<I", crc)


def parse_response(buf: bytes) -> tuple[int, int]:
    """Decode an ACK/NAK response. Raises on short input or unknown code."""
    if len(buf) != RESPONSE_SIZE:
        raise ValueError(f"response must be {RESPONSE_SIZE} B, got {len(buf)}")
    code, seq = struct.unpack("<BH", buf)
    if code not in (ACK, NAK):
        raise ValueError(f"unknown response byte 0x{code:02x}")
    return code, seq


def is_ack(code: int) -> bool:
    return code == ACK
