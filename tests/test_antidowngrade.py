#!/usr/bin/env python3
"""Regression test for anti-downgrade floor enforcement and torn-write recovery.

Verifies:
  1. Monotonic floor computation from slot versions and rollback counter.
  2. Rejection of firmware versions lower than the anti-downgrade floor.
  3. Acceptance of equal or higher firmware versions.
  4. Monotonic counter increment on major version upgrades.
  5. Recovery of anti-downgrade floor when BootConfig is wiped (simulating
     interrupted power during sector erase/reprogram) while valid signed
     firmware is present on flash.
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

# Mirror common/memory_map.h and bootloader/include/config.h
BOOT_CONFIG_MAGIC = 0xB00710AD
FIRMWARE_MAGIC = 0xDEADBEEF
SLOT_A_ID = ord("A")
SLOT_B_ID = ord("B")


class BootConfig:
    def __init__(
        self,
        slot_a_version: int = 0,
        slot_b_version: int = 0,
        active_slot: int = SLOT_A_ID,
        boot_attempts: int = 0,
        rollback_counter: int = 0,
        reserved: int = 0,
        last_good_boot: int = 0,
        magic: int = BOOT_CONFIG_MAGIC,
        crc32: int | None = None,
    ):
        self.slot_a_version = slot_a_version
        self.slot_b_version = slot_b_version
        self.active_slot = active_slot
        self.boot_attempts = boot_attempts
        self.rollback_counter = rollback_counter
        self._reserved = reserved
        self.last_good_boot = last_good_boot
        self.magic = magic
        if crc32 is None:
            self.stamp_crc()
        else:
            self.crc32 = crc32

    def pack_body(self) -> bytes:
        return struct.pack(
            "<II 4B II",
            self.slot_a_version,
            self.slot_b_version,
            self.active_slot,
            self.boot_attempts,
            self.rollback_counter,
            self._reserved,
            self.last_good_boot,
            self.magic,
        )

    def stamp_crc(self) -> None:
        self.crc32 = zlib.crc32(self.pack_body()) & 0xFFFFFFFF

    def pack(self) -> bytes:
        self.stamp_crc()
        return self.pack_body() + struct.pack("<I", self.crc32)

    @classmethod
    def unpack(cls, data: bytes) -> "BootConfig":
        if len(data) < 24:
            raise ValueError(f"Data too short: {len(data)} < 24")
        body = data[:20]
        crc = struct.unpack("<I", data[20:24])[0]
        (
            va,
            vb,
            act,
            att,
            rb,
            res,
            lgb,
            magic,
        ) = struct.unpack("<II 4B II", body)
        return cls(va, vb, act, att, rb, res, lgb, magic, crc)

    def validate(self) -> bool:
        if self.magic != BOOT_CONFIG_MAGIC:
            return False
        calc = zlib.crc32(self.pack_body()) & 0xFFFFFFFF
        return calc == self.crc32

    def antidowngrade_floor(self) -> int:
        floor = self.rollback_counter << 16
        if self.slot_a_version > floor:
            floor = self.slot_a_version
        if self.slot_b_version > floor:
            floor = self.slot_b_version
        return floor

    def firmware_allowed(self, fw_version: int) -> bool:
        return fw_version >= self.antidowngrade_floor()

    def record_ota_to_slot(self, slot: int, fw_version: int) -> None:
        floor = self.antidowngrade_floor()
        if slot == SLOT_A_ID:
            self.slot_a_version = fw_version
        else:
            self.slot_b_version = fw_version
        if fw_version > floor:
            major = (fw_version >> 16) & 0xFF
            self.rollback_counter = major if major != 0 else 1


def test_basic_anti_downgrade() -> None:
    cfg = BootConfig()
    assert cfg.validate(), "Default config failed validation"
    assert cfg.antidowngrade_floor() == 0, "Default floor should be 0"

    # Install v1.2.0 (0x00010200) into Slot A
    v1_2_0 = 0x00010200
    cfg.record_ota_to_slot(SLOT_A_ID, v1_2_0)
    assert cfg.rollback_counter == 1, f"Expected rollback_counter=1, got {cfg.rollback_counter}"
    assert cfg.antidowngrade_floor() == v1_2_0, f"Expected floor={hex(v1_2_0)}, got {hex(cfg.antidowngrade_floor())}"

    # Downgrade attempt to v1.1.0 (0x00010100) must be rejected
    v1_1_0 = 0x00010100
    assert not cfg.firmware_allowed(v1_1_0), "Failed: v1.1.0 should be rejected when floor is v1.2.0"

    # Same version v1.2.0 must be allowed (re-flash / slot swap)
    assert cfg.firmware_allowed(v1_2_0), "Failed: v1.2.0 should be allowed when floor is v1.2.0"

    # Patch upgrade to v1.2.1 (0x00010201) must be allowed
    v1_2_1 = 0x00010201
    assert cfg.firmware_allowed(v1_2_1), "Failed: v1.2.1 should be allowed when floor is v1.2.0"

    # Major upgrade to v2.0.0 (0x00020000)
    v2_0_0 = 0x00020000
    cfg.record_ota_to_slot(SLOT_B_ID, v2_0_0)
    assert cfg.rollback_counter == 2, f"Expected rollback_counter=2, got {cfg.rollback_counter}"
    assert cfg.antidowngrade_floor() == v2_0_0

    # Old branch v1.9.9 (0x00010909) must now be rejected
    v1_9_9 = 0x00010909
    assert not cfg.firmware_allowed(v1_9_9), "Failed: v1.9.9 must be rejected when floor is v2.0.0"
    print("  anti-downgrade monotonic rules: PASS")


def test_torn_write_recovery_preserves_floor() -> None:
    """Simulate power loss during BootConfig erase/write and verify that
    reconstruction from flash slot headers recovers the anti-downgrade floor."""
    # Simulate valid Slot A containing v2.3.0
    v2_3_0 = 0x00020300
    # BootConfig corrupted by power loss (all 0xFF from erase)
    erased_cfg_bytes = b"\xff" * 24

    # Unpack simulated torn config
    raw_cfg = BootConfig.unpack(erased_cfg_bytes)
    assert not raw_cfg.validate(), "Erased config should fail validation"

    # Fallback to default
    recovered = BootConfig()
    # Now simulate boot_config_recover_from_slots() with Slot A holding v2.3.0
    slot_a_installed_version = v2_3_0
    slot_b_installed_version = 0

    if slot_a_installed_version > 0:
        recovered.slot_a_version = slot_a_installed_version
    if slot_b_installed_version > 0:
        recovered.slot_b_version = slot_b_installed_version

    highest = max(recovered.slot_a_version, recovered.slot_b_version)
    if highest > 0:
        major = (highest >> 16) & 0xFF
        recovered.rollback_counter = major if major != 0 else 1
    recovered.stamp_crc()

    assert recovered.validate(), "Recovered config should be valid"
    assert recovered.antidowngrade_floor() == v2_3_0, (
        f"Floor after power-cut recovery must be {hex(v2_3_0)}, got {hex(recovered.antidowngrade_floor())}"
    )

    # Replay attack attempt with v1.5.0 after simulated power loss
    v1_5_0 = 0x00010500
    assert not recovered.firmware_allowed(v1_5_0), (
        "Replay attack with v1.5.0 succeeded after power loss; anti-downgrade floor was wiped!"
    )
    print("  torn-write floor recovery & replay protection: PASS")


def main() -> int:
    print("Running anti-downgrade & torn-write recovery tests...")
    test_basic_anti_downgrade()
    test_torn_write_recovery_preserves_floor()
    print("All anti-downgrade tests passed.\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
