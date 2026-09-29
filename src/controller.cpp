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

#include "controller.hpp"

#include "chat_source.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>
#include <plugin-support.h>

#include <memory>
#include <mutex>

namespace chiripy::controller {

namespace {

std::unique_ptr<youtube::ChatStream> stream;
config::Settings current;
Listener listener;
std::string status_text;
youtube::State state_now = youtube::State::Idle;
std::mutex mtx; // guards status_text/state_now, written from the worker
bool frontend_hooked = false;

// Runs fn on the OBS UI thread (Qt lives there); safe from any thread.
void on_ui(std::function<void()> fn)
{
	auto *boxed = new std::function<void()>(std::move(fn));
	obs_queue_task(
		OBS_TASK_UI,
		[](void *p) {
			auto *f = static_cast<std::function<void()> *>(p);
			(*f)();
			delete f;
		},
		boxed, false);
}

void notify()
{
	youtube::State s;
	std::string text;
	{
		std::lock_guard<std::mutex> lock(mtx);
		s = state_now;
		text = status_text;
	}
	if (listener)
		listener(s, text);
}

// Worker-thread callbacks.
void on_message(const youtube::ChatMessage &m)
{
	chat_source::message(m);
	if (m.type == "textMessageEvent") {
		const char *badge = m.is_owner       ? "[owner] "
				    : m.is_moderator ? "[mod] "
				    : m.is_member    ? "[member] "
						     : "";
		obs_log(LOG_INFO, "%s%s: %s", badge, m.author.c_str(), m.text.c_str());
	} else if (m.type == "messageDeletedEvent") {
		obs_log(LOG_INFO, "(message %s deleted)", m.deleted_message_id.c_str());
	} else {
		obs_log(LOG_INFO, "%s from %s: %s", m.type.c_str(), m.author.c_str(), m.text.c_str());
	}
}

void on_status(const std::string &text)
{
	obs_log(LOG_INFO, "%s", text.c_str());
	chat_source::status(text);
	{
		std::lock_guard<std::mutex> lock(mtx);
		status_text = text;
	}
	on_ui(notify);
}

void on_state(youtube::State s)
{
	if (s == youtube::State::Connected)
		chat_source::status(""); // clears the panel's status line
	{
		std::lock_guard<std::mutex> lock(mtx);
		state_now = s;
		if (s == youtube::State::Connected)
			status_text = "Connected to the live chat.";
		else if (s == youtube::State::Connecting)
			status_text = "Connecting...";
		else if (s == youtube::State::Idle)
			status_text = "Not connected.";
	}
	on_ui(notify);
}

void connect_now()
{
	if (current.api_key.empty() || current.video_id.empty()) {
		std::lock_guard<std::mutex> lock(mtx);
		status_text = current.api_key.empty() ? "Enter your YouTube API key in the Chiripy dock."
						      : "Enter the video ID of your live stream in the Chiripy dock.";
		state_now = youtube::State::Idle;
		notify();
		return;
	}
	obs_log(LOG_INFO, "connecting to the chat of video %s", current.video_id.c_str());
	if (!stream)
		stream = std::make_unique<youtube::ChatStream>();
	stream->start(current.api_key, current.video_id, on_message, on_status, on_state);
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		// The video ID a streamer pasted earlier may only go live now;
		// the worker retries the lookup until it does.
		if (!stream || stream->state() == youtube::State::Idle || stream->state() == youtube::State::Stopped)
			connect_now();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		disconnect();
		break;
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		chat_source::rehook_scenes();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		// Stop the worker before Qt and libobs go away under it.
		if (stream)
			stream->stop();
		break;
	default:
		break;
	}
}

} // namespace

void init()
{
	current = config::load();
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	frontend_hooked = true;
	{
		std::lock_guard<std::mutex> lock(mtx);
		status_text = "Not connected.";
	}
	// Only spend quota at launch if a stream is already running (OBS was
	// restarted mid-stream); otherwise wait for Streaming Started or the dock.
	if (obs_frontend_streaming_active())
		connect_now();
	else
		notify();
}

void shutdown()
{
	if (frontend_hooked) {
		obs_frontend_remove_event_callback(on_frontend_event, nullptr);
		frontend_hooked = false;
	}
	listener = nullptr;
	stream.reset();
}

void set_listener(Listener l)
{
	listener = std::move(l);
	notify();
}

config::Settings settings()
{
	return current;
}

youtube::State state()
{
	std::lock_guard<std::mutex> lock(mtx);
	return state_now;
}

std::string last_status()
{
	std::lock_guard<std::mutex> lock(mtx);
	return status_text;
}

void save_and_connect(const std::string &api_key, const std::string &video_id)
{
	if (!api_key.empty())
		current.api_key = api_key;
	current.video_id = youtube::extract_video_id(video_id);
	if (!config::save(current))
		obs_log(LOG_WARNING, "settings could not be saved; using them for this session only");
	connect_now();
}

void disconnect()
{
	if (stream)
		stream->stop();
	{
		std::lock_guard<std::mutex> lock(mtx);
		state_now = youtube::State::Idle;
		status_text = "Not connected.";
	}
	notify();
}

} // namespace chiripy::controller
