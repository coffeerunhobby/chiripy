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

#include "config.hpp"

#include "secret_store.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>

namespace chiripy::config {

namespace {

constexpr const char *kFile = "config.json";

std::string config_file_path()
{
	char *path = obs_module_config_path(kFile);
	std::string result = path ? path : "";
	bfree(path);
	return result;
}

} // namespace

Settings load()
{
	Settings s;
	const std::string path = config_file_path();
	if (path.empty())
		return s;

	obs_data_t *data = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	if (!data)
		return s; // first run: defaults

	if (const char *sealed = obs_data_get_string(data, "api_key_sealed"); sealed && *sealed) {
		if (auto key = chiripy::secret::unseal(sealed))
			s.api_key = *key;
		else
			// Tampered or corrupted: treat as unset rather than sending
			// garbage to YouTube. The dock will ask for the key again.
			obs_log(LOG_WARNING, "stored API key failed to unseal; ignoring it");
	}
	s.video_id = obs_data_get_string(data, "video_id");
	obs_data_set_default_int(data, "rows", s.rows);
	s.rows = static_cast<int>(obs_data_get_int(data, "rows"));

	obs_data_release(data);
	return s;
}

bool save(const Settings &s)
{
	const std::string path = config_file_path();
	if (path.empty())
		return false;

	// obs_module_config_path only builds the string; the directory is ours
	// to create.
	char *dir = obs_module_config_path("");
	const bool dir_ok = dir && os_mkdirs(dir) != MKDIR_ERROR;
	bfree(dir);
	if (!dir_ok) {
		obs_log(LOG_ERROR, "cannot create config directory for %s", path.c_str());
		return false;
	}

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "api_key_sealed", s.api_key.empty() ? "" : chiripy::secret::seal(s.api_key).c_str());
	obs_data_set_string(data, "video_id", s.video_id.c_str());
	obs_data_set_int(data, "rows", s.rows);
	const bool ok = obs_data_save_json_safe(data, path.c_str(), "tmp", "bak");
	obs_data_release(data);
	if (!ok)
		obs_log(LOG_ERROR, "failed to write %s", path.c_str());
	return ok;
}

} // namespace chiripy::config
