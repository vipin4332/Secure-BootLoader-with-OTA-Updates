#!/usr/bin/env python3
"""Sign an unsigned firmware image (output of create_image.py).

Sequence:
  1. Read the unsigned image (header + payload, signature field == zeros).
  2. Verify the embedded CRC32 matches the payload (sanity).
  3. Compute SHA-256 over the canonical signing region:
         [header_with_signature_field_zeroed || payload]
  4. ECDSA-sign the digest with keys/private_key.pem.
  5. Splat the raw r||s (64 B big-endian) into the header's signature field.
  6. Write the signed image.

The signature format is exactly what micro-ecc's uECC_verify() consumes
(no DER, no ASN.1).
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

try:
    from ecdsa import SigningKey
except ImportError:  # pragma: no cover
    sys.stderr.write(
        "error: missing dependency 'ecdsa'. Install with:\n"
        "    python -m pip install -r tools/requirements.txt\n"
    )
    sys.exit(2)

from firmware_image import (
    FirmwareImage,
    HEADER_SIZE,
    SIGNATURE_OFFSET,
    SIGNATURE_SIZE,
    FIRMWARE_MAGIC,
)


def parse_unsigned(blob: bytes) -> FirmwareImage:
    if len(blob) <= HEADER_SIZE:
        raise ValueError(
            f"image too small ({len(blob)} B) - expected > {HEADER_SIZE}"
        )
    magic, version, image_size, timestamp, crc = struct.unpack(
        "<IIIII", blob[:20]
    )
    if magic != FIRMWARE_MAGIC:
        raise ValueError(f"bad magic 0x{magic:08x}")
    payload = blob[HEADER_SIZE : HEADER_SIZE + image_size]
    if len(payload) != image_size:
        raise ValueError(
            f"payload short: header says {image_size}, "
            f"file has {len(payload)} B"
        )
    img = FirmwareImage(version=version, timestamp=timestamp, payload=payload)
    if img.payload_crc32 != crc:
        raise ValueError(
            f"CRC mismatch: header 0x{crc:08x}, computed "
            f"0x{img.payload_crc32:08x}"
        )
    return img


def sign(sk: SigningKey, signing_bytes: bytes) -> bytes:
    """Return raw r||s signature, 64 bytes big-endian with canonical low-s."""
    digest = hashlib.sha256(signing_bytes).digest()
    def sigencode_canonical(r: int, s: int, order: int) -> bytes:
        # Enforce low-S (RFC 6979 / BIP 62) to prevent signature malleability
        if s > order // 2:
            s = order - s
        return r.to_bytes(32, "big") + s.to_bytes(32, "big")
    return sk.sign_digest(digest, sigencode=sigencode_canonical)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Unsigned image (.bin)")
    parser.add_argument("output", type=Path, help="Signed image output")
    parser.add_argument(
        "--key",
        type=Path,
        default=Path("keys/private_key.pem"),
        help="Private key PEM (default: keys/private_key.pem)",
    )
    args = parser.parse_args()

    if not args.key.exists():
        sys.stderr.write(
            f"error: private key {args.key} not found. "
            f"Run 'make keys' first.\n"
        )
        sys.exit(1)

    blob = args.input.read_bytes()
    img = parse_unsigned(blob)
    sk = SigningKey.from_pem(args.key.read_bytes())
    sig = sign(sk, img.signing_bytes())
    if len(sig) != SIGNATURE_SIZE:
        sys.stderr.write(f"error: signature length {len(sig)} != 64\n")
        sys.exit(1)
    img.signature = sig

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(img.signed_image())
    print(
        f"Signed {args.input} -> {args.output} "
        f"(payload {img.image_size} B, sig {SIGNATURE_SIZE} B at offset "
        f"{SIGNATURE_OFFSET})"
    )


if __name__ == "__main__":
    main()
