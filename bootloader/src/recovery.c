/**
 * @file    recovery.c
 * @brief   UART-driven recovery menu (entered when both slots fail verify).
 *
 * Commands (single ASCII letter on the OTA UART, USART2):
 *   'U' / 'u'  Upload a signed image via the OTA framed protocol.
 *              On success, the receiver sets ota_pending and reboots.
 *   'V' / 'v'  Verify both slots and report pass/fail individually.
 *   'R' / 'r'  Force a system reset.
 *   anything else: ignored.
 *
 * The loop runs forever, kicking the IWDG continuously; the only way out
 * is a successful Upload (which ends in NVIC_SystemReset) or an explicit
 * Reboot command.
 */

#include "bootloader.h"
#include "crypto.h"
#include "iwdg.h"
#include "memory_map.h"
#include "ota_client.h"
#include "uart.h"

#include <stdint.h>

__attribute__((noreturn)) void bootloader_system_reset(void);

static void print_banner(void)
{
    uart_log_puts(
        "\r\n"
        "*****************************************************\r\n"
        "*  RECOVERY MODE - both slots failed verification    *\r\n"
        "*  Send a command on USART2 (TCP 4444 in QEMU):      *\r\n"
        "*    U = Upload signed image (OTA frame protocol)    *\r\n"
        "*    V = Verify both slots and report                *\r\n"
        "*    R = Reboot                                      *\r\n"
        "*****************************************************\r\n"
    );
}

static void verify_and_report(void)
{
    uart_log_printf("VERIFY: slot A @ 0x%08x ... ", (unsigned)SLOT_A_ADDR);
    bool a = crypto_verify_firmware(SLOT_A_ADDR);
    uart_log_puts(a ? "OK\r\n" : "FAIL\r\n");

    uart_log_printf("VERIFY: slot B @ 0x%08x ... ", (unsigned)SLOT_B_ADDR);
    bool b = crypto_verify_firmware(SLOT_B_ADDR);
    uart_log_puts(b ? "OK\r\n" : "FAIL\r\n");
}

__attribute__((noreturn))
void recovery_mode_run(void)
{
    print_banner();
    while (true) {
        iwdg_kick();
        uint8_t cmd;
        if (!uart_ota_getc(&cmd, 1000U)) continue; /* poll w/ timeout */

        switch (cmd) {
        case 'U': case 'u':
            uart_log_puts("RECOVERY: starting OTA receive\r\n");
            (void)ota_client_receive();
            /* If receive succeeded it reset the system. If it returned
             * (abort or fatal), redraw the banner. */
            print_banner();
            break;
        case 'V': case 'v':
            verify_and_report();
            print_banner();
            break;
        case 'R': case 'r':
            uart_log_puts("RECOVERY: rebooting on user request\r\n");
            bootloader_system_reset();  /* never returns */
            break;
        default:
            /* Ignore CR / LF / stray bytes silently. */
            break;
        }
    }
}
