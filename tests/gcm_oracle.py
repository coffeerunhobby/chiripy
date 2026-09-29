#!/usr/bin/env python3
"""Cross-checks the vendored AES-256-GCM against the `cryptography` package.

Runs the compiled crypto_test binary in --kat mode on random inputs (varied
plaintext and AAD lengths, including empty and non-block-aligned) and compares
ciphertext||tag with an independent implementation. Exits non-zero on any
mismatch.

    python3 tests/gcm_oracle.py build_tests/crypto_test [iterations]
"""
import os
import subprocess
import sys

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

binary = sys.argv[1]
iterations = int(sys.argv[2]) if len(sys.argv) > 2 else 200
lengths = [0, 1, 15, 16, 17, 31, 32, 33, 39, 64, 100, 255]

for i in range(iterations):
    key = os.urandom(32)
    nonce = os.urandom(12)
    aad = os.urandom(lengths[i % len(lengths)] if i % 3 else 0)
    pt = os.urandom(lengths[(i * 7) % len(lengths)])
    expected = AESGCM(key).encrypt(nonce, pt, aad)  # ciphertext || tag
    got = subprocess.run(
        [binary, "--kat", key.hex(), nonce.hex(), aad.hex(), pt.hex()],
        capture_output=True, text=True, check=True,
    ).stdout.strip()
    if got != expected.hex():
        print(f"MISMATCH at iteration {i}: pt={len(pt)}B aad={len(aad)}B")
        print(" key  ", key.hex())
        print(" nonce", nonce.hex())
        print(" want ", expected.hex())
        print(" got  ", got)
        sys.exit(1)

print(f"oracle: {iterations} random AES-256-GCM cases match cryptography")
