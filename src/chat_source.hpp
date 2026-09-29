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

#include "youtube.hpp"

#include <string>

// "YouTube Chat (Chiripy)" in OBS's Add Source list.
//
// The source type is a thin wrapper: each instance owns a private
// browser_source pointing at data/overlay.html and delegates rendering to it
// (obs_source_video_render on the child), so CEF still draws everything --
// the plugin never lays out text. What the wrapper owns is the properties
// dialog (size, rows, font size, opacity, background) and the delivery of
// chat events to its child through obs-browser's javascript_event proc.
//
// A ring buffer of recent messages is replayed to a child shortly after it
// is created or shown, so an overlay added mid-stream is not blank; the page
// dedupes by message id.
//
// message/status/clear are safe to call from the stream's worker thread.
namespace chiripy::chat_source {

void register_source(); // call from obs_module_load
void shutdown();        // call from obs_module_unload

void message(const youtube::ChatMessage &m);
void status(const std::string &text);
void clear();

int instance_count();

} // namespace chiripy::chat_source
