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

#include "chat_source.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <vector>

namespace chiripy::chat_source {

namespace {

constexpr const char *kSourceId = "chiripy_youtube_chat";
constexpr const char *kEventName = "chiripy";
constexpr size_t kHistory = 500; // ~100 KB; see CLAUDE.md "No memcached"

// The page needs a moment to load before it can receive events. Replays are
// idempotent (config, and messages deduped by id), so send twice to cover
// slow and fast machines rather than guess one delay.
constexpr float kReplayAt[] = {1.0f, 3.0f};

struct Instance {
	obs_source_t *self = nullptr;
	obs_source_t *child = nullptr; // private browser_source
	uint32_t width = 800;
	uint32_t height = 400;
	std::string config_json;  // last pushed settings, replayed with history
	float since_show = -1.0f; // seconds since create/show; <0 = replays done
	int replays_sent = 0;
};

std::mutex mtx;
std::vector<Instance *> instances;   // every live wrapper
std::deque<std::string> history;     // JSON of recent message events
std::string overlay_path;

std::string json_of(obs_data_t *d)
{
	const char *s = obs_data_get_json(d);
	return s ? s : "";
}

void dispatch_to(obs_source_t *child, const std::string &json)
{
	if (!child)
		return;
	proc_handler_t *ph = obs_source_get_proc_handler(child);
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "eventName", kEventName);
	calldata_set_string(&cd, "jsonString", json.c_str());
	proc_handler_call(ph, "javascript_event", &cd);
	calldata_free(&cd);
}

// Caller holds mtx.
void replay_locked(Instance *inst)
{
	dispatch_to(inst->child, inst->config_json);
	if (history.empty())
		return;
	std::string snapshot = "{\"kind\":\"snapshot\",\"messages\":[";
	for (size_t i = 0; i < history.size(); ++i) {
		if (i)
			snapshot += ',';
		snapshot += history[i]; // each element is a complete message object
	}
	snapshot += "]}";
	dispatch_to(inst->child, snapshot);
}

void broadcast(const std::string &json)
{
	std::lock_guard<std::mutex> lock(mtx);
	for (Instance *inst : instances)
		dispatch_to(inst->child, json);
}

std::string config_json_from(obs_data_t *settings)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "kind", "config");
	obs_data_set_int(d, "rows", obs_data_get_int(settings, "rows"));
	obs_data_set_int(d, "fontPx", obs_data_get_int(settings, "font_px"));
	obs_data_set_double(d, "opacity", obs_data_get_double(settings, "opacity"));
	obs_data_set_bool(d, "background", obs_data_get_bool(settings, "background"));
	const std::string out = json_of(d);
	obs_data_release(d);
	return out;
}

// ---- obs_source_info callbacks ----

const char *get_name(void *)
{
	return obs_module_text("Chiripy.Source.Name");
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 800);
	obs_data_set_default_int(settings, "height", 400);
	obs_data_set_default_int(settings, "rows", 8);
	obs_data_set_default_int(settings, "font_px", 20);
	obs_data_set_default_double(settings, "opacity", 0.55);
	obs_data_set_default_bool(settings, "background", true);
}

void update(void *data, obs_data_t *settings)
{
	auto *inst = static_cast<Instance *>(data);
	inst->width = static_cast<uint32_t>(obs_data_get_int(settings, "width"));
	inst->height = static_cast<uint32_t>(obs_data_get_int(settings, "height"));

	// Size belongs to the browser child; the look goes to the page.
	obs_data_t *cs = obs_data_create();
	obs_data_set_int(cs, "width", inst->width);
	obs_data_set_int(cs, "height", inst->height);
	obs_source_update(inst->child, cs);
	obs_data_release(cs);

	std::lock_guard<std::mutex> lock(mtx);
	inst->config_json = config_json_from(settings);
	dispatch_to(inst->child, inst->config_json);
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *inst = new Instance;
	inst->self = source;

	obs_data_t *cs = obs_data_create();
	obs_data_set_bool(cs, "is_local_file", true);
	obs_data_set_string(cs, "local_file", overlay_path.c_str());
	obs_data_set_int(cs, "width", obs_data_get_int(settings, "width"));
	obs_data_set_int(cs, "height", obs_data_get_int(settings, "height"));
	// Keep the page alive across scene switches: a reload would lose the
	// panel until the next replay.
	obs_data_set_bool(cs, "shutdown", false);
	// Private: never listed in the UI, lives and dies with the wrapper.
	inst->child = obs_source_create_private("browser_source", "chiripy-overlay", cs);
	obs_data_release(cs);
	if (!inst->child)
		obs_log(LOG_ERROR, "could not create the browser source; is obs-browser loaded?");

	{
		std::lock_guard<std::mutex> lock(mtx);
		instances.push_back(inst);
	}
	update(inst, settings);
	inst->since_show = 0.0f;
	return inst;
}

void destroy(void *data)
{
	auto *inst = static_cast<Instance *>(data);
	{
		std::lock_guard<std::mutex> lock(mtx);
		instances.erase(std::remove(instances.begin(), instances.end(), inst), instances.end());
	}
	obs_source_release(inst->child);
	delete inst;
}

uint32_t get_width(void *data)
{
	return static_cast<Instance *>(data)->width;
}

uint32_t get_height(void *data)
{
	return static_cast<Instance *>(data)->height;
}

void video_render(void *data, gs_effect_t *)
{
	auto *inst = static_cast<Instance *>(data);
	if (inst->child)
		obs_source_video_render(inst->child);
}

// Reporting the child here lets libobs propagate active/showing state to it
// exactly as it does for items in a scene, which is what obs-browser keys
// its own lifecycle on.
void enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	auto *inst = static_cast<Instance *>(data);
	if (inst->child)
		cb(inst->self, inst->child, param);
}

void show(void *data)
{
	auto *inst = static_cast<Instance *>(data);
	inst->since_show = 0.0f;
	inst->replays_sent = 0;
}

// Graphics thread, once per frame: drives the delayed replays.
void video_tick(void *data, float seconds)
{
	auto *inst = static_cast<Instance *>(data);
	if (inst->since_show < 0.0f)
		return;
	inst->since_show += seconds;
	if (inst->replays_sent < static_cast<int>(sizeof(kReplayAt) / sizeof(kReplayAt[0])) &&
	    inst->since_show >= kReplayAt[inst->replays_sent]) {
		std::lock_guard<std::mutex> lock(mtx);
		replay_locked(inst);
		++inst->replays_sent;
	}
	if (inst->replays_sent >= static_cast<int>(sizeof(kReplayAt) / sizeof(kReplayAt[0])))
		inst->since_show = -1.0f;
}

bool refresh_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *inst = static_cast<Instance *>(data);
	if (inst->child) {
		proc_handler_t *ph = obs_source_get_proc_handler(inst->child);
		calldata_t cd;
		calldata_init(&cd);
		proc_handler_call(ph, "refreshnocache", &cd);
		calldata_free(&cd);
	}
	show(inst); // schedule replays for the reloaded page
	return false;
}

obs_properties_t *get_properties(void *data)
{
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_int(p, "width", obs_module_text("Chiripy.Source.Width"), 100, 4096, 10);
	obs_properties_add_int(p, "height", obs_module_text("Chiripy.Source.Height"), 50, 4096, 10);
	obs_properties_add_int_slider(p, "rows", obs_module_text("Chiripy.Source.Rows"), 1, 40, 1);
	obs_properties_add_int_slider(p, "font_px", obs_module_text("Chiripy.Source.FontPx"), 8, 96, 1);
	obs_properties_add_float_slider(p, "opacity", obs_module_text("Chiripy.Source.Opacity"), 0.0, 1.0, 0.05);
	obs_properties_add_bool(p, "background", obs_module_text("Chiripy.Source.Background"));
	obs_properties_add_button2(p, "refresh", obs_module_text("Chiripy.Source.Refresh"), refresh_clicked, data);
	return p;
}

} // namespace

void register_source()
{
	char *path = obs_module_file("overlay.html");
	overlay_path = path ? path : "";
	bfree(path);
	if (overlay_path.empty())
		obs_log(LOG_ERROR, "overlay.html is missing from the plugin bundle");

	static obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = get_name;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.get_width = get_width;
	info.get_height = get_height;
	info.video_render = video_render;
	info.video_tick = video_tick;
	info.enum_active_sources = enum_sources;
	info.enum_all_sources = enum_sources;
	info.show = show;
	info.icon_type = OBS_ICON_TYPE_BROWSER;
	obs_register_source(&info);
}

void shutdown()
{
	std::lock_guard<std::mutex> lock(mtx);
	history.clear();
}

void message(const youtube::ChatMessage &m)
{
	obs_data_t *d = obs_data_create();
	if (m.type == "messageDeletedEvent") {
		obs_data_set_string(d, "kind", "delete");
		obs_data_set_string(d, "id", m.deleted_message_id.c_str());
	} else if (m.type == "userBannedEvent") {
		obs_data_set_string(d, "kind", "ban");
		obs_data_set_string(d, "channel", m.banned_channel_id.c_str());
	} else if (m.type == "textMessageEvent" || m.type == "superChatEvent" || m.type == "superStickerEvent" ||
		   m.type == "newSponsorEvent" || m.type == "memberMilestoneChatEvent") {
		obs_data_set_string(d, "kind", "message");
		obs_data_set_string(d, "id", m.id.c_str());
		obs_data_set_string(d, "type", m.type.c_str());
		obs_data_set_string(d, "author", m.author.c_str());
		obs_data_set_string(d, "channel", m.author_channel_id.c_str());
		obs_data_set_string(d, "text", m.text.c_str());
		obs_data_set_string(d, "role",
				    m.is_owner ? "owner" : m.is_moderator ? "mod" : m.is_member ? "member" : "user");
		if (!m.amount.empty())
			obs_data_set_string(d, "amount", m.amount.c_str());
	} else {
		obs_data_release(d);
		return; // tombstones, polls, sponsor-only toggles: nothing to draw
	}
	const std::string json = json_of(d);
	obs_data_release(d);

	{
		std::lock_guard<std::mutex> lock(mtx);
		if (m.type == "messageDeletedEvent") {
			// Deleted messages leave the replay buffer too, or a page that
			// loads later would show them again.
			const std::string needle = "\"id\":\"" + m.deleted_message_id + "\"";
			for (auto it = history.begin(); it != history.end(); ++it)
				if (it->find(needle) != std::string::npos) {
					history.erase(it);
					break;
				}
		} else if (m.type != "userBannedEvent") {
			history.push_back(json);
			if (history.size() > kHistory)
				history.pop_front();
		}
		for (Instance *inst : instances)
			dispatch_to(inst->child, json);
	}
}

void status(const std::string &text)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "kind", "status");
	obs_data_set_string(d, "text", text.c_str());
	const std::string json = json_of(d);
	obs_data_release(d);
	broadcast(json);
}

void clear()
{
	{
		std::lock_guard<std::mutex> lock(mtx);
		history.clear();
	}
	broadcast("{\"kind\":\"clear\"}");
}

int instance_count()
{
	std::lock_guard<std::mutex> lock(mtx);
	return static_cast<int>(instances.size());
}

} // namespace chiripy::chat_source
