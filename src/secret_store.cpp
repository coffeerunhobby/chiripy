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

#include "secret_store.hpp"

#include "crypto/aes_gcm.hpp"
#include "crypto/base64.hpp"

#include <random>

namespace chiripy::secret {

namespace {

using namespace chiripy::crypto;

constexpr const char *kPrefix = "v1:";
constexpr size_t kPrefixLen = 3;

// Generated once with os.urandom(32) on 2026-09-29. Changing it invalidates
// every stored key, so bump the "v1:" prefix if it ever has to rotate.
const Key256 kKey = {0x38, 0x83, 0x6f, 0x60, 0x82, 0xbf, 0xbc, 0xbd, 0x5c, 0x2e, 0xe7,
		     0x78, 0xb5, 0x96, 0x7b, 0xe4, 0x98, 0xca, 0x3f, 0xc2, 0x20, 0xf7,
		     0x80, 0x5c, 0x0c, 0xc0, 0x75, 0xa9, 0x08, 0x72, 0x87, 0x1e};

// Bound to the format version so a v1 blob cannot be replayed as a later
// version, and so a blob from some other program using the same primitives
// does not unseal here.
const std::vector<uint8_t> kAad = {'c', 'h', 'i', 'r', 'i', 'p', 'y', '.', 'a',
				   'p', 'i', 'k', 'e', 'y', '.', 'v', '1'};

Nonce96 random_nonce()
{
	std::random_device rd;
	Nonce96 n{};
	for (size_t i = 0; i < n.size(); i += 4) {
		const uint32_t r = rd();
		for (size_t k = 0; k < 4 && i + k < n.size(); ++k)
			n[i + k] = static_cast<uint8_t>(r >> (8 * k));
	}
	return n;
}

} // namespace

std::string seal(const std::string &plaintext)
{
	const Nonce96 nonce = random_nonce();
	const std::vector<uint8_t> pt(plaintext.begin(), plaintext.end());
	std::vector<uint8_t> ct;
	const Tag128 tag = gcm_seal(kKey, nonce, kAad, pt, ct);

	std::vector<uint8_t> blob;
	blob.reserve(nonce.size() + ct.size() + tag.size());
	blob.insert(blob.end(), nonce.begin(), nonce.end());
	blob.insert(blob.end(), ct.begin(), ct.end());
	blob.insert(blob.end(), tag.begin(), tag.end());
	return kPrefix + base64::encode(blob);
}

std::optional<std::string> unseal(const std::string &sealed)
{
	if (sealed.compare(0, kPrefixLen, kPrefix) != 0)
		return std::nullopt;
	const auto blob = base64::decode(sealed.substr(kPrefixLen));
	if (!blob || blob->size() < 12 + 16)
		return std::nullopt;

	Nonce96 nonce{};
	std::copy(blob->begin(), blob->begin() + 12, nonce.begin());
	Tag128 tag{};
	std::copy(blob->end() - 16, blob->end(), tag.begin());
	const std::vector<uint8_t> ct(blob->begin() + 12, blob->end() - 16);

	std::vector<uint8_t> pt;
	if (!gcm_open(kKey, nonce, kAad, ct, tag, pt))
		return std::nullopt;
	return std::string(pt.begin(), pt.end());
}

} // namespace chiripy::secret
