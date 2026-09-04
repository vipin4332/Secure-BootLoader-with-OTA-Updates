/**
 * @file    uart.c
 * @brief   STM32F4 USART driver (USART1 = log, USART2 = OTA).
 *
 * Register layout follows RM0090 §30. The QEMU netduino2 model implements
 * the same registers (CR1.UE/TE/RE, SR.TXE/RXNE, DR), so the same code
 * path works in simulation and on real silicon.
 *
 * We bring up RCC + GPIO state defensively. On QEMU some of the writes are
 * ignored harmlessly; on real F4 they're necessary to physically wire up
 * the pins.
 */

#include "uart.h"
#include "iwdg.h"

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* --- RCC ------------------------------------------------------------- */
#define RCC_BASE        0x40023800UL
#define RCC_AHB1ENR     (*(volatile uint32_t *)(RCC_BASE + 0x30U))
#define RCC_APB1ENR     (*(volatile uint32_t *)(RCC_BASE + 0x40U))
#define RCC_APB2ENR     (*(volatile uint32_t *)(RCC_BASE + 0x44U))

#define RCC_AHB1ENR_GPIOA   (1U << 0)
#define RCC_APB1ENR_USART2  (1U << 17)
#define RCC_APB2ENR_USART1  (1U << 4)

/* --- GPIOA ----------------------------------------------------------- */
#define GPIOA_BASE      0x40020000UL
#define GPIOA_MODER     (*(volatile uint32_t *)(GPIOA_BASE + 0x00U))
#define GPIOA_AFRL      (*(volatile uint32_t *)(GPIOA_BASE + 0x20U))
#define GPIOA_AFRH      (*(volatile uint32_t *)(GPIOA_BASE + 0x24U))

/* --- USART register offsets ------------------------------------------ */
#define USART_SR_OFF    0x00U
#define USART_DR_OFF    0x04U
#define USART_BRR_OFF   0x08U
#define USART_CR1_OFF   0x0CU

#define USART1_BASE     0x40011000UL
#define USART2_BASE     0x40004400UL

#define USART_REG(base, off)  (*(volatile uint32_t *)((base) + (off)))

#define USART_SR_TXE    (1U << 7)
#define USART_SR_RXNE   (1U << 5)

#define USART_CR1_UE    (1U << 13)
#define USART_CR1_TE    (1U << 3)
#define USART_CR1_RE    (1U << 2)

/* APB1 = 42 MHz, APB2 = 84 MHz on a typical F4 setup at 168 MHz core.
 * BRR = fck / baud ; for 115200:
 *   USART1 (APB2 84M):  BRR = 84e6 / 115200 = 729 -> 0x2D9
 *   USART2 (APB1 42M):  BRR = 42e6 / 115200 = 364 -> 0x16C
 *
 * QEMU's netduino2 ignores these and uses the chardev, but we set them
 * for real-HW correctness. */
#define USART1_BRR_115200  0x2D9U
#define USART2_BRR_115200  0x16CU

static void usart_init(uint32_t base, uint32_t brr_val)
{
    USART_REG(base, USART_CR1_OFF) = 0U;             /* disable */
    USART_REG(base, USART_BRR_OFF) = brr_val;
    USART_REG(base, USART_CR1_OFF) = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

static void usart_putc(uint32_t base, uint8_t c)
{
    /* Bounded busy-wait on TXE so a wedged UART can't lock us up forever. */
    for (uint32_t i = 0; i < 100000U; ++i) {
        if (USART_REG(base, USART_SR_OFF) & USART_SR_TXE) break;
    }
    USART_REG(base, USART_DR_OFF) = c;
}

static int usart_try_getc(uint32_t base, uint8_t *out)
{
    if (USART_REG(base, USART_SR_OFF) & USART_SR_RXNE) {
        *out = (uint8_t)(USART_REG(base, USART_DR_OFF) & 0xFFU);
        return 1;
    }
    return 0;
}

/* ===================================================================== */
/* Public API                                                              */
/* ===================================================================== */
void uart_init(void)
{
    /* Clock: GPIOA + USART1 (APB2) + USART2 (APB1). */
    RCC_AHB1ENR |= RCC_AHB1ENR_GPIOA;
    RCC_APB2ENR |= RCC_APB2ENR_USART1;
    RCC_APB1ENR |= RCC_APB1ENR_USART2;

    /* GPIO alternate function setup:
     *   PA2  -> USART2_TX (AF7)
     *   PA3  -> USART2_RX (AF7)
     *   PA9  -> USART1_TX (AF7)
     *   PA10 -> USART1_RX (AF7)
     *
     * MODER: 2 bits per pin, value 0b10 = alternate function.
     * AFRL/AFRH: 4 bits per pin, value 0x7 = AF7.
     */
    uint32_t mode = GPIOA_MODER;
    mode &= ~((3U <<  4) | (3U <<  6) | (3U << 18) | (3U << 20));
    mode |=  ((2U <<  4) | (2U <<  6) | (2U << 18) | (2U << 20));
    GPIOA_MODER = mode;

    uint32_t afrl = GPIOA_AFRL;
    afrl &= ~((0xFU <<  8) | (0xFU << 12));   /* PA2/PA3 */
    afrl |=  ((0x7U <<  8) | (0x7U << 12));
    GPIOA_AFRL = afrl;

    uint32_t afrh = GPIOA_AFRH;
    afrh &= ~((0xFU <<  4) | (0xFU <<  8));   /* PA9/PA10 */
    afrh |=  ((0x7U <<  4) | (0x7U <<  8));
    GPIOA_AFRH = afrh;

    usart_init(USART1_BASE, USART1_BRR_115200);
    usart_init(USART2_BASE, USART2_BRR_115200);
}

/* --- Log channel ----------------------------------------------------- */
void uart_log_putc(char c)            { usart_putc(USART1_BASE, (uint8_t)c); }

void uart_log_puts(const char *s)
{
    while (*s) uart_log_putc(*s++);
}

static void log_print_uint(unsigned long v, unsigned base, int min_width,
                           int upper)
{
    char buf[32];
    int  i = 0;
    if (v == 0U) buf[i++] = '0';
    while (v > 0U) {
        unsigned d = (unsigned)(v % base);
        buf[i++] = (char)(d < 10U
                          ? ('0' + d)
                          : ((upper ? 'A' : 'a') + d - 10U));
        v /= base;
    }
    while (i < min_width) buf[i++] = '0';
    while (i--) uart_log_putc(buf[i]);
}

void uart_log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt != '%') { uart_log_putc(*fmt++); continue; }
        ++fmt;
        int width = 0;
        int zero  = 0;
        if (*fmt == '0') { zero = 1; ++fmt; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            ++fmt;
        }
        (void)zero; /* always pad with zeros for hex; no-op flag for others */
        switch (*fmt) {
        case 'c': uart_log_putc((char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            uart_log_puts(s);
            break;
        }
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { uart_log_putc('-'); v = -v; }
            log_print_uint((unsigned long)v, 10, 0, 0);
            break;
        }
        case 'u':
            log_print_uint(va_arg(ap, unsigned int), 10, 0, 0);
            break;
        case 'x':
            log_print_uint(va_arg(ap, unsigned int), 16, width, 0);
            break;
        case 'X':
            log_print_uint(va_arg(ap, unsigned int), 16, width, 1);
            break;
        case 'p':
            uart_log_puts("0x");
            log_print_uint((unsigned long)(uintptr_t)va_arg(ap, void *),
                           16, 8, 0);
            break;
        case '%': uart_log_putc('%'); break;
        default: uart_log_putc('?'); break;
        }
        if (*fmt) ++fmt;
    }
    va_end(ap);
}

void uart_log_hex(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) uart_log_printf("%02x", p[i]);
}

/* --- OTA channel ----------------------------------------------------- */
void uart_ota_putc(uint8_t b)
{
    usart_putc(USART2_BASE, b);
}

void uart_ota_write(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; ++i) usart_putc(USART2_BASE, buf[i]);
}

/*
 * Coarse busy-wait timeout. Our timing reference is plain CPU loops, since
 * we may not have SysTick configured before we get here. Each iteration is
 * ~10 cycles; we calibrate `polls_per_ms` for ~16 MHz default clock (which
 * is what QEMU netduino2 effectively runs at) and accept a 2x slop.
 */
#define POLLS_PER_MS  1600U

bool uart_ota_getc(uint8_t *out, uint32_t timeout_ms)
{
    uint64_t budget = (uint64_t)timeout_ms * POLLS_PER_MS + 1U;
    while (budget--) {
        uint8_t c;
        if (usart_try_getc(USART2_BASE, &c)) {
            *out = c;
            return true;
        }
        if ((budget & 0x3FFFFU) == 0U) iwdg_kick();
    }
    return false;
}

size_t uart_ota_read(uint8_t *buf, size_t n, uint32_t timeout_ms)
{
    size_t got = 0;
    while (got < n) {
        if (!uart_ota_getc(&buf[got], timeout_ms)) break;
        ++got;
    }
    return got;
}
