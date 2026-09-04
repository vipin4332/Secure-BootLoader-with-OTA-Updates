#!/usr/bin/env python3
"""Push a signed firmware image to the bootloader's UART OTA receiver.

Two transports are supported:

  * --tcp HOST:PORT      (default: 127.0.0.1:4444)
        Connects to the QEMU `-serial tcp:HOST:PORT,server,nowait` socket.
        Cross-platform; works identically on Windows / Linux / macOS.

  * --serial DEVICE      (e.g. COM5 or /dev/ttyUSB0)
        Connects to a real UART. Requires pyserial.

Wire format mirrors bootloader/include/ota_client.h:
    [SOH(1)][OP(1)][SEQ(2)][LEN(2)][DATA(LEN)][CRC32(4)]    (LE)
Response:
    [ACK(0x06) | NAK(0x15)][SEQ(2)]                         (LE)

The script chunks the image into CHUNK_SIZE-byte payloads (default 512 B),
ACK-stops between frames, and retransmits on NAK or timeout up to RETRIES
times per frame.
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import time
from pathlib import Path

# Local module (run from repo root: python tools/ota_server.py)
import ota_protocol as ota

try:
    from delta_common import logical_signed_sha256, logical_signed_total_length
except ImportError:
    logical_signed_sha256 = None  # type: ignore[misc, assignment]
    logical_signed_total_length = None  # type: ignore[misc, assignment]

DEFAULT_TCP = "127.0.0.1:4444"
DEFAULT_BAUD = 115200
DEFAULT_CHUNK = 512
RETRIES_PER_FRAME = 5
RESPONSE_TIMEOUT_S = 2.0


# ===================================================================== #
# Transport abstraction                                                   #
# ===================================================================== #
class Transport:
    def write(self, data: bytes) -> None:
        raise NotImplementedError

    def read_exact(self, n: int, timeout: float) -> bytes:
        raise NotImplementedError

    def close(self) -> None:
        raise NotImplementedError


class TcpTransport(Transport):
    def __init__(self, host: str, port: int, retry_seconds: float = 5.0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        deadline = time.monotonic() + retry_seconds
        last_err: Exception | None = None
        while time.monotonic() < deadline:
            try:
                self.sock.connect((host, port))
                last_err = None
                break
            except (ConnectionRefusedError, OSError) as e:
                last_err = e
                time.sleep(0.25)
        if last_err is not None:
            raise SystemExit(
                f"error: could not connect to {host}:{port} - {last_err}"
            )

    def write(self, data: bytes) -> None:
        self.sock.sendall(data)

    def read_exact(self, n: int, timeout: float) -> bytes:
        self.sock.settimeout(timeout)
        chunks: list[bytes] = []
        remaining = n
        try:
            while remaining > 0:
                buf = self.sock.recv(remaining)
                if not buf:
                    raise TimeoutError("connection closed by peer")
                chunks.append(buf)
                remaining -= len(buf)
        except socket.timeout as e:
            raise TimeoutError(f"timed out waiting for {n} B") from e
        return b"".join(chunks)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


class SerialTransport(Transport):
    def __init__(self, port: str, baud: int):
        try:
            import serial  # type: ignore
        except ImportError as e:  # pragma: no cover
            raise SystemExit(
                "error: --serial requires pyserial. Install with:\n"
                "    python -m pip install -r tools/requirements.txt"
            ) from e
        self.ser = serial.Serial(port=port, baudrate=baud, timeout=0)

    def write(self, data: bytes) -> None:
        self.ser.write(data)
        self.ser.flush()

    def read_exact(self, n: int, timeout: float) -> bytes:
        self.ser.timeout = timeout
        buf = self.ser.read(n)
        if len(buf) != n:
            raise TimeoutError(f"timed out, got {len(buf)} of {n}")
        return bytes(buf)

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:  # pragma: no cover
            pass


# ===================================================================== #
# OTA push                                                                #
# ===================================================================== #
def send_with_retry(t: Transport, frame: bytes, expected_seq: int) -> bool:
    for attempt in range(1, RETRIES_PER_FRAME + 1):
        t.write(frame)
        try:
            resp = t.read_exact(ota.RESPONSE_SIZE, RESPONSE_TIMEOUT_S)
        except TimeoutError:
            sys.stderr.write(
                f"  attempt {attempt}/{RETRIES_PER_FRAME}: response timeout\n"
            )
            continue
        try:
            code, seq = ota.parse_response(resp)
        except ValueError as e:
            sys.stderr.write(f"  attempt {attempt}: malformed resp: {e}\n")
            continue
        if seq != expected_seq:
            sys.stderr.write(
                f"  attempt {attempt}: seq mismatch (got {seq}, "
                f"want {expected_seq})\n"
            )
            continue
        if not ota.is_ack(code):
            sys.stderr.write(f"  attempt {attempt}: NAK seq={seq}\n")
            continue
        return True
    return False


def progress(seq: int, sent: int, total: int) -> None:
    pct = (sent * 100) / total if total else 100
    bar_w = 40
    fill = int(bar_w * sent / total) if total else bar_w
    bar = "#" * fill + "-" * (bar_w - fill)
    sys.stdout.write(
        f"\r[{bar}] seq={seq:5d} {sent:>7}/{total} B ({pct:5.1f}%)"
    )
    sys.stdout.flush()


def push(t: Transport, image: bytes, chunk_size: int) -> bool:
    total = len(image)
    print(f"Pushing {total} B in chunks of {chunk_size} B...")
    seq = 1

    # START frame: 4-byte LE total_size in the data payload.
    start_data = struct.pack("<I", total)
    if not send_with_retry(t, ota.build_frame(ota.OP_START, seq, start_data), seq):
        sys.stderr.write("\nfatal: START failed after retries\n")
        return False

    sent = 0
    t0 = time.monotonic()
    while sent < total:
        seq = (seq + 1) & 0xFFFF
        chunk = image[sent : sent + chunk_size]
        if not send_with_retry(t, ota.build_frame(ota.OP_DATA, seq, chunk), seq):
            sys.stderr.write(f"\nfatal: DATA frame seq={seq} failed\n")
            return False
        sent += len(chunk)
        progress(seq, sent, total)
    elapsed = time.monotonic() - t0
    print(f"\nTransfer complete in {elapsed:.2f}s "
          f"({(sent / 1024.0) / elapsed:.1f} KiB/s)")

    seq = (seq + 1) & 0xFFFF
    end_ok = send_with_retry(t, ota.build_frame(ota.OP_END, seq), seq)
    if not end_ok:
        sys.stderr.write("fatal: END failed after retries\n")
        return False

    print("END acknowledged. Device should now reboot and swap slots.")
    return True


def push_delta(
    t: Transport,
    patch: bytes,
    old_signed: Path,
    new_signed: Path,
    chunk_size: int,
) -> bool:
    if logical_signed_sha256 is None or logical_signed_total_length is None:
        sys.stderr.write("error: delta_common import failed\n")
        return False

    digest = logical_signed_sha256(old_signed)
    expected_new = logical_signed_total_length(new_signed)
    total = len(patch)
    print(
        f"Pushing DELTA patch {total} B -> expected new image {expected_new} B..."
    )

    seq = 1
    payload = struct.pack("<II", total, expected_new) + digest
    assert len(payload) == 40
    if not send_with_retry(
        t, ota.build_frame(ota.OP_START_DELTA, seq, payload), seq
    ):
        sys.stderr.write("\nfatal: START_DELTA failed after retries\n")
        return False

    sent = 0
    t0 = time.monotonic()
    while sent < total:
        seq = (seq + 1) & 0xFFFF
        chunk = patch[sent : sent + chunk_size]
        if not send_with_retry(t, ota.build_frame(ota.OP_DATA, seq, chunk), seq):
            sys.stderr.write(f"\nfatal: DATA frame seq={seq} failed\n")
            return False
        sent += len(chunk)
        progress(seq, sent, total)
    elapsed = time.monotonic() - t0
    print(
        f"\nDelta transfer complete in {elapsed:.2f}s "
        f"({(sent / 1024.0) / elapsed:.1f} KiB/s)"
    )

    seq = (seq + 1) & 0xFFFF
    end_ok = send_with_retry(t, ota.build_frame(ota.OP_END, seq), seq)
    if not end_ok:
        sys.stderr.write("fatal: END failed after retries\n")
        return False

    print("END acknowledged. Device should now reboot and swap slots.")
    return True


# ===================================================================== #
# CLI                                                                     #
# ===================================================================== #
def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "image",
        type=Path,
        nargs="?",
        help="Signed firmware image (full OTA), or patch.bin with --delta",
    )
    parser.add_argument(
        "--tcp",
        default=DEFAULT_TCP,
        help=f"HOST:PORT (default: {DEFAULT_TCP})",
    )
    parser.add_argument("--serial", help="Use a serial port instead of TCP")
    parser.add_argument(
        "--baud", type=int, default=DEFAULT_BAUD, help="Serial baud rate"
    )
    parser.add_argument(
        "--chunk",
        type=int,
        default=DEFAULT_CHUNK,
        help=f"DATA chunk size (default {DEFAULT_CHUNK})",
    )
    parser.add_argument(
        "--delta",
        action="store_true",
        help="Send a patch file (HPatchLite; uncompressed or tinyuz-compressed)",
    )
    parser.add_argument(
        "--old-signed",
        type=Path,
        help="With --delta: signed image currently on the device (base for SHA-256)",
    )
    parser.add_argument(
        "--new-signed",
        type=Path,
        help="With --delta: target signed image (length check vs patch output)",
    )
    args = parser.parse_args()

    if args.delta:
        if args.image is None or args.old_signed is None or args.new_signed is None:
            sys.stderr.write(
                "error: --delta requires image (patch.bin), --old-signed, --new-signed\n"
            )
            sys.exit(2)
    elif args.image is None:
        sys.stderr.write("error: missing signed firmware image argument\n")
        sys.exit(2)

    if not args.image.exists():
        sys.stderr.write(f"error: {args.image} does not exist\n")
        sys.exit(1)
    if args.chunk < 1 or args.chunk > ota.MAX_PAYLOAD:
        sys.stderr.write(
            f"error: chunk size must be in [1, {ota.MAX_PAYLOAD}]\n"
        )
        sys.exit(1)

    if args.delta:
        patch_blob = args.image.read_bytes()
        if not args.old_signed.exists() or not args.new_signed.exists():
            sys.stderr.write("error: --old-signed / --new-signed must exist\n")
            sys.exit(1)
    else:
        patch_blob = args.image.read_bytes()

    if args.serial:
        print(f"Connecting to {args.serial} @ {args.baud}...")
        transport: Transport = SerialTransport(args.serial, args.baud)
    else:
        host, _, port_s = args.tcp.partition(":")
        if not port_s:
            sys.stderr.write(f"error: --tcp must be HOST:PORT, got {args.tcp}\n")
            sys.exit(1)
        port = int(port_s)
        print(f"Connecting to TCP {host}:{port}...")
        transport = TcpTransport(host, port)

    try:
        if args.delta:
            ok = push_delta(
                transport,
                patch_blob,
                args.old_signed,
                args.new_signed,
                args.chunk,
            )
        else:
            ok = push(transport, patch_blob, args.chunk)
    finally:
        transport.close()
    sys.exit(0 if ok else 2)


if __name__ == "__main__":
    main()
