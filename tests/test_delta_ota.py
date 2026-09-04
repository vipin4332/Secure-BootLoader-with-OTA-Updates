"""QEMU integration: delta OTA via recovery (START_DELTA + HPatchLite).

Uses a corrupted Slot A as the on-device old image; host patch and
base_sha256 are derived from that corrupted flash content so the
bootloader's active-slot hash check matches.
"""

from __future__ import annotations

import hashlib
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ota_protocol as ota  # noqa: E402
from delta_common import logical_signed_bytes, logical_signed_total_length  # noqa: E402
from firmware_image import FIRMWARE_MAGIC, HEADER_SIZE  # noqa: E402

QEMU_BIN = "qemu-system-arm"
SLOT_A_OFF = 0x11000
SLOT_SIZE = 460 * 1024
BL_BIN = ROOT / "build" / "bootloader" / "bootloader.bin"
LOG_FILE = ROOT / "build" / "qemu_delta.log"
FLASH_DELTA = ROOT / "build" / "flash_delta.bin"
PATCH_FILE = ROOT / "build" / "app_v1_to_v2.patch"
V1_SIGNED = ROOT / "build" / "app_v1_signed.bin"
V2_SIGNED = ROOT / "build" / "app_v2_signed.bin"


def have_qemu() -> bool:
    return shutil.which(QEMU_BIN) is not None


def have_hdiffi() -> bool:
    return shutil.which("hdiffi") is not None or bool(
        __import__("os").environ.get("HDIFFI")
    )


def skip(msg: str) -> int:
    print(f"SKIP: {msg}")
    return 2


def recv_exact(sock: socket.socket, n: int, timeout: float = 5.0) -> bytes:
    buf = b""
    sock.settimeout(timeout)
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise TimeoutError(f"connection closed with {len(buf)}/{n} bytes")
        buf += chunk
    return buf


def logical_span_from_slot_slice(slot_data: bytes, expected_total: int) -> bytes:
    """Return the first @p expected_total bytes (corrupt images may have bad magic)."""
    if len(slot_data) < expected_total:
        raise ValueError(f"slot slice too small: need {expected_total}, have {len(slot_data)}")
    return slot_data[:expected_total]


def build_flash_and_patch() -> None:
    """Flash image with valid Slot A header but corrupted payload (still enters recovery)."""
    subprocess.check_call(
        [
            sys.executable,
            str(ROOT / "tools" / "flash_layout.py"),
            "--bootloader", str(BL_BIN),
            "--slot-a", str(V1_SIGNED),
            "--out", str(FLASH_DELTA),
        ]
    )
    flash = bytearray(FLASH_DELTA.read_bytes())
    logical_total = len(logical_signed_bytes(V1_SIGNED))
    corrupt_off = SLOT_A_OFF + HEADER_SIZE + 16
    flash[corrupt_off] ^= 0xFF
    FLASH_DELTA.write_bytes(flash)
    flash = FLASH_DELTA.read_bytes()
    logical_total = len(logical_signed_bytes(V1_SIGNED))
    old_on_device = flash[SLOT_A_OFF : SLOT_A_OFF + logical_total]
    old_tmp = ROOT / "build" / "_delta_old_on_device.bin"
    new_tmp = ROOT / "build" / "_delta_new_logical.bin"
    old_tmp.write_bytes(old_on_device)
    new_tmp.write_bytes(logical_signed_bytes(V2_SIGNED))

    subprocess.check_call(
        [
            sys.executable,
            str(ROOT / "tools" / "make_delta.py"),
            str(old_tmp),
            str(new_tmp),
            str(PATCH_FILE),
        ]
    )


def push_delta_recovery() -> int:
    if LOG_FILE.exists():
        LOG_FILE.unlink()

    qemu = subprocess.Popen(
        [
            QEMU_BIN,
            "-machine", "netduino2",
            "-nographic",
            "-kernel", str(FLASH_DELTA),
            "-serial", f"file:{LOG_FILE}",
            "-serial", "tcp:127.0.0.1:4444,server,nowait",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        time.sleep(0.6)
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5.0)
        sock.connect(("127.0.0.1", 4444))
        sock.sendall(b"U")
        time.sleep(0.3)

        patch = PATCH_FILE.read_bytes()
        flash = FLASH_DELTA.read_bytes()
        logical_total = len(logical_signed_bytes(V1_SIGNED))
        old_on_device = flash[SLOT_A_OFF : SLOT_A_OFF + logical_total]
        digest = hashlib.sha256(old_on_device).digest()
        expected_new = logical_signed_total_length(V2_SIGNED)

        seq = 1
        payload = struct.pack("<II", len(patch), expected_new) + digest
        sock.sendall(ota.build_frame(ota.OP_START_DELTA, seq, payload))
        resp = recv_exact(sock, 3)
        if resp[0] != ota.ACK:
            print(f"FAIL: START_DELTA not ACKed: {resp!r}")
            return 1

        offset = 0
        chunk = 256
        while offset < len(patch):
            seq = (seq + 1) & 0xFFFF
            blob = patch[offset : offset + chunk]
            sock.sendall(ota.build_frame(ota.OP_DATA, seq, blob))
            resp = recv_exact(sock, 3)
            if resp[0] != ota.ACK:
                print(f"FAIL: DATA NAK at seq={seq}")
                return 1
            offset += len(blob)

        seq = (seq + 1) & 0xFFFF
        sock.sendall(ota.build_frame(ota.OP_END, seq))
        try:
            recv_exact(sock, 3, timeout=3.0)
        except TimeoutError:
            pass
        sock.close()
        time.sleep(1.0)
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass

    if not LOG_FILE.exists():
        print("FAIL: no QEMU log")
        return 1
    log = LOG_FILE.read_text(errors="replace")
    print("--- bootloader log ---")
    print(log)
    for needle in (
        "RECOVERY MODE",
        "OTA : END verified",
        "OTA swap committed",
        "INFO: jumping to slot",
    ):
        if needle not in log:
            print(f"FAIL: missing log line containing {needle!r}")
            return 1
    print("PASS: recovery + delta OTA + swap confirmed in log")
    return 0


def main() -> int:
    if not have_qemu():
        return skip("qemu-system-arm not on PATH")
    if not have_hdiffi():
        return skip("hdiffi not on PATH (set HDIFFI=...)")
    for p in (BL_BIN, V1_SIGNED, V2_SIGNED):
        if not p.exists():
            return skip(f"{p} missing; run 'make keys all' and 'make delta'")
    subprocess.check_call(["make", "delta"], cwd=ROOT)
    build_flash_and_patch()
    return push_delta_recovery()


if __name__ == "__main__":
    sys.exit(main())
