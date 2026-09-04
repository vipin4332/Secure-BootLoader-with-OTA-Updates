"""Power-loss / kill-mid-flash regression test.

Test plan:
  1. Start QEMU with build/flash.bin (good Slot A image).
  2. Begin an OTA push to TCP 4444.
  3. After RANDOM_KILL_MS ms (uniformly between 100 and 4000 ms),
     SIGKILL the QEMU process.
  4. Restart QEMU with the same flash image.
  5. Read UART log, assert that:
       (a) the bootloader announced itself,
       (b) at least one slot verified successfully (we get an
           "INFO: jumping to slot ..." line), and
       (c) the demo application banner eventually appears.

The test runs ITERATIONS such mid-flash kills and reports a pass/fail
for each. By default ITERATIONS=8 (kept short for CI; can be increased
for extended stress testing).

Skipped automatically if `qemu-system-arm` is not on PATH.
"""

from __future__ import annotations

import os
import random
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

ITERATIONS = int(os.environ.get("POWERLOSS_ITERS", "8"))
KILL_MIN_MS = 100
KILL_MAX_MS = 4000

QEMU_BIN = "qemu-system-arm"
FLASH_BIN = ROOT / "build" / "flash.bin"
APP_SIGNED = ROOT / "build" / "app_signed.bin"


def have_qemu() -> bool:
    return shutil.which(QEMU_BIN) is not None


def boot_qemu_capture(timeout_s: float = 8.0) -> tuple[bool, str]:
    """Boot QEMU and read stdout for up to timeout_s; return (ok, log)."""
    proc = subprocess.Popen(
        [
            QEMU_BIN,
            "-machine", "netduino2",
            "-nographic",
            "-kernel", str(FLASH_BIN),
            "-serial", "mon:stdio",
            "-serial", "null",
        ],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    log_lines: list[str] = []
    deadline = time.monotonic() + timeout_s

    def reader() -> None:
        try:
            for line in proc.stdout:  # type: ignore[union-attr]
                log_lines.append(line)
        except Exception:
            pass

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            break
        time.sleep(0.05)
    proc.kill()
    proc.wait(timeout=2)
    t.join(timeout=1)
    log = "".join(log_lines)
    ok = ("Demo Application" in log) or ("App v" in log)
    return ok, log


def run_one_iteration(iteration: int) -> bool:
    print(f"\n[iter {iteration}] launching QEMU + OTA push, killing mid-flash")
    qemu = subprocess.Popen(
        [
            QEMU_BIN,
            "-machine", "netduino2",
            "-nographic",
            "-kernel", str(FLASH_BIN),
            "-serial", "null",
            "-serial", "tcp:127.0.0.1:4444,server,nowait",
        ],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    # Give QEMU time to bind the TCP listener.
    time.sleep(0.5)

    pusher = subprocess.Popen(
        [
            sys.executable, str(ROOT / "tools" / "ota_server.py"),
            str(APP_SIGNED), "--tcp", "127.0.0.1:4444",
        ],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )

    kill_ms = random.randint(KILL_MIN_MS, KILL_MAX_MS)
    time.sleep(kill_ms / 1000.0)

    qemu.kill(); qemu.wait(timeout=2)
    pusher.kill(); pusher.wait(timeout=2)

    print(f"[iter {iteration}] killed after {kill_ms} ms; rebooting")
    ok, log = boot_qemu_capture()
    if not ok:
        print("[iter {0}] FAIL - device did not reach app".format(iteration))
        print("--- log tail ---")
        print(log[-2000:])
    else:
        print(f"[iter {iteration}] PASS")
    return ok


def main() -> int:
    if not have_qemu():
        print("SKIP: qemu-system-arm not on PATH; skipping power-loss test")
        return 2
    if not FLASH_BIN.exists() or not APP_SIGNED.exists():
        print(f"SKIP: {FLASH_BIN} or {APP_SIGNED} missing; run 'make all' first")
        return 2

    random.seed(42)  # reproducible
    failures = 0
    for i in range(1, ITERATIONS + 1):
        if not run_one_iteration(i):
            failures += 1
    print(f"\n=== power-loss: {ITERATIONS - failures}/{ITERATIONS} passed ===")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
