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

#include "config.hpp"
#include "youtube.hpp"

#include <functional>
#include <string>

// Ties the chat stream to OBS: connect when streaming starts (or the dock
// asks), disconnect when it stops, and keep the dock informed. Everything
// here runs on the UI thread except the worker callbacks, which are
// marshalled back with obs_queue_task(OBS_TASK_UI).
namespace chiripy::controller {

using Listener = std::function<void(youtube::State state, const std::string &status)>;

void init();     // obs_module_post_load
void shutdown(); // obs_module_unload

// The dock's view of things.
void set_listener(Listener listener);
config::Settings settings();
youtube::State state();
std::string last_status();

// From the dock. An empty api_key keeps the stored one.
void save_and_connect(const std::string &api_key, const std::string &video_id);
void disconnect();

} // namespace chiripy::controller
