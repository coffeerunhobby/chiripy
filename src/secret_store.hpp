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

#include <optional>
#include <string>

// Seals a short secret (the YouTube API key) for storage in the plugin's
// config file, so the key is never on disk in the clear and a pasted config
// does not leak it at a glance.
//
// Honest scope: the AES-256-GCM key is baked into this binary (the owner's
// decision: no dependency on OS keystores). That makes this obfuscation
// against casual disclosure and tampering, not protection against anyone who
// has this plugin and the config file. The value in this design is the
// authentication tag: a hand-edited or corrupted value fails to unseal instead
// of yielding a garbage key that then 400s at YouTube.
//
// Wire format: "v1:" + base64(nonce[12] || ciphertext || tag[16]). A fresh
// random nonce is drawn per seal; GCM must never see a nonce twice under one
// key, and there is no counter to persist, so randomness is the only safe
// choice here.
namespace chiripy::secret {

std::string seal(const std::string &plaintext);
std::optional<std::string> unseal(const std::string &sealed);

} // namespace chiripy::secret
