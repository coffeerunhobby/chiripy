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

#include "aes_gcm.hpp"

#include <cstring>

namespace chiripy::crypto {

namespace {

// ---- GF(2^8) arithmetic, polynomial x^8 + x^4 + x^3 + x + 1 (0x11b) ----

uint8_t xtime(uint8_t a)
{
	return static_cast<uint8_t>((a << 1) ^ ((a & 0x80) ? 0x1b : 0x00));
}

uint8_t gf_mul(uint8_t a, uint8_t b)
{
	uint8_t p = 0;
	while (b) {
		if (b & 1)
			p ^= a;
		a = xtime(a);
		b >>= 1;
	}
	return p;
}

// Multiplicative inverse by exhaustive search; only runs once to build the
// S-box, so 256 * 256 multiplications is fine.
uint8_t gf_inv(uint8_t a)
{
	if (a == 0)
		return 0;
	for (unsigned b = 1; b < 256; ++b)
		if (gf_mul(a, static_cast<uint8_t>(b)) == 1)
			return static_cast<uint8_t>(b);
	return 0; // unreachable: every nonzero element has an inverse
}

uint8_t rotl8(uint8_t v, unsigned n)
{
	return static_cast<uint8_t>((v << n) | (v >> (8 - n)));
}

struct SBox {
	std::array<uint8_t, 256> table{};
	SBox()
	{
		for (unsigned x = 0; x < 256; ++x) {
			const uint8_t inv = gf_inv(static_cast<uint8_t>(x));
			// FIPS-197 5.1.1: affine transformation over GF(2)
			table[x] = static_cast<uint8_t>(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^ rotl8(inv, 3) ^
							rotl8(inv, 4) ^ 0x63);
		}
	}
};

const std::array<uint8_t, 256> &sbox()
{
	static const SBox s;
	return s.table;
}

// ---- GF(2^128) for GHASH, bit order as in SP 800-38D 6.3 ----

using Block = std::array<uint8_t, 16>;

Block xor_block(const Block &a, const Block &b)
{
	Block r;
	for (size_t i = 0; i < 16; ++i)
		r[i] = a[i] ^ b[i];
	return r;
}

// Algorithm 1 of SP 800-38D: Z = X * Y. Bit 0 is the MSB of byte 0.
Block gf128_mul(const Block &x, const Block &y)
{
	Block z{};
	Block v = y;
	for (unsigned i = 0; i < 128; ++i) {
		if ((x[i / 8] >> (7 - (i % 8))) & 1)
			z = xor_block(z, v);
		const bool lsb = v[15] & 1;
		// v >>= 1 across the whole 128-bit block
		for (int j = 15; j > 0; --j)
			v[j] = static_cast<uint8_t>((v[j] >> 1) | (v[j - 1] << 7));
		v[0] >>= 1;
		if (lsb)
			v[0] ^= 0xe1; // R = 11100001 || 0^120
	}
	return z;
}

void ghash_update(Block &y, const Block &h, const uint8_t *data, size_t len)
{
	for (size_t off = 0; off < len; off += 16) {
		Block blk{};
		const size_t n = (len - off < 16) ? (len - off) : 16;
		std::memcpy(blk.data(), data + off, n); // zero-padded final block
		y = gf128_mul(xor_block(y, blk), h);
	}
}

void put_be64(uint8_t *out, uint64_t v)
{
	for (int i = 7; i >= 0; --i) {
		out[i] = static_cast<uint8_t>(v & 0xff);
		v >>= 8;
	}
}

Block ghash(const Block &h, const std::vector<uint8_t> &aad, const std::vector<uint8_t> &c)
{
	Block y{};
	ghash_update(y, h, aad.data(), aad.size());
	ghash_update(y, h, c.data(), c.size());
	Block lens{};
	put_be64(lens.data(), static_cast<uint64_t>(aad.size()) * 8);
	put_be64(lens.data() + 8, static_cast<uint64_t>(c.size()) * 8);
	y = gf128_mul(xor_block(y, lens), h);
	return y;
}

void inc32(Block &ctr)
{
	for (int i = 15; i >= 12; --i)
		if (++ctr[i] != 0)
			break;
}

// CTR keystream application shared by seal and open (GCTR in the spec).
void gctr(const Aes256 &aes, Block ctr, const uint8_t *in, size_t len, uint8_t *out)
{
	for (size_t off = 0; off < len; off += 16) {
		inc32(ctr);
		Block ks = ctr;
		aes.encrypt_block(ks.data());
		const size_t n = (len - off < 16) ? (len - off) : 16;
		for (size_t i = 0; i < n; ++i)
			out[off + i] = in[off + i] ^ ks[i];
	}
}

Block make_j0(const Nonce96 &nonce)
{
	Block j0{};
	std::memcpy(j0.data(), nonce.data(), 12);
	j0[15] = 1;
	return j0;
}

Tag128 compute_tag(const Aes256 &aes, const Block &h, const Block &j0, const std::vector<uint8_t> &aad,
		   const std::vector<uint8_t> &c)
{
	Block s = ghash(h, aad, c);
	Block ej0 = j0;
	aes.encrypt_block(ej0.data());
	return xor_block(s, ej0);
}

} // namespace

// ---- AES-256 block cipher (FIPS-197) ----

Aes256::Aes256(const Key256 &key)
{
	constexpr unsigned Nk = 8, Nr = 14;
	const auto &s = sbox();
	uint8_t rcon = 0x01;
	std::memcpy(round_keys_.data(), key.data(), 32);
	for (unsigned i = Nk; i < 4 * (Nr + 1); ++i) {
		uint8_t t[4];
		std::memcpy(t, &round_keys_[(i - 1) * 4], 4);
		if (i % Nk == 0) {
			// RotWord then SubWord, then Rcon on the first byte
			const uint8_t t0 = t[0];
			t[0] = s[t[1]] ^ rcon;
			t[1] = s[t[2]];
			t[2] = s[t[3]];
			t[3] = s[t0];
			rcon = xtime(rcon);
		} else if (i % Nk == 4) {
			for (auto &b : t)
				b = s[b];
		}
		for (unsigned j = 0; j < 4; ++j)
			round_keys_[i * 4 + j] = round_keys_[(i - Nk) * 4 + j] ^ t[j];
	}
}

void Aes256::encrypt_block(uint8_t st[16]) const
{
	constexpr unsigned Nr = 14;
	const auto &s = sbox();

	auto add_round_key = [&](unsigned r) {
		for (unsigned i = 0; i < 16; ++i)
			st[i] ^= round_keys_[r * 16 + i];
	};
	auto sub_bytes = [&]() {
		for (unsigned i = 0; i < 16; ++i)
			st[i] = s[st[i]];
	};
	// State is column-major: byte index = 4 * column + row. Row r rotates
	// left by r columns.
	auto shift_rows = [&]() {
		uint8_t t[16];
		for (unsigned c = 0; c < 4; ++c)
			for (unsigned r = 0; r < 4; ++r)
				t[4 * c + r] = st[4 * ((c + r) % 4) + r];
		std::memcpy(st, t, 16);
	};
	auto mix_columns = [&]() {
		for (unsigned c = 0; c < 4; ++c) {
			uint8_t *col = st + 4 * c;
			const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
			col[0] = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
			col[1] = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
			col[2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
			col[3] = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
		}
	};

	add_round_key(0);
	for (unsigned r = 1; r < Nr; ++r) {
		sub_bytes();
		shift_rows();
		mix_columns();
		add_round_key(r);
	}
	sub_bytes();
	shift_rows();
	add_round_key(Nr);
}

// ---- GCM ----

Tag128 gcm_seal(const Key256 &key, const Nonce96 &nonce, const std::vector<uint8_t> &aad,
		const std::vector<uint8_t> &plaintext, std::vector<uint8_t> &ciphertext)
{
	const Aes256 aes(key);
	Block h{};
	aes.encrypt_block(h.data());
	const Block j0 = make_j0(nonce);

	ciphertext.resize(plaintext.size());
	gctr(aes, j0, plaintext.data(), plaintext.size(), ciphertext.data());
	return compute_tag(aes, h, j0, aad, ciphertext);
}

bool gcm_open(const Key256 &key, const Nonce96 &nonce, const std::vector<uint8_t> &aad,
	      const std::vector<uint8_t> &ciphertext, const Tag128 &tag, std::vector<uint8_t> &plaintext)
{
	plaintext.clear();
	const Aes256 aes(key);
	Block h{};
	aes.encrypt_block(h.data());
	const Block j0 = make_j0(nonce);

	// Verify before decrypting so a forged message never yields plaintext.
	const Tag128 expected = compute_tag(aes, h, j0, aad, ciphertext);
	uint8_t diff = 0;
	for (size_t i = 0; i < 16; ++i)
		diff |= static_cast<uint8_t>(expected[i] ^ tag[i]);
	if (diff != 0)
		return false;

	plaintext.resize(ciphertext.size());
	gctr(aes, j0, ciphertext.data(), ciphertext.size(), plaintext.data());
	return true;
}

} // namespace chiripy::crypto
