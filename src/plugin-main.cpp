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
#include "youtube.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <curl/curl.h>

#include <memory>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

std::unique_ptr<chiripy::youtube::ChatStream> stream;

// Milestone 1: every chat event goes to the OBS log. The browser-source
// dispatch replaces the body of this function in the next step.
void on_message(const chiripy::youtube::ChatMessage &m)
{
	if (m.type == "textMessageEvent") {
		const char *badge = m.is_owner ? "[owner] " : m.is_moderator ? "[mod] " : m.is_member ? "[member] " : "";
		obs_log(LOG_INFO, "%s%s: %s", badge, m.author.c_str(), m.text.c_str());
	} else if (m.type == "messageDeletedEvent") {
		obs_log(LOG_INFO, "(message %s deleted)", m.deleted_message_id.c_str());
	} else {
		obs_log(LOG_INFO, "%s from %s: %s", m.type.c_str(), m.author.c_str(), m.text.c_str());
	}
}

void on_status(const std::string &s)
{
	obs_log(LOG_INFO, "%s", s.c_str());
}

void start_from_config()
{
	stream.reset(); // joins the previous worker; it aborts within ~1 s
	const chiripy::config::Settings s = chiripy::config::load();
	if (s.api_key.empty() || s.video_id.empty()) {
		obs_log(LOG_INFO, "no API key or video ID configured; idle");
		return;
	}
	obs_log(LOG_INFO, "looking up live chat for video %s", s.video_id.c_str());
	const auto lookup = chiripy::youtube::resolve_chat_id(s.api_key, s.video_id);
	if (!lookup.error.empty()) {
		obs_log(LOG_WARNING, "%s", lookup.error.c_str());
		return;
	}
	obs_log(LOG_INFO, "connecting to live chat %s", lookup.chat_id.c_str());
	stream = std::make_unique<chiripy::youtube::ChatStream>();
	stream->start(s.api_key, lookup.chat_id, on_message, on_status);
}

} // namespace

bool obs_module_load(void)
{
	// Reference-counted; OBS's own curl users make this a no-op in practice,
	// but a plugin must not assume that.
	curl_global_init(CURL_GLOBAL_DEFAULT);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

// Frontend and every other module are up here; safe to touch the network.
void obs_module_post_load(void)
{
	// Until the dock exists, this is how a changed config.json (new video
	// ID) is picked up without restarting OBS -- which, with OBS-managed
	// YouTube broadcasts, would end the very stream being connected to.
	// Runs on the UI thread and blocks for one videos.list round trip.
	obs_frontend_add_tools_menu_item("Chiripy: Reconnect to chat", [](void *) { start_from_config(); }, nullptr);
	start_from_config();
}

void obs_module_unload(void)
{
	stream.reset(); // joins the worker
	curl_global_cleanup();
	obs_log(LOG_INFO, "plugin unloaded");
}
