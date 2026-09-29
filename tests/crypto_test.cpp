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

// Known-answer tests for the vendored AES-256-GCM, base64 and the secret
// store. Builds without OBS:
//   clang++ -std=c++17 -I src tests/crypto_test.cpp src/crypto/*.cpp src/secret_store.cpp
// Usage:
//   crypto_test                      run the built-in vectors, exit 0 on pass
//   crypto_test --kat KEY NONCE AAD PT   print hex(ciphertext||tag) for an
//                                        external oracle (all args hex)
//   crypto_test --seal < keyfile         print the sealed form of stdin
//                                        (trailing newline stripped), for
//                                        writing config.json by hand

#include "crypto/aes_gcm.hpp"
#include "crypto/base64.hpp"
#include "secret_store.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace chiripy;
using namespace chiripy::crypto;

namespace {

int failures = 0;

std::vector<uint8_t> hex(const char *s)
{
	std::vector<uint8_t> out;
	for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
		unsigned v;
		std::sscanf(s + i, "%2x", &v);
		out.push_back(static_cast<uint8_t>(v));
	}
	return out;
}

std::string to_hex(const uint8_t *p, size_t n)
{
	std::string s;
	char buf[3];
	for (size_t i = 0; i < n; ++i) {
		std::snprintf(buf, sizeof buf, "%02x", p[i]);
		s += buf;
	}
	return s;
}

template<size_t N> std::array<uint8_t, N> arr(const std::vector<uint8_t> &v)
{
	std::array<uint8_t, N> a{};
	std::memcpy(a.data(), v.data(), N);
	return a;
}

void check(bool ok, const char *what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		++failures;
}

// AES-256-GCM vectors from the GCM specification (McGrew & Viega, "The
// Galois/Counter Mode of Operation", appendix B), test cases 13-16.
void gcm_vectors()
{
	struct V {
		const char *name, *key, *iv, *aad, *pt, *ct, *tag;
	};
	const V vectors[] = {
		{"GCM test case 13 (empty)", "0000000000000000000000000000000000000000000000000000000000000000",
		 "000000000000000000000000", "", "", "", "530f8afbc74536b9a963b4f1c4cb738b"},
		{"GCM test case 14 (one zero block)",
		 "0000000000000000000000000000000000000000000000000000000000000000", "000000000000000000000000", "",
		 "00000000000000000000000000000000", "cea7403d4d606b6e074ec5d3baf39d18", "d0d1c8a799996bf0265b98b5d48ab919"},
		{"GCM test case 15 (4 blocks)",
		 "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888", "",
		 "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255",
		 "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad",
		 "b094dac5d93471bdec1a502270e3cc6c"},
		{"GCM test case 16 (AAD + partial block)",
		 "feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308", "cafebabefacedbaddecaf888",
		 "feedfacedeadbeeffeedfacedeadbeefabaddad2",
		 "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39",
		 "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662",
		 "76fc6ece0f4e1768cddf8853bb2d551b"},
	};
	for (const auto &v : vectors) {
		std::vector<uint8_t> ct;
		const Tag128 tag = gcm_seal(arr<32>(hex(v.key)), arr<12>(hex(v.iv)), hex(v.aad), hex(v.pt), ct);
		const bool ok = ct == hex(v.ct) && tag == arr<16>(hex(v.tag));
		check(ok, v.name);
		if (!ok)
			std::printf("      got ct=%s tag=%s\n", to_hex(ct.data(), ct.size()).c_str(),
				    to_hex(tag.data(), 16).c_str());

		std::vector<uint8_t> pt;
		check(gcm_open(arr<32>(hex(v.key)), arr<12>(hex(v.iv)), hex(v.aad), ct, tag, pt) && pt == hex(v.pt),
		      "  ...open round-trips");
	}
}

void gcm_tamper()
{
	const Key256 key = arr<32>(hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308"));
	const Nonce96 iv = arr<12>(hex("cafebabefacedbaddecaf888"));
	const std::vector<uint8_t> aad = {1, 2, 3};
	const std::vector<uint8_t> pt = {'A', 'I', 'z', 'a', 'S', 'y'};
	std::vector<uint8_t> ct, out;
	Tag128 tag = gcm_seal(key, iv, aad, pt, ct);

	ct[0] ^= 0x01;
	check(!gcm_open(key, iv, aad, ct, tag, out) && out.empty(), "flipped ciphertext bit rejected");
	ct[0] ^= 0x01;
	tag[15] ^= 0x80;
	check(!gcm_open(key, iv, aad, ct, tag, out), "flipped tag bit rejected");
	tag[15] ^= 0x80;
	check(!gcm_open(key, iv, {1, 2, 4}, ct, tag, out), "wrong AAD rejected");
	check(gcm_open(key, iv, aad, ct, tag, out) && out == pt, "untouched message accepted");
}

void base64_vectors()
{
	// RFC 4648 section 10
	const std::pair<const char *, const char *> v[] = {
		{"", ""},           {"f", "Zg=="},         {"fo", "Zm8="},        {"foo", "Zm9v"},
		{"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
	};
	for (const auto &[raw, enc] : v) {
		const std::vector<uint8_t> bytes(raw, raw + std::strlen(raw));
		check(base64::encode(bytes) == enc, (std::string("base64 encode \"") + raw + "\"").c_str());
		const auto dec = base64::decode(enc);
		check(dec && *dec == bytes, (std::string("base64 decode \"") + enc + "\"").c_str());
	}
	check(!base64::decode("Zm9"), "base64 rejects length not multiple of 4");
	check(!base64::decode("Zm9v!A=="), "base64 rejects character outside alphabet");
	check(!base64::decode("Zg==Zg=="), "base64 rejects data after padding");
	check(!base64::decode("=Zm9"), "base64 rejects padding in wrong slot");
}

void secret_store()
{
	const std::string key = "AIzaSyDUMMY-KEY-0123456789abcdefghijklm"; // 39 chars, key-shaped
	const std::string sealed = secret::seal(key);
	check(sealed.rfind("v1:", 0) == 0, "sealed value carries v1 prefix");
	check(sealed.find("AIza") == std::string::npos, "sealed value does not contain the key");
	check(secret::seal(key) != sealed, "two seals of the same key differ (fresh nonce)");
	const auto back = secret::unseal(sealed);
	check(back && *back == key, "unseal returns the original key");
	std::string edited = sealed;
	edited[edited.size() - 3] ^= 0x01; // inside the tag
	check(!secret::unseal(edited), "edited sealed value rejected");
	check(!secret::unseal("v1:"), "empty payload rejected");
	check(!secret::unseal("v2:AAAA"), "unknown version rejected");
	check(!secret::unseal(""), "empty string rejected");
	check(secret::unseal(secret::seal("")) == std::string(""), "empty secret round-trips");
}

} // namespace

int main(int argc, char **argv)
{
	if (argc == 2 && std::strcmp(argv[1], "--seal") == 0) {
		std::string in;
		for (int ch; (ch = std::getchar()) != EOF;)
			in += static_cast<char>(ch);
		while (!in.empty() && (in.back() == '\n' || in.back() == '\r'))
			in.pop_back();
		std::printf("%s\n", secret::seal(in).c_str());
		return 0;
	}
	if (argc == 6 && std::strcmp(argv[1], "--kat") == 0) {
		std::vector<uint8_t> ct;
		const Tag128 tag = gcm_seal(arr<32>(hex(argv[2])), arr<12>(hex(argv[3])), hex(argv[4]), hex(argv[5]), ct);
		std::printf("%s%s\n", to_hex(ct.data(), ct.size()).c_str(), to_hex(tag.data(), 16).c_str());
		return 0;
	}

	gcm_vectors();
	gcm_tamper();
	base64_vectors();
	secret_store();
	std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
