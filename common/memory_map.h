/**
 * @file    memory_map.h
 * @brief   Authoritative flash and RAM memory map for the bootloader and
 *          application.
 *
 * This is the single source of truth for partition addresses. Any change here
 * must be mirrored in:
 *   - bootloader/linker_bootloader.ld
 *   - application/linker_app.ld
 *   - tools/flash_layout.py
 *   - docs/architecture.md
 *
 * Compile-time _Static_asserts at the bottom of this file enforce the
 * ARM Cortex-M4 VTOR alignment requirement (512 bytes) for the application
 * vector table inside each slot.
 */
#ifndef MEMORY_MAP_H
#define MEMORY_MAP_H

#include <stdint.h>

/* ===== Flash partitions ============================================== */

#define FLASH_BASE_ADDR        0x08000000UL
#define FLASH_TOTAL_SIZE       (1024UL * 1024UL)         /* 1 MB */

#define BOOTLOADER_ADDR        0x08000000UL
#define BOOTLOADER_SIZE        (64UL * 1024UL)           /* 64 KB */

#define BOOT_CONFIG_ADDR       0x08010000UL
#define BOOT_CONFIG_SIZE       (4UL * 1024UL)            /* 4 KB logical */

#define SLOT_A_ADDR            0x08011000UL
#define SLOT_B_ADDR            0x08084000UL
#define SLOT_SIZE              (460UL * 1024UL)          /* 460 KB each */

#define FLASH_USED_END         (SLOT_B_ADDR + SLOT_SIZE) /* 0x080F7000 */

/* ===== SRAM ========================================================== */

/*
 * The STM32F407 has 192 KB of SRAM but it is split into two non-contiguous
 * regions:
 *   - SRAM1 + SRAM2: 128 KB contiguous at 0x20000000 (general-purpose).
 *   - CCM:            64 KB at 0x10000000 (data only, can't run code).
 * The bootloader, app, and SharedBootBlock all live in the contiguous
 * 128 KB region. The SharedBootBlock is at the very top of that region.
 */
#define SRAM_BASE_ADDR         0x20000000UL
#define SRAM_TOTAL_SIZE        (128UL * 1024UL)          /* SRAM1+SRAM2 contiguous */
#define SRAM_END_ADDR          (SRAM_BASE_ADDR + SRAM_TOTAL_SIZE)

/*
 * SharedBootBlock lives in the very last 16 bytes of SRAM. It must not be
 * cleared by the application's startup .bss zeroer, hence the NOLOAD section
 * in both linker scripts.
 */
#define SHARED_BLOCK_ADDR      0x2001FFF0UL
#define SHARED_BLOCK_SIZE      16UL

/* ===== Slot identifiers ============================================== */

#define SLOT_A                 'A'
#define SLOT_B                 'B'

/* ===== Magic constants =============================================== */

#define FIRMWARE_MAGIC         0xDEADBEEFUL  /* in FirmwareHeader_t.magic */
#define BOOT_CONFIG_MAGIC      0xB00710ADUL  /* in BootConfig_t.magic */
#define SHARED_BLOCK_MAGIC     0x5EAFB001UL  /* in SharedBootBlock.magic */

/* ===== Helper: slot address from id ================================== */

static inline uint32_t memory_map_slot_addr(uint8_t slot)
{
    return (slot == SLOT_B) ? SLOT_B_ADDR : SLOT_A_ADDR;
}

static inline uint8_t memory_map_other_slot(uint8_t slot)
{
    return (slot == SLOT_A) ? SLOT_B : SLOT_A;
}

/* ===== Compile-time alignment proofs ================================== */

/*
 * VTOR alignment requirement on STM32F4 (98+ vectors -> 512-byte alignment).
 * The firmware header is exactly 512 bytes (_Static_asserted in
 * firmware_format.h), so the application vector table sits at slot+0x200,
 * which must itself be a multiple of 0x200.
 */
#define _SLOT_A_APP_ENTRY  (SLOT_A_ADDR + 0x200UL)
#define _SLOT_B_APP_ENTRY  (SLOT_B_ADDR + 0x200UL)

_Static_assert((_SLOT_A_APP_ENTRY & 0x1FFUL) == 0,
               "Slot A app entry must be 512-byte aligned for VTOR");
_Static_assert((_SLOT_B_APP_ENTRY & 0x1FFUL) == 0,
               "Slot B app entry must be 512-byte aligned for VTOR");
_Static_assert(BOOTLOADER_ADDR + BOOTLOADER_SIZE <= BOOT_CONFIG_ADDR,
               "Bootloader overlaps boot config");
_Static_assert(BOOT_CONFIG_ADDR + BOOT_CONFIG_SIZE <= SLOT_A_ADDR,
               "Boot config overlaps slot A");
_Static_assert(SLOT_A_ADDR + SLOT_SIZE <= SLOT_B_ADDR,
               "Slot A overlaps slot B");
_Static_assert(SLOT_B_ADDR + SLOT_SIZE <= FLASH_BASE_ADDR + FLASH_TOTAL_SIZE,
               "Slot B exceeds flash size");
_Static_assert(SHARED_BLOCK_ADDR + SHARED_BLOCK_SIZE == SRAM_END_ADDR,
               "SharedBootBlock must occupy the top 16 B of SRAM");

#endif /* MEMORY_MAP_H */
