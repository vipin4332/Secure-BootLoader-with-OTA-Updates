"""Top-level test orchestrator.

Runs everything that can run in the current environment and reports a
single pass/fail summary. Tests that require a missing tool (host C
compiler, qemu-system-arm) are reported as SKIPPED unless --strict is set.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def header(label: str) -> None:
    print("\n" + "=" * 60)
    print(f"== {label}")
    print("=" * 60)


def run(cmd: list[str], cwd: Path | None = None) -> int:
    print(f"$ {' '.join(cmd)}")
    return subprocess.call(cmd, cwd=cwd)


def host_cc() -> str | None:
    for c in ("gcc", "cc", "clang"):
        if shutil.which(c):
            return c
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Fail if any integration test is skipped (QEMU, host gcc, build artifacts)",
    )
    args = parser.parse_args()

    failures = 0
    skipped = 0

    def skip(msg: str) -> None:
        nonlocal skipped, failures
        print(f"SKIP: {msg}")
        skipped += 1
        if args.strict:
            print("STRICT: skip treated as failure")
            failures += 1

    header("test_signature_python.py")
    if run([sys.executable, str(ROOT / "tests" / "test_signature_python.py")]) != 0:
        failures += 1

    header("test_delta_common.py")
    if run([sys.executable, str(ROOT / "tests" / "test_delta_common.py")]) != 0:
        failures += 1

    header("test_antidowngrade.py")
    if run([sys.executable, str(ROOT / "tests" / "test_antidowngrade.py")]) != 0:
        failures += 1

    header("test_fuzz_and_negative.py")
    if run([sys.executable, str(ROOT / "tests" / "test_fuzz_and_negative.py")]) != 0:
        failures += 1

    header("test_signature.c (host)")
    cc = host_cc()
    signed = ROOT / "build" / "app_signed.bin"
    pubkey = ROOT / "bootloader" / "include" / "public_key.h"
    if cc is None:
        skip("no host C compiler (gcc/cc/clang) on PATH")
    elif not signed.exists():
        skip(f"{signed} missing; run 'make all' first")
    elif not pubkey.exists():
        skip(f"{pubkey} missing; run 'make keys' first")
    else:
        out = ROOT / "build" / "test_signature_host"
        if sys.platform.startswith("win"):
            out = out.with_suffix(".exe")
        cmd = [
            cc,
            "-O2", "-Wall", "-Wextra",
            "-DuECC_PLATFORM=0",
            "-DuECC_SUPPORTS_secp160r1=0",
            "-DuECC_SUPPORTS_secp192r1=0",
            "-DuECC_SUPPORTS_secp224r1=0",
            "-DuECC_SUPPORTS_secp256r1=1",
            "-DuECC_SUPPORTS_secp256k1=0",
            "-DuECC_OPTIMIZATION_LEVEL=2",
            "-DuECC_SQUARE_FUNC=1",
            "-DuECC_VLI_NATIVE_LITTLE_ENDIAN=0",
            "-DuECC_ENABLE_VLI_API=0",
            "-Ibootloader/include", "-Icommon", "-Ithird_party/micro-ecc",
            "tests/test_signature.c",
            "tests/host_test_harness.c",
            "bootloader/src/crypto.c",
            "bootloader/src/sha256.c",
            "bootloader/src/crc32.c",
            "third_party/micro-ecc/uECC.c",
            "-o", str(out),
        ]
        if run(cmd, cwd=ROOT) != 0:
            failures += 1
        elif run([str(out), str(signed)], cwd=ROOT) != 0:
            failures += 1

    integration = [
        ("test_power_loss.py", ROOT / "tests" / "test_power_loss.py"),
        ("test_recovery_ota.py", ROOT / "tests" / "test_recovery_ota.py"),
        ("test_delta_ota.py", ROOT / "tests" / "test_delta_ota.py"),
        ("test_rollback.py", ROOT / "tests" / "test_rollback.py"),
    ]
    for label, script in integration:
        header(label)
        rc = run([sys.executable, str(script)])
        if rc == 2:
            skip(f"{label} skipped (missing toolchain or build)")
        elif rc != 0:
            failures += 1

    print("\n" + "=" * 60)
    if failures == 0:
        print(f"ALL TESTS PASSED ({skipped} skipped)")
        return 0
    print(f"{failures} TEST(S) FAILED ({skipped} skipped)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
