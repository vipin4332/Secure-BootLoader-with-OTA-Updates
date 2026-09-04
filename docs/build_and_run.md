# Build and Run

End-to-end instructions for building, running, and updating the
bootloader + demo application. Two simulators are supported:

- **QEMU** (`-machine netduino2`) — universally available, runs
  Cortex-M3 / STM32F2; the bootloader is built with `-mcpu=cortex-m3
  -mfloat-abi=soft` and `-DQEMU_FLASH_SIM=1` for this profile.
- **Renode** (`stm32f4_discovery`) — for full STM32F407 peripheral
  fidelity (FLASH controller register semantics, IWDG ticking, real
  sector erase). The bootloader for Renode is built with
  `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`.

## Prerequisites

| tool                  | version              | install hint                                              |
| --------------------- | -------------------- | --------------------------------------------------------- |
| `arm-none-eabi-gcc`   | 10.0+                | https://developer.arm.com/Tools%20and%20Software/GNU%20Toolchain |
| `qemu-system-arm`     | 7.0+                 | apt / brew / https://www.qemu.org/download/                |
| `make`                | GNU Make 4.0+        | apt / brew / chocolatey                                    |
| `python`              | 3.10+                | https://python.org                                         |
| `python -m pip install -r tools/requirements.txt` | latest | adds `ecdsa` and `pyserial` |
| `hdiffi` (HPatchLite) | built from source    | https://github.com/sisong/HPatchLite — required for `make delta` |
| Renode (optional)     | 1.14+                | https://renode.io (only needed for `stm32f4_discovery`)   |

Run `make preflight` to verify the toolchain is in `PATH`.

## QEMU quickstart

```bash
make keys                      # one-time: generate ECDSA P-256 key pair
make all                       # builds bootloader + signed app + flash.bin

# Terminal A
make qemu                      # launches QEMU; bootloader logs on stdio,
                               # OTA UART exposed as TCP 127.0.0.1:4444

# Terminal B (after QEMU is up)
make ota_server                # pushes build/app_signed.bin into Slot B
                               # via the framed protocol over TCP 4444
```

The OTA push takes ~15 s for a typical 100-200 KB image at 115200-baud
emulated speed. Once the device receives `OP_END` and the verify passes,
it sets the OTA-pending flag in `SharedBootBlock` and resets - the
bootloader then commits the swap and runs the new app.

To launch QEMU manually instead of via `make qemu`:

```bash
qemu-system-arm \
    -machine netduino2 \
    -nographic \
    -kernel build/flash.bin \
    -serial mon:stdio \
    -serial tcp:127.0.0.1:4444,server,nowait
```

QEMU's `netduino2` machine emulates STM32F2 / Cortex-M3. We compile the
QEMU bootloader for Cortex-M3 / soft-float and short-circuit the FLASH
register accesses via `-DQEMU_FLASH_SIM=1` because QEMU does not model
the F2/F4 FLASH peripheral registers cleanly.

## Alternative: Renode for F407 fidelity

Renode emulates STM32F4 with cycle-accurate FLASH register semantics,
which exercises the bootloader's real-hardware flash code path
(`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`, no
`QEMU_FLASH_SIM`).

```bash
make BUILD=hw all              # build the M4 / hard-float profile
make BUILD=hw renode           # emit build/run.resc
renode build/run.resc          # launch Renode (will open a window)
```

The generated `build/run.resc` contains:

```
using sysbus
mach create
machine LoadPlatformDescription @platforms/boards/stm32f4_discovery.repl
sysbus LoadELF @build/bootloader/bootloader.elf
sysbus LoadBinary @build/app_signed.bin 0x08011000
showAnalyzer sysbus.usart2
start
```

Notes:

- Use `LoadELF` for the bootloader so vector table + symbol info come
  from the linker, and `LoadBinary` at `0x08011000` for the app
  (Renode treats this as raw flash content at that address).
- `showAnalyzer sysbus.usart2` opens a UART analyzer window showing
  exactly what the OTA server sees on the wire.
- For OTA testing in Renode, use `emulation CreateUartPtyTerminal "ota"
  "/tmp/ota.tty"` and attach `tools/ota_server.py --serial /tmp/ota.tty`.

## Building only one component

```bash
make bootloader                # bootloader.elf + .bin only (default M3 profile)
make app                        # app.elf + .bin only
make image                      # build/app_unsigned.bin (header + payload)
make sign                       # build/app_signed.bin (signed)
make flash_image                # build/flash.bin (combined 1 MB image)
```

## Switching profiles

```bash
make clean                      # always clean before switching
make BUILD=qemu all             # default; for QEMU
make BUILD=hw   all             # for Renode / real silicon
```

The two profiles produce different .elf files (different `-mcpu`,
`-mfloat-abi`, and `-DQEMU_FLASH_SIM`), so a build-tree clean is
required when switching.

## Tests

```bash
make test                       # runs tests/run_all.py (skips missing tools)
make test STRICT=1              # fail if QEMU/gcc/build artifacts unavailable (CI)
```

Orchestrated tests:

| Script | Requires |
|--------|----------|
| `test_signature_python.py` | Python + `ecdsa` |
| `test_delta_common.py` | Python |
| `test_signature.c` | Host gcc + `make all` + `make keys` |
| `test_power_loss.py` | QEMU + `build/flash.bin` |
| `test_recovery_ota.py` | QEMU + build artifacts |
| `test_delta_ota.py` | QEMU + `hdiffi` + `make delta` |
| `test_rollback.py` | QEMU + `make app_rollback` |

## Delta patches

```bash
make delta                      # build/app_v1_signed.bin, app_v2_signed.bin, app_v1_to_v2.patch
python tools/make_delta.py --compress tuz old.bin new.bin out.patch
```

## Updating the application

1. Bump the version: `make APP_VERSION=1.1.0 all` produces a fresh
   `build/app_signed.bin`.
2. With QEMU running (`make qemu`), push it: `make ota_server`.
3. Watch the bootloader logs - you'll see "OTA: END verified;
   rebooting", then the bootloader's banner again, then "App v1.1.0
   running".

## Triggering rollback by hand

```bash
make clean
make BUILD=qemu APP_VERSION=1.0.0 all
python tools/flash_layout.py \
    --bootloader build/bootloader/bootloader.bin \
    --slot-a build/app_signed.bin \
    --corrupt slotA \
    --out build/flash.bin
make qemu
```

The bootloader will detect Slot A's corrupted byte during signature
verification, log a failure, then verify Slot B - which is `0xFF`-blank
so it'll also fail, and we'll drop into recovery mode. From there you
can issue an `Upload` command (any input on TCP 4444 starting with `U`)
to receive a fresh signed image into Slot A.

## Triggering recovery mode

Same as above with `--corrupt slotA` and Slot B blank: both slots fail
verify, the bootloader prints the recovery banner on USART1 (stdio),
and waits on USART2 (TCP 4444) for a single-letter command (`U` /
`V` / `R`).

---

## Honest Hardware Boundaries & Verification Reality

To maintain strict engineering transparency, the following boundaries define what has been actively validated versus what requires on-silicon validation before deployment:

### What Was Actively Verified
- **QEMU `netduino2` (Cortex-M3 / STM32F2 profile):**
  - Full dual-bank state machine, boot attempt counters, automatic rollback upon failed application confirmation (`SharedBootBlock`), and recovery mode fall-through.
  - In-flight OTA streaming and HPatchLite delta reconstruction (`tools/ota_server.py` over TCP socket bridged to USART2).
  - Power-loss resilience and torn-write recovery under simulated random `SIGKILL` termination (`tests/test_power_loss.py`).
  - Cryptographic validation (ECDSA-P256 with canonical low-S enforcement, SHA-256 integrity, public key validation).
  - Simulated flash operations via an SRAM-backed shadow window (`-DQEMU_FLASH_SIM=1`) to emulate sector erasing and word programming.

### What Has NOT Been Verified on Physical STM32F407 Silicon
The real-hardware peripheral driver (`bootloader/src/flash_driver.c` without `QEMU_FLASH_SIM`) implements the direct register sequences per ST RM0090 Reference Manual, but has **not** been benchmarked or run on physical STM32F407 silicon. Specific hardware realities to address on physical hardware:
1. **Flash Sector Erase Timing & Watchdog Starvation:**
   - On physical STM32F407 silicon, erasing a 128 KB sector (sectors 5 through 11) typically takes **1 to 2 seconds** (up to 4 seconds at cold temperatures or end-of-life silicon wear).
   - In simulation, `flash_erase_sector()` returns immediately. On real hardware, `wait_busy()` inside `flash_erase_sector()` must service `iwdg_kick()` frequently enough to prevent the hardware independent watchdog from resetting the MCU mid-erase.
2. **Flash Write Voltage & Parallelism:**
   - The hardware driver configures 32-bit parallelism (`FLASH_CR_PSIZE_X32`). STM32F407 hardware requires supply voltage **$V_{DD} \ge 2.7\text{ V}$** for 32-bit programming. If the board operates between 2.1 V and 2.7 V, the hardware controller will generate a programming parallelism error (`FLASH_SR_PGPERR`) unless shifted to 16-bit (`PSIZE_X16`).
3. **ART Accelerator & Cache Invalidation:**
   - Physical STM32F4 silicon uses the Adaptive Real-Time (ART) accelerator with an instruction cache, data cache, and prefetch queue.
   - When a new image is programmed into flash, the ART cache must be explicitly reset (`FLASH_ACR_ICRST | FLASH_ACR_DCRST`) before branching to the application. Failing to flush the ART cache on real silicon can result in the Cortex-M4 executing stale instruction lines from pre-update memory. QEMU does not model the ART cache.
4. **Peripheral Interrupt Teardown:**
   - Prior to jumping to the application, all enabled peripheral interrupts (such as USART RXNE/TXE or SysTick) should be explicitly disabled in the NVIC (`NVIC->ICER` and `NVIC->ICPR`). If an interrupt remains pending when `__asm volatile ("cpsie i")` runs after VTOR relocation, the CPU vectors into the application vector table before application startup code initializes `.bss` and memory, causing an immediate HardFault.
5. **Silicon Flash Wear & Endurance:**
   - STM32F4 embedded flash is rated for 10,000 erase/write cycles. The bootloader does not implement wear leveling on Sector 4 (where BootConfig lives). In high-frequency update deployments, a ping-pong alternating sector scheme or EEPROM emulation with wear leveling is required.
