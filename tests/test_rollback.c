/*
 * test_rollback.c - "Bad app" used to exercise the bootloader's rollback
 * path. This compiles as a normal application target binary; just sub it
 * in for application/src/app_main.c when building the rollback test
 * image.
 *
 * Behavior:
 *   - Initialise UART so the test orchestrator can see we tried to start.
 *   - Print a banner identifying ourselves as the bad app.
 *   - DO NOT call bootloader_confirm_boot().
 *   - Trigger a hard fault by dereferencing 0xDEAD0000 - this guarantees
 *     the bootloader will not see the confirmation flag on the next boot.
 *
 * The test orchestrator (tests/test_rollback.py) launches
 * QEMU with this image in Slot A; after 3 boot cycles the bootloader
 * should switch to Slot B and run the good demo app from v1.0.0.
 */

#include "iwdg.h"
#include "uart.h"

#include <stdint.h>

int app_main(void)
{
    iwdg_init();
    uart_init();
    uart_log_puts("\r\nBAD APP: deliberately not confirming boot\r\n");
    uart_log_puts("BAD APP: triggering hard fault now\r\n");

    /* Spin a few moments so the host sees the message. */
    for (volatile uint32_t i = 0; i < 200000U; ++i) { iwdg_kick(); }

    /* Force a hard fault: write to an unmapped address. */
    *(volatile uint32_t *)0xDEAD0000U = 0xCAFEBABEU;

    /* If for some reason that didn't fault (shouldn't happen), spin so
     * the watchdog kills us. */
    while (1) { /* no iwdg_kick on purpose */ }
}
