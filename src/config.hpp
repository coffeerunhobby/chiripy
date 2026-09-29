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

#include <string>

// The plugin's own config file, at obs_module_config_path("config.json"):
//   macOS   ~/Library/Application Support/obs-studio/plugin_config/chiripy/
//   Windows %APPDATA%\obs-studio\plugin_config\chiripy\
// Deliberately separate from scene collections and profiles, which users
// export and paste into bug reports. The API key is stored sealed (see
// secret_store.hpp); everything else is plain JSON.
namespace chiripy::config {

struct Settings {
	std::string api_key;  // in the clear in memory only
	std::string video_id; // session only: never persisted (stale by next launch)
	int rows = 8;
};

Settings load();
bool save(const Settings &settings);

} // namespace chiripy::config
