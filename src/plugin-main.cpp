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
#include "chat_source.hpp"
#include "controller.hpp"
#include "dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <curl/curl.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
	// Reference-counted; OBS's own curl users make this a no-op in practice,
	// but a plugin must not assume that.
	curl_global_init(CURL_GLOBAL_DEFAULT);
	chiripy::chat_source::register_source(); // "Chiripy Chat" in Add Source
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

// Frontend and every other module are up here; safe to touch the network
// and to create Qt widgets.
void obs_module_post_load(void)
{
	chiripy::controller::init();
	chiripy::register_dock();
}

void obs_module_unload(void)
{
	chiripy::controller::shutdown(); // joins the worker
	chiripy::chat_source::shutdown();
	curl_global_cleanup();
	obs_log(LOG_INFO, "plugin unloaded");
}
