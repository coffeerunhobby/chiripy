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

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// RFC 4648 base64 with padding. Decoding is strict: any character outside the
// alphabet, bad padding or a trailing partial group returns nullopt, so a
// hand-edited config value fails cleanly instead of yielding garbage bytes.
namespace chiripy::base64 {

std::string encode(const std::vector<uint8_t> &data);
std::optional<std::vector<uint8_t>> decode(const std::string &text);

} // namespace chiripy::base64
