/**
 * @file    uart.h
 * @brief   USART driver used by logs, recovery, and OTA.
 *
 * Two channels:
 *   - LOG  channel  (USART1 on hardware, QEMU stdio in netduino2): used for
 *     human-readable log lines from the bootloader.
 *   - OTA  channel  (USART2): exposed in QEMU as TCP 127.0.0.1:4444. Used
 *     for the framed OTA protocol and for the recovery menu I/O.
 *
 * The QEMU netduino2 USART model is permissive about register-level setup;
 * the same code path drives a real STM32F4 USART because we go through the
 * standard register layout.
 */
#ifndef UART_H
#define UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void uart_init(void);

/* --- Log channel (USART1 / stdio in QEMU) ------------------------------ */
void uart_log_putc(char c);
void uart_log_puts(const char *s);
void uart_log_printf(const char *fmt, ...);
void uart_log_hex(const void *data, size_t len);

/* --- OTA channel (USART2 / TCP 4444 in QEMU) --------------------------- */
void uart_ota_putc(uint8_t b);
void uart_ota_write(const uint8_t *buf, size_t n);
/**
 * @brief Read up to @p n bytes from the OTA UART, blocking up to
 *        @p timeout_ms milliseconds total. Returns the number of bytes
 *        actually read (which may be 0 on timeout).
 *
 * Polls a coarse cycle counter for the timeout; precise timing isn't
 * required for OTA framing.
 */
size_t uart_ota_read(uint8_t *buf, size_t n, uint32_t timeout_ms);

/**
 * @brief Read exactly one byte; returns false on timeout.
 */
bool   uart_ota_getc(uint8_t *out, uint32_t timeout_ms);

#endif /* UART_H */
