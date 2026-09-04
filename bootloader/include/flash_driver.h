/**
 * @file    flash_driver.h
 * @brief   STM32F4 internal flash driver: erase, program, read.
 *
 * Two compile-time backends are provided:
 *
 * 1. **Real hardware path** (default): drives the STM32F4 FLASH controller
 *    registers (FLASH_KEYR, FLASH_CR, FLASH_SR, FLASH_OPTKEYR) directly.
 *    Suitable for Renode (`stm32f4_discovery`) and physical silicon.
 *
 * 2. **QEMU_FLASH_SIM path** (when `-DQEMU_FLASH_SIM=1`): bypasses the FLASH
 *    controller and performs the equivalent operation as a plain memcpy /
 *    memset against the flash address range. This is used in QEMU's
 *    `-machine netduino2` which does not faithfully model the F2/F4 FLASH
 *    peripheral. Erase fills with 0xFF, programs are direct stores. Sector
 *    boundary semantics are still enforced so logic that relies on them
 *    (e.g. erase-before-program) behaves identically in both backends.
 *
 * All long operations call `iwdg_kick()` periodically.
 */
#ifndef FLASH_DRIVER_H
#define FLASH_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Number of flash sectors on STM32F407 (1 MB). */
#define FLASH_NUM_SECTORS  12U

typedef enum {
    FLASH_OK = 0,
    FLASH_ERR_BUSY,
    FLASH_ERR_VERIFY,
    FLASH_ERR_RANGE,
    FLASH_ERR_LOCKED,
    FLASH_ERR_PROG,
    FLASH_ERR_WRITE_PROTECTED,
} flash_status_t;

/**
 * @brief Initialise the flash driver.
 *
 * On the HW path this just clears any pending error flags. On the QEMU
 * path it's a no-op. Always returns FLASH_OK.
 */
flash_status_t flash_init(void);

/**
 * @brief Unlock the flash for writes/erases.
 *
 * @return FLASH_OK on success, FLASH_ERR_LOCKED on failure.
 */
flash_status_t flash_unlock(void);

/**
 * @brief Re-lock the flash after writes are complete.
 */
flash_status_t flash_lock(void);

/**
 * @brief Erase a single flash sector by address.
 *
 * Resolves @p sector_start_addr to a sector index internally. Address must
 * be sector-aligned.
 *
 * @param sector_start_addr  Address of the first byte of the sector.
 * @return FLASH_OK on success.
 */
flash_status_t flash_erase_sector(uint32_t sector_start_addr);

/**
 * @brief Erase enough sectors to fully cover the [addr, addr+size) range.
 *
 * If the range only partially overlaps a sector, the WHOLE sector is
 * erased (this is fundamental to NOR flash and worth being explicit about).
 * Caller is responsible for any backup of data they wanted to preserve in
 * partially-overlapping sectors.
 */
flash_status_t flash_erase_range(uint32_t addr, uint32_t size);

/**
 * @brief Program a 32-bit word at @p addr. Address must be 4-byte aligned.
 *
 * Performs a readback verify after the write.
 */
flash_status_t flash_program_word(uint32_t addr, uint32_t value);

/**
 * @brief Program a contiguous range of bytes.
 *
 * Internally programs in 32-bit chunks where possible, byte-by-byte for
 * the trailing remainder. Each chunk is followed by a readback verify.
 * Calls iwdg_kick() every 256 bytes.
 *
 * @param addr  Destination flash address.
 * @param data  Source buffer.
 * @param size  Number of bytes to program.
 */
flash_status_t flash_program_bytes(uint32_t addr,
                                   const uint8_t *data,
                                   uint32_t size);

/**
 * @brief Read @p size bytes from flash at @p addr into @p buf.
 *
 * On both backends this is a memcpy because flash is memory-mapped on
 * Cortex-M and STM32F4. Provided for API symmetry.
 */
flash_status_t flash_read(uint32_t addr, uint8_t *buf, uint32_t size);

/**
 * @brief Return the start address of the sector containing @p addr.
 */
uint32_t flash_sector_start(uint32_t addr);

/**
 * @brief Return the size in bytes of the sector containing @p addr.
 */
uint32_t flash_sector_size(uint32_t addr);

/**
 * @brief Resolve a flash address to a CPU-readable pointer.
 *
 * On real hardware this is a no-op cast: STM32F4 flash is memory-mapped
 * and a load from `addr` always reads the live flash content. On the
 * QEMU_FLASH_SIM backend, however, the netduino2's flash region is a
 * read-only ROM in QEMU; writes from the CPU bus are silently dropped.
 * To make the bootloader logic (config save, OTA program-and-verify,
 * post-OTA re-verify across a soft reset, rollback) testable on QEMU, the
 * QEMU backend keeps a SRAM-backed shadow of the writable flash regions
 * (config + slot A + slot B) and serves both reads and writes from it.
 *
 * Code that needs to dereference a pointer into a slot or config region
 * (e.g. `(const FirmwareHeader_t *)slot_addr`) MUST go through this helper
 * so it picks up the shadow on QEMU.
 *
 * @return  A pointer aliasing the same logical bytes as @p addr, but
 *          living in shadow SRAM on QEMU when @p addr falls in a
 *          shadowed region. Always usable for reads; writes through this
 *          pointer are NOT supported - use flash_program_*().
 */
void *flash_get_ptr(uint32_t addr);

/**
 * @brief Span used for OTA/delta layout in the inactive slot.
 *
 * On QEMU_FLASH_SIM the SRAM shadow covers only the first 32 KB of each
 * slot; tail-staged patches must be placed within that span. On hardware
 * the full SLOT_SIZE is available.
 */
uint32_t flash_ota_slot_span(void);

#endif /* FLASH_DRIVER_H */
