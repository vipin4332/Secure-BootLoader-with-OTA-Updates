/**
 * @file    shared_memory.h
 * @brief   Persistent-across-reset SRAM block used to communicate boot status
 *          between the bootloader and the application.
 *
 * The block lives in the very last 16 bytes of SRAM (0x2001FFF0), placed via
 * a NOLOAD section in both the bootloader and application linker scripts so
 * neither startup .bss zeroer touches it. After a soft reset the SRAM
 * contents survive, which is exactly what we exploit here.
 *
 * The bootloader writes nothing; the application calls
 * `bootloader_confirm_boot()` (see application/include/bootloader_api.h)
 * within ~5 s of starting to indicate "I am alive and well". On the next
 * reset the bootloader checks the magic + boot_confirmed fields:
 *   - magic mismatch  -> treat as cold-boot (RAM was just powered up)
 *   - boot_confirmed  -> previous boot was a success, clear boot_attempts
 *   - else            -> previous boot crashed, leave attempts alone
 */
#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdint.h>
#include "memory_map.h"

typedef struct __attribute__((packed)) {
    uint32_t magic;          /**< SHARED_BLOCK_MAGIC if valid. */
    uint32_t boot_confirmed; /**< 1 = app called confirm; 0 = not yet. */
    uint32_t ota_pending;    /**< 1 = OTA wrote a new image, swap on next boot. */
    uint32_t reserved;       /**< Reserved for future flags. */
} SharedBootBlock_t;

_Static_assert(sizeof(SharedBootBlock_t) == SHARED_BLOCK_SIZE,
               "SharedBootBlock_t must be exactly 16 B");

#define SHARED_BOOT_BLOCK \
    (*(volatile SharedBootBlock_t *)SHARED_BLOCK_ADDR)

#endif /* SHARED_MEMORY_H */
