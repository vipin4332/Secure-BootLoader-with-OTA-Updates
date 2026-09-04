"""Integration test: OTA push into recovery mode.

Boots QEMU with a corrupted Slot A (so the bootloader enters recovery),
sends 'U' on TCP 4444 to trigger the OTA receiver, then pushes the
freshly-signed app and verifies the bootloader reports success.

Skipped automatically if `qemu-system-arm` is not on PATH.
"""

from __future__ import annotations

import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import ota_protocol as ota  # noqa: E402

QEMU_BIN = "qemu-system-arm"
APP_SIGNED = ROOT / "build" / "app_signed.bin"
FLASH_CORRUPT = ROOT / "build" / "flash_corrupt.bin"
LOG_FILE = ROOT / "build" / "qemu_recovery.log"
BL_BIN = ROOT / "build" / "bootloader" / "bootloader.bin"


def have_qemu() -> bool:
    return shutil.which(QEMU_BIN) is not None


def build_corrupt_flash() -> None:
    subprocess.check_call(
        [
            sys.executable,
            str(ROOT / "tools" / "flash_layout.py"),
            "--bootloader", str(BL_BIN),
            "--slot-a", str(APP_SIGNED),
            "--corrupt", "slotA",
            "--out", str(FLASH_CORRUPT),
        ]
    )


def recv_exact(sock: socket.socket, n: int, timeout: float = 5.0) -> bytes:
    """Accumulate exactly n bytes from sock or raise on timeout."""
    buf = b""
    sock.settimeout(timeout)
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise TimeoutError(
                f"connection closed with {len(buf)}/{n} bytes received"
            )
        buf += chunk
    return buf


def push_to_recovery() -> int:
    if LOG_FILE.exists():
        LOG_FILE.unlink()
    qemu = subprocess.Popen(
        [
            QEMU_BIN,
            "-machine", "netduino2",
            "-nographic",
            "-kernel", str(FLASH_CORRUPT),
            "-serial", f"file:{LOG_FILE}",
            "-serial", "tcp:127.0.0.1:4444,server,nowait",
        ],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        # Give the bootloader time to print the recovery banner and start
        # listening on USART2.
        time.sleep(0.6)

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5.0)
        sock.connect(("127.0.0.1", 4444))
        # Send 'U' to switch into the OTA receiver.
        sock.sendall(b"U")
        time.sleep(0.3)

        image = APP_SIGNED.read_bytes()
        # START frame
        seq = 1
        sock.sendall(ota.build_frame(ota.OP_START, seq, len(image).to_bytes(4, "little")))
        resp = recv_exact(sock, 3)
        assert resp[0] == ota.ACK, f"START not ACKed: {resp!r}"

        t_start = time.monotonic()

        # DATA frames
        chunk = 256
        offset = 0
        while offset < len(image):
            seq = (seq + 1) & 0xFFFF
            blob = image[offset:offset + chunk]
            sock.sendall(ota.build_frame(ota.OP_DATA, seq, blob))
            resp = recv_exact(sock, 3)
            if resp[0] != ota.ACK:
                print(f"DATA NAK at seq={seq}: {resp!r}")
                return 1
            offset += len(blob)

        # END frame
        seq = (seq + 1) & 0xFFFF
        sock.sendall(ota.build_frame(ota.OP_END, seq))
        try:
            resp = recv_exact(sock, 3, timeout=3.0)
        except TimeoutError:
            resp = b""

        t_end = time.monotonic()
        ota_seconds = t_end - t_start
        print(f"OTA payload transfer (incl. ACKs and verify): "
              f"{len(image)} B in {ota_seconds*1000:.1f} ms "
              f"= {len(image) / max(ota_seconds, 1e-3) / 1024:.1f} KB/s")
        sock.close()

        # The bootloader should now reset; we wait for a fresh banner from
        # the post-reset boot pass before sampling the log.
        time.sleep(1.0)
    finally:
        qemu.kill()
        try:
            qemu.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass

    if not LOG_FILE.exists():
        print("FAIL: no log produced")
        return 1
    log = LOG_FILE.read_text(errors="replace")
    print("--- bootloader log ---")
    print(log)
    if "RECOVERY MODE" not in log:
        print("FAIL: did not enter recovery mode")
        return 1
    if "OTA : END verified" not in log:
        print("FAIL: OTA did not complete (image not verified)")
        return 1
    if "OTA swap committed; new active slot=B" not in log:
        print("FAIL: post-reset OTA swap did not commit slot B")
        return 1
    if "INFO: jumping to slot B" not in log:
        print("FAIL: bootloader did not jump into the new active slot")
        return 1
    print("PASS: recovery -> OTA -> reset -> swap -> jump-to-B all confirmed")
    return 0


def main() -> int:
    if not have_qemu():
        print("SKIP: qemu-system-arm not on PATH")
        return 2
    if not APP_SIGNED.exists() or not BL_BIN.exists():
        print("SKIP: build artefacts missing; run 'make all' first")
        return 2
    build_corrupt_flash()
    return push_to_recovery()


if __name__ == "__main__":
    sys.exit(main())
