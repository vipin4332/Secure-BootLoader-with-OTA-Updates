/**
 * @file    bootloader.h
 * @brief   Public bootloader entrypoints used across the bootloader sources.
 */
#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

/**
 * @brief Pre-main hardware bring-up. Implemented in system.c.
 *
 * Configures the system clock to a sensible default for the target.
 * On QEMU netduino2 this is a no-op (the model runs at the modeled clock
 * regardless), but symbols/CMSIS hooks expect this to exist.
 */
void SystemInit(void);

/**
 * @brief Top-level main(), invoked by Reset_Handler.
 */
int  main(void);

/**
 * @brief Branch into the application at @p slot_addr.
 *
 * Sets SCB->VTOR to the application's vector table (slot + 0x200), loads
 * the initial MSP from the first vector, and tail-calls the Reset_Handler
 * at vector 1. Never returns.
 */
__attribute__((noreturn))
void bootloader_jump_to_application(uint32_t slot_addr);

/**
 * @brief Run the UART-only recovery menu. Never returns (only system reset
 *        gets you out). Called when both slots fail verification.
 */
__attribute__((noreturn))
void recovery_mode_run(void);

#endif /* BOOTLOADER_H */
