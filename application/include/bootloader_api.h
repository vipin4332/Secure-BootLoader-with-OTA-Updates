/**
 * @file    bootloader_api.h
 * @brief   Application-side interface to the bootloader.
 *
 * Single function: bootloader_confirm_boot(). Call it once your app has
 * successfully reached its main idle loop (typically after a few seconds
 * of normal operation). If you don't, the bootloader will treat the
 * boot as failed on the next reset and start counting up boot_attempts;
 * after 3 consecutive un-confirmed boots, it will switch to the other
 * slot and try again.
 */
#ifndef BOOTLOADER_API_H
#define BOOTLOADER_API_H

#include <stdint.h>
#include "shared_memory.h"

/**
 * @brief Tell the bootloader the application is alive and well.
 *
 * Writes the boot-confirmation flag into the SharedBootBlock at the top
 * of SRAM; the bootloader reads (and clears) this flag on the next reset.
 *
 * Idempotent. Safe to call from any context.
 */
static inline void bootloader_confirm_boot(void)
{
    SHARED_BOOT_BLOCK.magic          = SHARED_BLOCK_MAGIC;
    SHARED_BOOT_BLOCK.boot_confirmed = 1U;
}

/**
 * @brief Programmatically request an OTA swap on next reboot.
 *
 * This is normally done by the bootloader's OTA client itself, but is
 * exposed so an application can trigger a swap explicitly (e.g. after
 * downloading an image via its own protocol).
 */
static inline void bootloader_request_ota_swap(void)
{
    SHARED_BOOT_BLOCK.magic       = SHARED_BLOCK_MAGIC;
    SHARED_BOOT_BLOCK.ota_pending = 1U;
}

#endif /* BOOTLOADER_API_H */
