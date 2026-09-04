/**
 * @file    config.h
 * @brief   Persistent boot configuration (BootConfig_t) read/write API.
 *
 * Layout note: the fields are exactly those listed in the project spec, with
 * a single 1-byte `_reserved` padding inserted between the three uint8_t
 * counters and the next uint32_t so that:
 *   - the whole struct is 24 bytes,
 *   - `crc32` lands at word-aligned offset 20, and
 *   - the CRC field can be programmed last as one atomic word write.
 *
 * The CRC covers offsets [0, 20) - everything except the CRC field itself.
 * The `magic` field sits at offset 16, immediately before the CRC, so the
 * common torn-write failure mode (CRC corrupted, body intact) is detected
 * by the CRC check; any byte-level corruption of `magic` is detected
 * separately. A pristine, never-written sector reads as 0xFFFFFFFF for the
 * magic, which fails the magic check and triggers default initialisation.
 */
#ifndef BOOTLOADER_CONFIG_H
#define BOOTLOADER_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "memory_map.h"

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t slot_a_version;    /**< offset  0: semver of Slot A image. */
    uint32_t slot_b_version;    /**< offset  4: semver of Slot B image. */
    uint8_t  active_slot;       /**< offset  8: 'A' or 'B'. */
    uint8_t  boot_attempts;     /**< offset  9: incremented per boot. */
    uint8_t  rollback_counter;  /**< offset 10: anti-downgrade counter. */
    uint8_t  _reserved;         /**< offset 11: padding to next u32. */
    uint32_t last_good_boot;    /**< offset 12: timestamp of last confirm. */
    uint32_t magic;             /**< offset 16: BOOT_CONFIG_MAGIC. */
    uint32_t crc32;             /**< offset 20: CRC32 of bytes [0,20). */
} BootConfig_t;

_Static_assert(sizeof(BootConfig_t) == 24,
               "BootConfig_t layout drift; fix offsets / padding");

/**
 * @brief Load and validate the boot config from flash.
 *
 * @param out  Receives the loaded record on success.
 * @return     true if the on-flash config is valid (magic + CRC ok).
 *             If false, @p out is filled with defaults.
 */
bool boot_config_load(BootConfig_t *out);

/**
 * @brief Reset @p cfg to factory defaults (Slot A active, attempts=0).
 *
 * Does not persist; call boot_config_save() to write to flash.
 */
void boot_config_default(BootConfig_t *cfg);

/**
 * @brief Validate a record (magic match + CRC over bytes [0, 20)).
 */
bool boot_config_validate(const BootConfig_t *cfg);

/**
 * @brief Compute and stamp the CRC32 over @p cfg's body, in place.
 *
 * Sets cfg->crc32 = crc32(bytes [0,20)). Useful before persisting.
 */
void boot_config_stamp_crc(BootConfig_t *cfg);

/**
 * @brief Persist @p cfg to flash.
 *
 * Strategy:
 *   1. Erase the boot-config sector (idempotent, fills with 0xFF).
 *   2. Write the body bytes [0, 20) (everything but CRC).
 *   3. Write the CRC field LAST as a single word so a torn write at any
 *      point before this leaves the on-flash record CRC-invalid, which we
 *      reject on the next boot.
 *
 * @return true on full success, false on any flash error or readback mismatch.
 */
bool boot_config_save(const BootConfig_t *cfg);

/**
 * @brief Minimum firmware version allowed (anti-downgrade floor).
 *
 * Uses slot version records and @p rollback_counter (major semver byte).
 */
uint32_t boot_config_antidowngrade_floor(const BootConfig_t *cfg);

/** @return true if @p fw_version may be installed. */
bool boot_config_firmware_allowed(const BootConfig_t *cfg, uint32_t fw_version);

/** Record a verified image in @p slot and raise the anti-downgrade floor. */
void boot_config_record_ota_to_slot(BootConfig_t *cfg,
                                    uint8_t slot,
                                    uint32_t fw_version);

/**
 * @brief Reconstruct slot versions and anti-downgrade floor from on-flash slot headers.
 *
 * Called when the stored BootConfig is invalid, uninitialized, or corrupted
 * (e.g. after a power loss during sector erase/reprogram) to prevent anti-downgrade
 * floor reset to zero.
 */
void boot_config_recover_from_slots(BootConfig_t *cfg);

#endif /* BOOTLOADER_CONFIG_H */
