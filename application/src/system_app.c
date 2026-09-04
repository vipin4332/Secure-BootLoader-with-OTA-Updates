/**
 * @file    system_app.c
 * @brief   Pre-main hardware bring-up for the demo application.
 *
 * Mostly a no-op on QEMU netduino2: VTOR is already set by the bootloader
 * before it jumps into us, and the model runs at the modeled clock
 * regardless of RCC writes. We re-assert VTOR here defensively so the app
 * is still correct if it's ever loaded by some external tool that didn't
 * touch VTOR.
 */

#include "memory_map.h"

#include <stdint.h>

#define SCB_VTOR  (*(volatile uint32_t *)0xE000ED08UL)

/* The application image lives in either Slot A or Slot B; either way, our
 * vector table is at slot+0x200, but we don't know our own load address
 * at compile time. We use the linker-defined symbol for the start of the
 * vector table (which equals the load address of .isr_vector) by deriving
 * it from the SCB_VTOR value the bootloader already wrote. This is safe:
 * any sane bootloader (including ours) sets VTOR correctly before
 * branching, so reading it back gives us the right address. */

void SystemInit_App(void)
{
    /* Defensive: ensure VTOR points where the app's vector table actually
     * is. We read the current value (which the bootloader set) and write
     * it back; if VTOR is unset (0), default to Slot A. */
    uint32_t vtor = SCB_VTOR;
    if (vtor == 0U) {
        SCB_VTOR = SLOT_A_ADDR + 0x200UL;
    }
}
