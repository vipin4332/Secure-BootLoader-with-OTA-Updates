/**
 * @file    iwdg.c
 * @brief   Independent watchdog (IWDG) at ~10 s timeout.
 *
 * Configuration on STM32F4:
 *   LSI clock = 32 kHz, prescaler = 256 -> tick = 8 ms.
 *   Reload value 1250 -> timeout = 1250 * 8 ms = 10 s.
 *
 * On QEMU netduino2 the IWDG is not modelled; the register writes are
 * silently accepted. This is fine for development because we never want
 * QEMU runs to be killed by the watchdog. For real-hardware testing the
 * Renode `stm32f4_discovery` platform models IWDG correctly.
 */

#include "iwdg.h"

#include <stdint.h>

#define IWDG_BASE    0x40003000UL
#define IWDG_KR      (*(volatile uint32_t *)(IWDG_BASE + 0x00U))
#define IWDG_PR      (*(volatile uint32_t *)(IWDG_BASE + 0x04U))
#define IWDG_RLR     (*(volatile uint32_t *)(IWDG_BASE + 0x08U))
#define IWDG_SR      (*(volatile uint32_t *)(IWDG_BASE + 0x0CU))

#define IWDG_KR_KEY_RELOAD   0xAAAAU
#define IWDG_KR_KEY_ENABLE   0xCCCCU
#define IWDG_KR_KEY_ACCESS   0x5555U

#define IWDG_PR_DIV256   0x6U
#define IWDG_RLR_10S     1250U

void iwdg_init(void)
{
    /* Unlock prescaler/reload registers. */
    IWDG_KR = IWDG_KR_KEY_ACCESS;
    /* Wait for any prior PVU/RVU to clear (IWDG_SR != 0). On QEMU SR is 0
     * already; we still poll a bounded number of times for HW correctness. */
    for (uint32_t i = 0; (IWDG_SR != 0U) && i < 1000U; ++i) { /* spin */ }
    IWDG_PR  = IWDG_PR_DIV256;
    IWDG_RLR = IWDG_RLR_10S;
    /* Reload now and start. */
    IWDG_KR = IWDG_KR_KEY_RELOAD;
    IWDG_KR = IWDG_KR_KEY_ENABLE;
}

void iwdg_kick(void)
{
    IWDG_KR = IWDG_KR_KEY_RELOAD;
}
