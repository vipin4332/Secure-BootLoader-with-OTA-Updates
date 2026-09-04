# Production OTA transport (integration guide)

This repository implements the **framed UART OTA protocol** in the bootloader
([`docs/update_protocol.md`](update_protocol.md)). Lab builds bridge USART2 to
TCP port 4444 in QEMU; production products typically carry the same frames over
**TLS-protected TCP** (or another reliable byte stream).

## Layered model

```
+-------------------------------------------------------------+
|  Application task (product firmware)                         |
|  - download manifest, schedule updates, UI / policy          |
+----------------------------+--------------------------------+
                             | calls
+----------------------------v--------------------------------+
|  OTA transport adapter (new code in product tree)            |
|  - lwIP socket + mbedTLS (or vendor TLS stack)               |
|  - reads/writes bytes; no change to frame layout             |
+----------------------------+--------------------------------+
                             | same SOH/OP/SEQ/LEN/CRC32 frames
+----------------------------v--------------------------------+
|  Bootloader OTA client ([`bootloader/src/ota_client.c`](../bootloader/src/ota_client.c)) |
|  - recovery menu today; inactive-slot writes + verify        |
+-------------------------------------------------------------+
```

Host reference: [`tools/ota_protocol.py`](../tools/ota_protocol.py) and
[`tools/ota_server.py`](../tools/ota_server.py). A product adapter should
produce **identical frames**; only the physical transport changes.

## What stays in the bootloader

- Image authentication (CRC32 → SHA-256 → ECDSA-P256).
- Dual-bank inactive-slot writes, delta apply, `ota_pending` swap.
- `rollback_counter` anti-downgrade (see [`bootloader/include/config.h`](../bootloader/include/config.h)).

## What moves to the application / connectivity stack

- DNS, TCP connect, retry/backoff at the socket layer.
- TLS handshake, certificate pinning, optional mutual TLS.
- HTTP(S) manifest fetch (version URL, patch URL) if used — **outside** this repo.

Keeping TLS out of the 64 KB bootloader preserves flash budget and audit surface.

## Threat notes

| Topic | Lab build | Production recommendation |
|-------|-----------|---------------------------|
| Confidentiality | Plaintext UART/TCP | TLS 1.2+; pin server cert or SPKI hash |
| Integrity | ECDSA on firmware | Keep signature verify; TLS adds transport integrity |
| Downgrade | `rollback_counter` in `BootConfig_t` | Enforce monotonic `FirmwareHeader_t.version` |
| Metadata leakage | `START` / `START_DELTA` fields visible | Acceptable if images are signed; avoid secrets in frames |

## Suggested bring-up sequence

1. Prove framed OTA on serial (this repo: `make qemu`, `make ota_server`).
2. Add lwIP + TLS in the **application**, tunnel frames to USART2 or a
   dedicated OTA UART on hardware.
3. Run Renode / silicon with `BUILD=hw` and real flash driver paths.
4. Add factory provisioning for trust anchors (separate from the demo PEM in `keys/`).

## Out of scope in this repository

- lwIP, mbedTLS, HTTP client, or cloud manifest servers.
- Automated Renode CI (manual `make BUILD=hw renode` only).

See also [`docs/security_analysis.md`](security_analysis.md) for the full threat model.
