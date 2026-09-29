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

#include "base64.hpp"

namespace chiripy::base64 {

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int value_of(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}
} // namespace

std::string encode(const std::vector<uint8_t> &data)
{
	std::string out;
	out.reserve((data.size() + 2) / 3 * 4);
	size_t i = 0;
	for (; i + 3 <= data.size(); i += 3) {
		const uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
		out += kAlphabet[(n >> 18) & 63];
		out += kAlphabet[(n >> 12) & 63];
		out += kAlphabet[(n >> 6) & 63];
		out += kAlphabet[n & 63];
	}
	const size_t rem = data.size() - i;
	if (rem == 1) {
		const uint32_t n = data[i] << 16;
		out += kAlphabet[(n >> 18) & 63];
		out += kAlphabet[(n >> 12) & 63];
		out += "==";
	} else if (rem == 2) {
		const uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
		out += kAlphabet[(n >> 18) & 63];
		out += kAlphabet[(n >> 12) & 63];
		out += kAlphabet[(n >> 6) & 63];
		out += '=';
	}
	return out;
}

std::optional<std::vector<uint8_t>> decode(const std::string &text)
{
	if (text.size() % 4 != 0)
		return std::nullopt;
	std::vector<uint8_t> out;
	out.reserve(text.size() / 4 * 3);
	for (size_t i = 0; i < text.size(); i += 4) {
		int v[4];
		int pad = 0;
		for (int k = 0; k < 4; ++k) {
			const char c = text[i + k];
			if (c == '=') {
				// Padding is only legal in the last group, last two slots.
				if (i + 4 != text.size() || k < 2)
					return std::nullopt;
				v[k] = 0;
				++pad;
			} else {
				if (pad)
					return std::nullopt; // data after padding
				v[k] = value_of(c);
				if (v[k] < 0)
					return std::nullopt;
			}
		}
		const uint32_t n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
		out.push_back(static_cast<uint8_t>((n >> 16) & 0xff));
		if (pad < 2)
			out.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
		if (pad < 1)
			out.push_back(static_cast<uint8_t>(n & 0xff));
	}
	return out;
}

} // namespace chiripy::base64
