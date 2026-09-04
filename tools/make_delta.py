#!/usr/bin/env python3
"""Create an HPatchLite patch between two signed firmware images.

Requires `hdiffi` from HPatchLite on PATH (or set HDIFFI to the executable).

Example::

  python tools/make_delta.py \\
      build/app_v1_signed.bin build/app_v2_signed.bin build/app_v1_to_v2.patch

  python tools/make_delta.py --compress tuz old.bin new.bin out.patch
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT / "tools"))

from delta_common import FIRMWARE_MAGIC, logical_signed_bytes  # noqa: E402
import struct


def bytes_for_diff(path: Path) -> bytes:
    """Signed-image logical span, or raw bytes if magic is not standard (e.g. corrupt test)."""
    data = path.read_bytes()
    if len(data) >= 20:
        magic, = struct.unpack("<I", data[:4])
        if magic == FIRMWARE_MAGIC:
            return logical_signed_bytes(path)
    return data


def find_hdiffi() -> str:
    exe = os.environ.get("HDIFFI", "").strip()
    if exe:
        return exe
    found = shutil.which("hdiffi")
    if found:
        return found
    raise SystemExit(
        "error: 'hdiffi' not on PATH. Build or download HPatchLite and add it, "
        "or set HDIFFI to the hdiffi executable path."
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("old_signed", type=Path, help="Older signed image (.bin)")
    parser.add_argument("new_signed", type=Path, help="Newer signed image (.bin)")
    parser.add_argument("out_patch", type=Path, help="Output patch file")
    parser.add_argument(
        "--compress",
        choices=("none", "tuz"),
        default="none",
        help="Patch compression (default: none; tuz needs tinyuz in bootloader)",
    )
    args = parser.parse_args()

    for p in (args.old_signed, args.new_signed):
        if not p.exists():
            raise SystemExit(f"error: {p} not found")

    tmp_dir = Path(os.environ.get("MAKE_DELTA_TMP", str(ROOT / "build"))).resolve()
    tmp_dir.mkdir(parents=True, exist_ok=True)
    old_log = tmp_dir / "_make_delta_old.bin"
    new_log = tmp_dir / "_make_delta_new.bin"
    old_log.write_bytes(bytes_for_diff(args.old_signed))
    new_log.write_bytes(bytes_for_diff(args.new_signed))

    hdiffi = find_hdiffi()
    cmd = [
        hdiffi,
        "-m-6",
        "-f",
        "-p-1",
        str(old_log),
        str(new_log),
        str(args.out_patch),
    ]
    if args.compress == "tuz":
        cmd.insert(1, "-c-tuz")

    print(f"$ {' '.join(cmd)}")
    rc = subprocess.call(cmd)
    if rc != 0:
        raise SystemExit(rc)

    sz = args.out_patch.stat().st_size
    print(
        f"Wrote {args.out_patch} ({sz} B, compress={args.compress}). "
        "Use: python tools/ota_server.py --delta ... (see --help)"
    )


if __name__ == "__main__":
    main()
