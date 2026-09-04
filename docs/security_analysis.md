# Security Analysis

This document explains the bootloader's security model, threat assumptions,
and the residual risks that this project does **not** mitigate. Treat it
as the starting point for a real product threat model rather than a
finished one.

## Goals

The bootloader provides:

1. **Authenticated firmware updates** — only firmware images signed with
   the project's ECDSA-P256 private key will be accepted into either slot.
2. **Tamper detection on boot** — at every boot the active slot is
   re-verified end-to-end (CRC32 → SHA-256 → ECDSA), so post-deployment
   modification of flash (e.g. via JTAG, or via a write to flash from a
   compromised application) is caught before control transfers.
3. **Automatic rollback** — three consecutive failed boots cause the
   bootloader to switch slots, so a bad update cannot brick the device
   indefinitely.
4. **Atomic update** — power-loss in the middle of an OTA write leaves
   the previous active slot intact (it was never modified) and the new
   image either fully verified or rejected.

## Trust boundaries

```
+----------------------------+   trusted: signing key, build host
|        offline              |
+----------------------------+
              |
              | signed image (.bin), distributed any way
              v
+----------------------------+   semi-trusted: device flash, RAM,
|     device (this project)    |  application code (we can revoke
+----------------------------+   it via OTA but cannot prevent it
                                 from running once we've jumped to it)
```

The signing key never leaves the offline / build host. The device only
ever sees the public key (embedded in `bootloader/include/public_key.h`
at build time).

## Threat model

| threat                                            | mitigation in this project                   |
| ------------------------------------------------- | -------------------------------------------- |
| **Forged firmware** (attacker-supplied image, no key access) | ECDSA-P256 signature verification on every boot. |
| **Bit-flipped firmware** (transit/storage error)  | CRC32 fast pre-check + SHA-256 cover the whole image. |
| **Power-loss mid-update**                         | Erase-then-program-then-verify; CRC field of `BootConfig_t` written **last** so torn writes are detectable. Active slot never modified in place. |
| **Brick via bad update**                          | `boot_attempts > 3` triggers rollback to the other slot, with `boot_attempts` reset on switch to prevent ping-pong. |
| **Application tampering with firmware**           | App has no flash-driver code; would need to replicate the FLASH controller key sequence and target the inactive slot. The active slot is the running image, so it cannot replace itself. |
| **Debug-bypass**                                  | The verify path is mandatory and has no `#ifdef DEBUG_SKIP_VERIFY`. Production builds should additionally enable RDP level 2 in the option bytes. |

## Threats this project does **not** mitigate

1. **Signing-key compromise.** If an attacker obtains the ECDSA-P256
   private key (e.g. from a compromised build server), they can sign
   arbitrary firmware. Mitigations a real product would add:
   - hardware-backed key storage (HSM / cloud KMS) for the signing
     operation,
   - an on-device anti-rollback counter (`BootConfig_t.rollback_counter`
     plus per-slot version fields) — **enforced in this build** via
     `boot_config_firmware_allowed()` before OTA commit and updated
     after a successful swap; rejects signed images below the recorded
     version floor,
   - a public-key revocation mechanism (multi-key configuration where
     the bootloader can be told via OTA "from now on, accept these N
     new keys; reject the old one").
2. **Side-channel / fault-injection attacks on the verifier.** This is
   a software-only project. micro-ecc's `uECC_verify` is not constant-
   time on all paths; for a high-value target use a hardware crypto
   accelerator (STM32 CRYP/PKA) or a constant-time library and add
   double-checking + error-detection on the verifier's branches.
3. **Bus-snooping during OTA.** The QEMU UART transport is plaintext.
   In production, OTA must be carried over TLS (or equivalent),
   especially if any OTA-borne data is sensitive (it usually isn't,
   because the image is signed and integrity-protected, but any
   metadata in the START frame might be).
4. **Hardware-level read-out of flash.** RDP option bytes are out of
   scope; with RDP level 0 (the default) any debugger can dump the
   bootloader and discover the embedded public key. This is not a
   secret (it's by design), but the same debugger could also write
   arbitrary data to flash and bypass the verifier on next boot.
5. **Glitching the CPU during the verify branch.** A precisely-timed
   power glitch during the `mbedtls_ecdsa_verify`/`uECC_verify` return
   path could flip a register value and turn a "fail" into a "pass".
   Real silicon-level mitigation (TZ-M, dual-core fault detection,
   redundant compares) is out of scope.

## Production crypto stack

This project uses [micro-ecc](https://github.com/kmackay/micro-ecc) (~6 KB
compiled) for the on-device verifier. It's BSD-2 licensed, widely
deployed, and small enough to fit comfortably in the 64 KB bootloader
budget. The `crypto.h` API is deliberately backend-agnostic so swapping
in a different verifier is mechanical.

For production, **mbedTLS** is the recommended alternative because:

- it has hardware-acceleration hooks for the STM32 CRYP/HASH/PKA
  peripherals (much faster verify, ~10-20× speedup on F4/L5 silicon),
- it is FIPS-validatable (PSA Crypto API),
- it has been audited many times over.

To swap:

1. Vendor the trimmed mbedTLS sources at `third_party/mbedtls/` with a
   `mbedtls_config_min.h` enabling only `MBEDTLS_ECDSA_C`,
   `MBEDTLS_ECP_DP_SECP256R1_ENABLED`, `MBEDTLS_SHA256_C`,
   `MBEDTLS_BIGNUM_C`.
2. Replace the `uECC_verify` call in `bootloader/src/crypto.c` with the
   equivalent `mbedtls_ecdsa_verify` (the project's `crypto.h` API
   doesn't change).
3. Adjust the Makefile to add a `CRYPTO_BACKEND=mbedtls` variable that
   selects the right vendor directory and `-D` flags.

The signing convention (raw r||s, 64 B big-endian over
`SHA256(header_with_zero_sig || payload)`) is unchanged - this is
what `tools/sign_firmware.py` produces, and both micro-ecc and mbedTLS
consume that format identically.

## Defense in depth

Beyond the cryptographic checks, the bootloader uses several smaller
mechanisms whose collective effect is "fail closed":

- **Bounded loops everywhere.** `MAX_SWAP_RETRIES` in `main.c`,
  `wait_busy()` in `flash_driver.c`, and the polling timeout in `uart.c`
  all guarantee no within-boot infinite loop.
- **IWDG.** A 10-second hardware watchdog kicks in if the bootloader
  ever stops feeding it. Every long path (flash erase, OTA receive,
  ECDSA verify) explicitly calls `iwdg_kick()`.
- **Compile-time alignment proofs.** `_Static_assert` in
  `common/memory_map.h` and `common/firmware_format.h` catches header /
  layout drift at build time, so a future header field that breaks VTOR
  alignment is a compile error rather than a silent runtime crash.
