/*
Chiripy - low-latency YouTube live chat overlay for OBS
Copyright (C) 2026 c4xp <c4xp@msn.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Self-contained AES-256-GCM (NIST SP 800-38D), no OS or third-party
// dependency. Written for a single ~40-byte secret at startup, so it is a
// plain, table-free, bit-at-a-time implementation: correctness and
// auditability over speed. Do not use it for bulk data.
//
// The S-box is derived at first use from the GF(2^8) inverse and affine map
// rather than typed in as a table, so there is nothing to mistype; the known
// answer tests in tests/crypto_test.cpp pin it to the FIPS-197 values.
namespace chiripy::crypto {

using Key256 = std::array<uint8_t, 32>;
using Nonce96 = std::array<uint8_t, 12>;
using Tag128 = std::array<uint8_t, 16>;

class Aes256 {
public:
	explicit Aes256(const Key256 &key);
	// Encrypts one 16-byte block in place. GCM only ever needs the forward
	// cipher, so there is deliberately no decrypt_block.
	void encrypt_block(uint8_t block[16]) const;

private:
	std::array<uint8_t, 240> round_keys_{}; // 4 * (Nr + 1) words, Nr = 14
};

// Authenticated encryption with a 96-bit nonce. Never reuse a nonce with the
// same key: GCM's security collapses completely on nonce reuse.
Tag128 gcm_seal(const Key256 &key, const Nonce96 &nonce, const std::vector<uint8_t> &aad,
		const std::vector<uint8_t> &plaintext, std::vector<uint8_t> &ciphertext);

// Returns false (and leaves plaintext empty) if the tag does not verify.
// Tag comparison is constant-time.
bool gcm_open(const Key256 &key, const Nonce96 &nonce, const std::vector<uint8_t> &aad,
	      const std::vector<uint8_t> &ciphertext, const Tag128 &tag, std::vector<uint8_t> &plaintext);

} // namespace chiripy::crypto
