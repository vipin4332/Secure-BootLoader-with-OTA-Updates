/**
 * @file    app_main.c
 * @brief   Demo application v1.0.0 (default).
 *
 * Behavior:
 *   1. Initialise the UART (same USART1 the bootloader uses for logs).
 *   2. Print a banner with the build-time APP_VERSION_STR.
 *   3. Loop, printing "App vX.Y.Z tick N" every ~1 s.
 *   4. After 5 ticks, call bootloader_confirm_boot() so the bootloader
 *      knows the next reset is not a crash recovery.
 *   5. Keep the IWDG fed by calling iwdg_kick() each iteration.
 *
 * The version string is supplied from the build system via
 * -DAPP_VERSION_STR=\"x.y.z\". The default below keeps the app buildable
 * standalone (e.g. by an IDE) without that flag.
 */

#include "bootloader_api.h"
#include "iwdg.h"
#include "uart.h"

#include <stdint.h>

#ifndef APP_VERSION_STR
#define APP_VERSION_STR "1.0.0"
#endif

/* Coarse delay; matches the POLLS_PER_MS calibration in uart.c. */
static void delay_ms(uint32_t ms)
{
    volatile uint32_t i = ms * 1600U;
    while (i--) {
        if ((i & 0xFFFFU) == 0U) iwdg_kick();
    }
}

int app_main(void)
{
    iwdg_init();
    uart_init();

    uart_log_puts("\r\n");
    uart_log_puts("==============================================\r\n");
    uart_log_puts(" Demo Application v" APP_VERSION_STR " running\r\n");
    uart_log_puts("==============================================\r\n");

    uint32_t tick = 0;
    while (1) {
        uart_log_printf("App v" APP_VERSION_STR " tick %u\r\n",
                        (unsigned)tick);

        if (tick == 5U) {
            bootloader_confirm_boot();
            uart_log_puts("App: boot confirmed to bootloader\r\n");
        }

        delay_ms(1000U);
        iwdg_kick();
        tick++;
    }
    /* unreachable */
}
