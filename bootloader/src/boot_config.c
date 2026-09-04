/**
 * @file    boot_config.c
 * @brief   Persistent boot configuration storage at BOOT_CONFIG_ADDR.
 */

#include "config.h"
#include "crc32.h"
#include "crypto.h"
#include "firmware_format.h"
#include "flash_driver.h"
#include "memory_map.h"

#include <stddef.h>
#include <string.h>

/* Bytes [0, CRC_OFFSET) are protected by the CRC; the CRC field follows. */
#define CFG_CRC_OFFSET   20U
_Static_assert(CFG_CRC_OFFSET == offsetof(BootConfig_t, crc32),
               "CRC offset must equal the offset of the crc32 field");

void boot_config_default(BootConfig_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->slot_a_version    = 0U;
    cfg->slot_b_version    = 0U;
    cfg->active_slot       = SLOT_A;
    cfg->boot_attempts     = 0U;
    cfg->rollback_counter  = 0U;
    cfg->_reserved         = 0U;
    cfg->last_good_boot    = 0U;
    cfg->magic             = BOOT_CONFIG_MAGIC;
    boot_config_stamp_crc(cfg);
}

void boot_config_recover_from_slots(BootConfig_t *cfg)
{
    FirmwareHeader_t hdr_a;
    FirmwareHeader_t hdr_b;
    bool a_valid = false;
    bool b_valid = false;

    if (flash_read(SLOT_A_ADDR, (uint8_t *)&hdr_a, sizeof(hdr_a)) == FLASH_OK) {
        if (hdr_a.magic == FIRMWARE_MAGIC &&
            hdr_a.image_size > 0U &&
            hdr_a.image_size <= FIRMWARE_MAX_PAYLOAD_SIZE) {
            if (crypto_verify_firmware(SLOT_A_ADDR)) {
                a_valid = true;
                cfg->slot_a_version = hdr_a.version;
            }
        }
    }

    if (flash_read(SLOT_B_ADDR, (uint8_t *)&hdr_b, sizeof(hdr_b)) == FLASH_OK) {
        if (hdr_b.magic == FIRMWARE_MAGIC &&
            hdr_b.image_size > 0U &&
            hdr_b.image_size <= FIRMWARE_MAX_PAYLOAD_SIZE) {
            if (crypto_verify_firmware(SLOT_B_ADDR)) {
                b_valid = true;
                cfg->slot_b_version = hdr_b.version;
            }
        }
    }

    uint32_t floor = 0U;
    if (a_valid && cfg->slot_a_version > floor) {
        floor = cfg->slot_a_version;
        cfg->active_slot = SLOT_A;
    }
    if (b_valid && cfg->slot_b_version > floor) {
        floor = cfg->slot_b_version;
        cfg->active_slot = SLOT_B;
    }

    if (floor > 0U) {
        const uint8_t major = (uint8_t)((floor >> 16) & 0xFFU);
        cfg->rollback_counter = (major != 0U) ? major : 1U;
    }

    boot_config_stamp_crc(cfg);
}

void boot_config_stamp_crc(BootConfig_t *cfg)
{
    cfg->crc32 = crc32(cfg, CFG_CRC_OFFSET);
}

bool boot_config_validate(const BootConfig_t *cfg)
{
    if (cfg->magic != BOOT_CONFIG_MAGIC) return false;
    uint32_t calc = crc32(cfg, CFG_CRC_OFFSET);
    return (calc == cfg->crc32);
}

bool boot_config_load(BootConfig_t *out)
{
    flash_read(BOOT_CONFIG_ADDR, (uint8_t *)out, sizeof(*out));
    if (boot_config_validate(out)) return true;
    boot_config_default(out);
    boot_config_recover_from_slots(out);
    return false;
}

uint32_t boot_config_antidowngrade_floor(const BootConfig_t *cfg)
{
    uint32_t floor = (uint32_t)cfg->rollback_counter << 16;
    if (cfg->slot_a_version > floor) {
        floor = cfg->slot_a_version;
    }
    if (cfg->slot_b_version > floor) {
        floor = cfg->slot_b_version;
    }
    return floor;
}

bool boot_config_firmware_allowed(const BootConfig_t *cfg, uint32_t fw_version)
{
    return fw_version >= boot_config_antidowngrade_floor(cfg);
}

void boot_config_record_ota_to_slot(BootConfig_t *cfg,
                                    uint8_t slot,
                                    uint32_t fw_version)
{
    const uint32_t floor = boot_config_antidowngrade_floor(cfg);
    if (slot == SLOT_A) {
        cfg->slot_a_version = fw_version;
    } else {
        cfg->slot_b_version = fw_version;
    }
    if (fw_version > floor) {
        const uint8_t major = (uint8_t)((fw_version >> 16) & 0xFFU);
        cfg->rollback_counter = (major != 0U) ? major : 1U;
    }
}

bool boot_config_save(const BootConfig_t *cfg)
{
    BootConfig_t buf = *cfg;
    boot_config_stamp_crc(&buf);

    /* Step 1: erase sector */
    if (flash_erase_range(BOOT_CONFIG_ADDR, sizeof(buf)) != FLASH_OK) {
        return false;
    }

    /* Step 2: write config payload (excluding CRC) */
    if (flash_program_bytes(BOOT_CONFIG_ADDR,
                            (const uint8_t *)&buf,
                            CFG_CRC_OFFSET) != FLASH_OK) {
        return false;
    }

    /* Step 3: write CRC last */
    if (flash_program_word(BOOT_CONFIG_ADDR + CFG_CRC_OFFSET,
                           buf.crc32) != FLASH_OK) {
        return false;
    }

    /* Step 4: readback verify */
    BootConfig_t readback;
    flash_read(BOOT_CONFIG_ADDR, (uint8_t *)&readback, sizeof(readback));
    if (memcmp(&readback, &buf, sizeof(buf)) != 0) {
        return false;
    }
    return true;
}
