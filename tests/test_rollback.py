"""QEMU integration: boot-attempt rollback from bad Slot A to good Slot B.

Seeds BootConfig with boot_attempts at MAX_BOOT_ATTEMPTS so the first
boot verifies Slot A, immediately switches to Slot B, and runs the good app.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

QEMU_BIN = "qemu-system-arm"
FLASH_ROLLBACK = ROOT / "build" / "flash_rollback.bin"
BL_BIN = ROOT / "build" / "bootloader" / "bootloader.bin"
GOOD_SIGNED = ROOT / "build" / "app_signed.bin"
BAD_SIGNED = ROOT / "build" / "app_rollback_signed.bin"

# Matches bootloader MAX_BOOT_ATTEMPTS in main.c
MAX_BOOT_ATTEMPTS = 3


def have_qemu() -> bool:
    return shutil.which(QEMU_BIN) is not None


def skip(msg: str) -> int:
    print(f"SKIP: {msg}")
    return 2


def build_flash() -> None:
    subprocess.check_call(
        [
            sys.executable,
            str(ROOT / "tools" / "flash_layout.py"),
            "--bootloader", str(BL_BIN),
            "--slot-a", str(BAD_SIGNED),
            "--slot-b", str(GOOD_SIGNED),
            "--active", "A",
            "--boot-attempts", str(MAX_BOOT_ATTEMPTS),
            "--out", str(FLASH_ROLLBACK),
        ]
    )


def boot_once(timeout_s: float = 12.0) -> str:
    proc = subprocess.Popen(
        [
            QEMU_BIN,
            "-machine", "netduino2",
            "-nographic",
            "-kernel", str(FLASH_ROLLBACK),
            "-serial", "mon:stdio",
            "-serial", "null",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    lines: list[str] = []
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        line = proc.stdout.readline() if proc.stdout else ""
        if line:
            lines.append(line)
            if (
                "Demo Application" in line
                or "App v1.0.0 running" in line
                or "jumping to slot B" in line
            ):
                break
        if proc.poll() is not None:
            break
        time.sleep(0.02)
    proc.kill()
    proc.wait(timeout=2)
    return "".join(lines)


def main() -> int:
    if not have_qemu():
        return skip("qemu-system-arm not on PATH")
    for p in (BL_BIN, GOOD_SIGNED):
        if not p.exists():
            return skip(f"{p} missing; run 'make keys all' first")
    subprocess.check_call(["make", "app_rollback"], cwd=ROOT)
    if not BAD_SIGNED.exists():
        print("FAIL: app_rollback_signed.bin not built")
        return 1
    build_flash()

    log = boot_once()
    print("--- boot log ---")
    print(log[-1200:] if len(log) > 1200 else log)

    if "switching slot" not in log and "WARN:" not in log:
        print("FAIL: expected slot-switch log line")
        return 1
    if "Demo Application" in log or "App v1.0.0 running" in log:
        print("PASS: rolled back to good app in Slot B")
        return 0
    if "jumping to slot B" in log:
        print("PASS: bootloader selected Slot B after rollback")
        return 0
    print("FAIL: good application banner not seen")
    return 1


if __name__ == "__main__":
    sys.exit(main())
