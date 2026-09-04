# micro-ecc (vendored)

Vendored from https://github.com/kmackay/micro-ecc, BSD-2-Clause licensed
(see `LICENSE.txt`). Copyright (c) 2014, Kenneth MacKay.

## Configuration in this project

This bootloader uses **only** SECP256R1 (NIST P-256), verify-only. Compile
flags set in the top-level `Makefile`:

- `uECC_SUPPORTS_secp160r1=0`
- `uECC_SUPPORTS_secp192r1=0`
- `uECC_SUPPORTS_secp224r1=0`
- `uECC_SUPPORTS_secp256r1=1`
- `uECC_SUPPORTS_secp256k1=0`
- `uECC_OPTIMIZATION_LEVEL=2`
- `uECC_SQUARE_FUNC=1`
- `uECC_VLI_NATIVE_LITTLE_ENDIAN=0`
- `uECC_PLATFORM=uECC_arm_thumb` (HW build) / `uECC_arch_other` (host tests)

Public-key API used:

```c
int uECC_verify(const uint8_t *public_key,
                const uint8_t *message_hash,
                unsigned hash_size,
                const uint8_t *signature,
                uECC_Curve curve);
```

The public key is passed as a 64-byte concatenation `[X(32) || Y(32)]`,
and the signature as raw `[r(32) || s(32)]` big-endian. This is exactly
what `tools/sign_firmware.py` produces.
