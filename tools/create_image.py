#!/usr/bin/env python3
"""Wrap a raw application binary in the FirmwareHeader_t format.

Output layout:
    [ 512-byte header (signature field zeroed) | application payload ]

The CRC32 field is computed and stamped into the header here; the signature
field is left as 64 zero bytes for sign_firmware.py to fill.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from firmware_image import FirmwareImage, parse_version, format_version


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("payload", type=Path, help="Raw application .bin")
    parser.add_argument("output", type=Path, help="Path for unsigned image")
    parser.add_argument(
        "--version", required=True, help="Semver, e.g. 1.0.0"
    )
    parser.add_argument(
        "--timestamp",
        type=int,
        default=None,
        help="Unix timestamp; defaults to now()",
    )
    args = parser.parse_args()

    version = parse_version(args.version)
    timestamp = args.timestamp if args.timestamp is not None else int(time.time())

    payload = args.payload.read_bytes()
    if not payload:
        sys.stderr.write(f"error: {args.payload} is empty\n")
        sys.exit(1)

    img = FirmwareImage(version=version, timestamp=timestamp, payload=payload)
    out_bytes = img.signed_image()  # signature is still zero
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(out_bytes)

    print(
        f"Wrote {args.output}: "
        f"{len(out_bytes)} B "
        f"(header 512 + payload {img.image_size}), "
        f"v{format_version(version)}, "
        f"crc32 0x{img.payload_crc32:08x}"
    )


if __name__ == "__main__":
    main()
