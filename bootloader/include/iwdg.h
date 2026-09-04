/**
 * @file    iwdg.h
 * @brief   Independent watchdog driver (IWDG).
 *
 * Configured for ~10 s timeout (LSI 32 kHz, prescaler 256, reload 1250).
 * `iwdg_kick()` must be called periodically from any long-running loop
 * (flash erase/program, OTA receive, ECDSA verify, recovery menu).
 */
#ifndef IWDG_H
#define IWDG_H

void iwdg_init(void);
void iwdg_kick(void);

#endif /* IWDG_H */
