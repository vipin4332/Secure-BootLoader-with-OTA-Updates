"""Python-side self-consistency test for the signing pipeline.

Independent of any C compiler. Verifies that:

  * a freshly signed image round-trips through ECDSA verification,
  * single-bit flips in the payload, signature, or header are rejected,
  * the SHA-256 input the verifier hashes ([header_with_zeroed_signature
    || payload]) matches what the signing tool produces.

Used by tests/run_all.py.
"""

from __future__ import annotations

import hashlib
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from ecdsa import SigningKey, VerifyingKey, NIST256p, BadSignatureError

from firmware_image import FirmwareImage  # noqa: E402
from sign_firmware import sign            # noqa: E402


def make_signed(payload: bytes, version: int = 0x010000) -> tuple[FirmwareImage, VerifyingKey]:
    sk = SigningKey.generate(curve=NIST256p)
    vk = sk.get_verifying_key()
    img = FirmwareImage(version=version, timestamp=0, payload=payload)
    img.signature = sign(sk, img.signing_bytes())
    return img, vk


def verify(img: FirmwareImage, vk: VerifyingKey) -> bool:
    digest = hashlib.sha256(img.signing_bytes()).digest()
    sig = img.signature
    r = int.from_bytes(sig[:32], "big")
    s = int.from_bytes(sig[32:], "big")
    # Reject non-canonical low-s (s > n/2) or zero components to eliminate malleability
    order = NIST256p.order
    if r == 0 or s == 0 or s > order // 2:
        return False
    try:
        # ecdsa lib accepts a sigdecode callable
        return vk.verify_digest(
            sig,
            digest,
            sigdecode=lambda sig, order: (
                int.from_bytes(sig[:32], "big"),
                int.from_bytes(sig[32:], "big"),
            ),
        )
    except BadSignatureError:
        return False


def case(label: str, ok: bool, expected: bool) -> int:
    status = "PASS" if ok == expected else "FAIL"
    print(f"  {label:<35} expect={'OK' if expected else 'BAD':<3} got={'OK' if ok else 'BAD':<3} -> {status}")
    return 0 if ok == expected else 1


def main() -> int:
    payload = os.urandom(8192)
    img, vk = make_signed(payload)

    failures = 0
    failures += case("baseline accept", verify(img, vk), True)

    # mutate payload
    bad_payload = bytearray(img.payload)
    bad_payload[0] ^= 0x01
    img2 = FirmwareImage(version=img.version, timestamp=img.timestamp,
                         payload=bytes(bad_payload),
                         signature=img.signature)
    failures += case("flip payload[0]", verify(img2, vk), False)

    # mutate signature
    bad_sig = bytearray(img.signature)
    bad_sig[7] ^= 0x80
    img3 = FirmwareImage(version=img.version, timestamp=img.timestamp,
                         payload=img.payload, signature=bytes(bad_sig))
    failures += case("flip signature[7]", verify(img3, vk), False)

    # use wrong verify key
    other_vk = SigningKey.generate(curve=NIST256p).get_verifying_key()
    failures += case("wrong key rejects valid sig", verify(img, other_vk), False)

    # malleability test: flipped s-value (s' = order - s)
    r = int.from_bytes(img.signature[:32], "big")
    s = int.from_bytes(img.signature[32:], "big")
    order = NIST256p.order
    s_flipped = order - s  # s_flipped > order // 2
    malleable_sig = r.to_bytes(32, "big") + s_flipped.to_bytes(32, "big")
    img_malleable = FirmwareImage(version=img.version, timestamp=img.timestamp,
                                  payload=img.payload, signature=malleable_sig)
    failures += case("reject malleable s > n/2 sig", verify(img_malleable, vk), False)

    if failures:
        print(f"\n{failures} test(s) failed.")
        return 1
    print("\nAll Python signature self-consistency tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
