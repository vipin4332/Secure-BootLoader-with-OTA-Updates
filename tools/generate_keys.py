#!/usr/bin/env python3
"""Generate a fresh SECP256R1 (NIST P-256) key pair.

Outputs:
    keys/private_key.pem  -- PKCS#8 PEM, KEEP SECRET (gitignored)
    keys/public_key.pem   -- SubjectPublicKeyInfo PEM

Use ``python tools/generate_keys.py --force`` to overwrite an existing key
pair (otherwise the script refuses to clobber existing keys, which is the
right default to protect a key already in use to sign firmware).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from ecdsa import SigningKey, NIST256p
except ImportError:  # pragma: no cover
    sys.stderr.write(
        "error: missing dependency 'ecdsa'. Install it with:\n"
        "    python -m pip install -r tools/requirements.txt\n"
    )
    sys.exit(2)


def generate(out_dir: Path, force: bool) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    priv_path = out_dir / "private_key.pem"
    pub_path = out_dir / "public_key.pem"

    if (priv_path.exists() or pub_path.exists()) and not force:
        sys.stderr.write(
            f"error: keys already exist at {priv_path} / {pub_path}.\n"
            f"       Refusing to overwrite. Pass --force to regenerate.\n"
        )
        sys.exit(1)

    sk = SigningKey.generate(curve=NIST256p)
    vk = sk.get_verifying_key()

    priv_path.write_bytes(sk.to_pem())
    pub_path.write_bytes(vk.to_pem())
    # Restrict permissions on POSIX; on Windows this is a no-op.
    try:
        priv_path.chmod(0o600)
    except OSError:
        pass

    print(f"Wrote {priv_path}")
    print(f"Wrote {pub_path}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out", default="keys", help="Output directory (default: keys/)"
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Overwrite existing keys (DANGEROUS - rotates the signing key)",
    )
    args = parser.parse_args()
    generate(Path(args.out), args.force)


if __name__ == "__main__":
    main()
