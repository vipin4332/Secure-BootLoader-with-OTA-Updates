/*
 * Tiny host-build shim that lets us link the bootloader's crypto / sha256
 * / crc32 / uECC sources into a host executable without the rest of the
 * embedded HAL. Anything that the firmware would have provided (iwdg,
 * uart, flash) is stubbed here.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "flash_driver.h"

void iwdg_init(void) { /* host stub */ }
void iwdg_kick(void) { /* host stub */ }

/* uECC RNG is not used for verify; provide a deterministic stub anyway so
 * tests that exercise sign-side code on host don't try to read /dev/urandom
 * unexpectedly. */
int  uECC_random_func_stub(uint8_t *dest, unsigned size)
{
    for (unsigned i = 0; i < size; ++i) dest[i] = (uint8_t)(i ^ 0xA5);
    return 1;
}

flash_status_t flash_read(uint32_t addr, uint8_t *buf, uint32_t size)
{
    if ((buf == NULL) || (size == 0U)) {
        return FLASH_ERR_RANGE;
    }
    memcpy(buf, (const void *)(uintptr_t)addr, size);
    return FLASH_OK;
}
