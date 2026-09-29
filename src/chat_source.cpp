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

#include "diagnostics.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/bmem.h>

#include <cmath>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

namespace chiripy::chat_source {

namespace {

constexpr const char *kSourceId = "chiripy_youtube_chat";
constexpr const char *kEventName = "chiripy";
constexpr size_t kHistory = 500; // ~100 KB; see CLAUDE.md "No memcached"

// The page reads its settings from the URL fragment at load, so the only
// thing a replay carries is recent history. One replay, once the page has
// certainly loaded (owner's call: no repeated pushes); messages that arrive
// earlier are dispatched live anyway, and the page dedupes by id.
constexpr float kReplayAt[] = {10.0f};

struct Instance {
	obs_source_t *self = nullptr;
	obs_source_t *child = nullptr; // private browser_source
	uint32_t width = 800;
	uint32_t height = 400;
	std::string config_json;  // last pushed settings, replayed with history
	float since_show = -1.0f; // seconds since create/show; <0 = replays done
	int replays_sent = 0;

	// Handle-drag resize: the scene's item_transform signal arms this;
	// once no further transform has arrived for kSettleSec (drag released)
	// the scaled size becomes the real page size. Idle cost: one branch
	// per frame.
	bool handle_resize = true;
	bool resize_armed = false;
	float resize_settle = 0.0f;
	obs_sceneitem_t *resize_item = nullptr; // ref held while armed
};

constexpr float kSettleSec = 0.5f;

std::mutex mtx;
std::vector<Instance *> instances;              // every live wrapper
std::vector<obs_weak_source_t *> hooked_scenes; // scenes we listen to
std::deque<std::string> history;                // JSON of recent message events
std::string overlay_path;

std::string json_of(obs_data_t *d)
{
	const char *s = obs_data_get_json(d);
	return s ? s : "";
}

// RFC 3986 percent-encoding for the URL fragment.
std::string percent_encode(const std::string &in)
{
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	out.reserve(in.size() * 3);
	for (const unsigned char c : in) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

// file:///.../overlay.html#cfg=<settings> -- the page reads its settings from
// the fragment at load, so the first paint does not depend on an event that
// may arrive before CEF has started (which it did, on a slow machine).
std::string page_url(const std::string &config_json)
{
	return "file://" + overlay_path + "#cfg=" + percent_encode(config_json);
}

void set_child_url(Instance *inst)
{
	if (!inst->child)
		return;
	obs_data_t *cs = obs_data_create();
	obs_data_set_string(cs, "url", page_url(inst->config_json).c_str());
	obs_source_update(inst->child, cs);
	obs_data_release(cs);
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

// OBS colour properties are 0xAABBGGRR integers; the page wants #rrggbb.
std::string css_color(long long abgr)
{
	char buf[8];
	snprintf(buf, sizeof buf, "#%02x%02x%02x", static_cast<unsigned>(abgr & 0xff),
		 static_cast<unsigned>((abgr >> 8) & 0xff), static_cast<unsigned>((abgr >> 16) & 0xff));
	return buf;
}

std::string config_json_from(obs_data_t *settings)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "kind", "config");
	obs_data_set_int(d, "rows", obs_data_get_int(settings, "rows"));
	obs_data_set_int(d, "fontPx", obs_data_get_int(settings, "font_px"));
	obs_data_set_double(d, "opacity", obs_data_get_double(settings, "opacity"));
	obs_data_set_bool(d, "background", obs_data_get_bool(settings, "background"));
	obs_data_set_int(d, "shadowPx", obs_data_get_int(settings, "shadow_px"));
	obs_data_set_string(d, "greeting", obs_data_get_string(settings, "greeting"));
	obs_data_t *colors = obs_data_create();
	for (const char *key : {"owner", "mod", "member", "user", "text"})
		obs_data_set_string(
			colors, key,
			css_color(obs_data_get_int(settings, (std::string("color_") + key).c_str())).c_str());
	obs_data_set_obj(d, "colors", colors);
	obs_data_release(colors);
	const std::string out = json_of(d);
	obs_data_release(d);
	return out;
}

// ---- obs_source_info callbacks ----

void show(void *data);

// obs-browser does not repaint after a size change (black bars until the
// page reloads), so a resize reloads the child; the timed replays then fill
// the panel again. Look-only changes are just a config event.
void reload_child(Instance *inst)
{
	if (!inst->child)
		return;
	set_child_url(inst); // a fragment-only change does not reload by itself
	// obs-browser exposes reload only as its "refreshnocache" properties
	// button (its one proc is javascript_event), so press the button.
	obs_properties_t *props = obs_source_properties(inst->child);
	if (obs_property_t *button = obs_properties_get(props, "refreshnocache"))
		obs_property_button_clicked(button, inst->child);
	else
		obs_log(LOG_WARNING, "browser source has no refreshnocache button; cannot reload the overlay page");
	obs_properties_destroy(props);
	show(inst);
}

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
	obs_data_set_default_int(settings, "shadow_px", 2);
	obs_data_set_default_string(settings, "greeting", "Welcome to live chat!");
	obs_data_set_default_bool(settings, "handle_resize", true);
	// 0xAABBGGRR: pale classic-game palette (owner's picks, 2026-09-29), same as
	// the CSS defaults in overlay.html
	obs_data_set_default_int(settings, "color_owner", 0xff80e6ff);  // #ffe680
	obs_data_set_default_int(settings, "color_mod", 0xfffffc90);    // #90fcff
	obs_data_set_default_int(settings, "color_member", 0xff9affa0); // #a0ff9a
	obs_data_set_default_int(settings, "color_user", 0xffffc4ff);   // #ffc4ff
	obs_data_set_default_int(settings, "color_text", 0xfff0f0f0);   // #f0f0f0
}

void update(void *data, obs_data_t *settings)
{
	auto *inst = static_cast<Instance *>(data);
	const auto w = static_cast<uint32_t>(obs_data_get_int(settings, "width"));
	const auto h = static_cast<uint32_t>(obs_data_get_int(settings, "height"));
	const bool resized = inst->child && (w != inst->width || h != inst->height);
	inst->width = w;
	inst->height = h;
	inst->handle_resize = obs_data_get_bool(settings, "handle_resize");

	// Size belongs to the browser child; the look goes to the page.
	obs_data_t *cs = obs_data_create();
	obs_data_set_int(cs, "width", inst->width);
	obs_data_set_int(cs, "height", inst->height);
	obs_source_update(inst->child, cs);
	obs_data_release(cs);

	{
		std::lock_guard<std::mutex> lock(mtx);
		inst->config_json = config_json_from(settings);
		dispatch_to(inst->child, inst->config_json);
	}
	if (resized)
		reload_child(inst);
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *inst = new Instance;
	inst->self = source;

	inst->config_json = config_json_from(settings);
	obs_data_t *cs = obs_data_create();
	obs_data_set_bool(cs, "is_local_file", false);
	obs_data_set_string(cs, "url", page_url(inst->config_json).c_str());
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

	inst->width = static_cast<uint32_t>(obs_data_get_int(settings, "width"));
	inst->height = static_cast<uint32_t>(obs_data_get_int(settings, "height"));
	{
		std::lock_guard<std::mutex> lock(mtx);
		instances.push_back(inst);
	}
	update(inst, settings); // same size as the child was created with: no reload
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
	if (inst->resize_item)
		obs_sceneitem_release(inst->resize_item);
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

// OBS handles only scale the picture; a browser page stretched that way
// goes soft and does not re-wrap. When the drag has settled, the scaled
// size becomes the real page size and the item goes back to 1:1.
void apply_handle_resize(Instance *inst)
{
	obs_sceneitem_t *item = inst->resize_item;
	inst->resize_item = nullptr;
	if (!item)
		return;
	vec2 scale;
	obs_sceneitem_get_scale(item, &scale);
	const bool scaled = std::fabs(scale.x - 1.0f) > 0.005f || std::fabs(scale.y - 1.0f) > 0.005f;
	if (scaled && obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE) {
		const auto w = static_cast<uint32_t>(std::lround(inst->width * scale.x));
		const auto h = static_cast<uint32_t>(std::lround(inst->height * scale.y));
		if (w >= 100 && h >= 50) {
			vec2 one = {1.0f, 1.0f};
			obs_sceneitem_set_scale(item, &one);
			obs_data_t *settings = obs_source_get_settings(inst->self);
			obs_data_set_int(settings, "width", w);
			obs_data_set_int(settings, "height", h);
			obs_source_update(inst->self, settings); // -> update(): child resize + reload
			obs_data_release(settings);
		}
	}
	obs_sceneitem_release(item);
}

// Scene signal "item_transform": fires for every transform change, i.e.
// continuously during a handle drag. Arm (or re-arm) the settle timer.
void on_item_transform_impl(calldata_t *cd)
{
	auto *item = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
	if (!item)
		return;
	obs_source_t *src = obs_sceneitem_get_source(item);
	std::lock_guard<std::mutex> lock(mtx);
	for (Instance *inst : instances) {
		if (inst->self != src || !inst->handle_resize)
			continue;
		if (inst->resize_item != item) {
			if (inst->resize_item)
				obs_sceneitem_release(inst->resize_item);
			obs_sceneitem_addref(item);
			inst->resize_item = item;
		}
		inst->resize_armed = true;
		inst->resize_settle = 0.0f;
	}
}

void on_item_transform(void *, calldata_t *cd)
{
	diag::guard("scene item transform", [cd] { on_item_transform_impl(cd); });
}

// Graphics thread, once per frame. Idle cost is two branches: the replay
// countdown until the single replay has gone out, and the resize settle
// timer while a drag is in progress.
void video_tick(void *data, float seconds)
{
	auto *inst = static_cast<Instance *>(data);
	if (inst->resize_armed) {
		inst->resize_settle += seconds;
		if (inst->resize_settle >= kSettleSec) {
			inst->resize_armed = false;
			apply_handle_resize(inst);
		}
	}
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
	diag::guard("reload button", [data] { reload_child(static_cast<Instance *>(data)); });
	return false;
}

obs_properties_t *get_properties(void *data)
{
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_int(p, "width", obs_module_text("Chiripy.Source.Width"), 100, 4096, 10);
	obs_properties_add_int(p, "height", obs_module_text("Chiripy.Source.Height"), 50, 4096, 10);
	obs_properties_add_text(p, "greeting", obs_module_text("Chiripy.Source.Greeting"), OBS_TEXT_DEFAULT);
	obs_properties_add_int_slider(p, "rows", obs_module_text("Chiripy.Source.Rows"), 0, 40, 1);
	obs_properties_add_int_slider(p, "font_px", obs_module_text("Chiripy.Source.FontPx"), 8, 96, 1);
	obs_properties_add_float_slider(p, "opacity", obs_module_text("Chiripy.Source.Opacity"), 0.0, 1.0, 0.05);
	obs_properties_add_bool(p, "background", obs_module_text("Chiripy.Source.Background"));
	obs_properties_add_bool(p, "handle_resize", obs_module_text("Chiripy.Source.HandleResize"));
	obs_properties_add_int_slider(p, "shadow_px", obs_module_text("Chiripy.Source.ShadowPx"), 0, 8, 1);
	obs_properties_add_color(p, "color_owner", obs_module_text("Chiripy.Source.ColorOwner"));
	obs_properties_add_color(p, "color_mod", obs_module_text("Chiripy.Source.ColorMod"));
	obs_properties_add_color(p, "color_member", obs_module_text("Chiripy.Source.ColorMember"));
	obs_properties_add_color(p, "color_user", obs_module_text("Chiripy.Source.ColorUser"));
	obs_properties_add_color(p, "color_text", obs_module_text("Chiripy.Source.ColorText"));
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

	// OBS calls these from C: every callback with non-trivial code runs
	// behind an exception barrier (see diagnostics.hpp).
	static obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = get_name;
	info.create = [](obs_data_t *s, obs_source_t *src) -> void * {
		void *r = nullptr;
		diag::guard("source create", [&] { r = create(s, src); });
		return r;
	};
	info.destroy = [](void *d) {
		diag::guard("source destroy", [d] { destroy(d); });
	};
	info.update = [](void *d, obs_data_t *s) {
		diag::guard("source update", [d, s] { update(d, s); });
	};
	info.get_defaults = [](obs_data_t *s) {
		diag::guard("source defaults", [s] { get_defaults(s); });
	};
	info.get_properties = [](void *d) -> obs_properties_t * {
		obs_properties_t *p = nullptr;
		diag::guard("source properties", [&] { p = get_properties(d); });
		return p;
	};
	info.get_width = get_width;
	info.get_height = get_height;
	info.video_render = video_render;
	info.video_tick = [](void *d, float t) {
		diag::guard("source tick", [d, t] { video_tick(d, t); });
	};
	info.enum_active_sources = enum_sources;
	info.enum_all_sources = enum_sources;
	info.show = [](void *d) {
		diag::guard("source show", [d] { show(d); });
	};
	info.icon_type = OBS_ICON_TYPE_BROWSER;
	obs_register_source(&info);
}

void rehook_scenes()
{
	for (obs_weak_source_t *weak : hooked_scenes) {
		if (obs_source_t *scene = obs_weak_source_get_source(weak)) {
			signal_handler_disconnect(obs_source_get_signal_handler(scene), "item_transform",
						  on_item_transform, nullptr);
			obs_source_release(scene);
		}
		obs_weak_source_release(weak);
	}
	hooked_scenes.clear();

	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		obs_source_t *scene = scenes.sources.array[i];
		signal_handler_connect(obs_source_get_signal_handler(scene), "item_transform", on_item_transform,
				       nullptr);
		hooked_scenes.push_back(obs_source_get_weak_source(scene));
	}
	obs_frontend_source_list_free(&scenes);
}

void shutdown()
{
	for (obs_weak_source_t *weak : hooked_scenes) {
		if (obs_source_t *scene = obs_weak_source_get_source(weak)) {
			signal_handler_disconnect(obs_source_get_signal_handler(scene), "item_transform",
						  on_item_transform, nullptr);
			obs_source_release(scene);
		}
		obs_weak_source_release(weak);
	}
	hooked_scenes.clear();
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
				    m.is_owner       ? "owner"
				    : m.is_moderator ? "mod"
				    : m.is_member    ? "member"
						     : "user");
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
