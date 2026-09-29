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

#include <atomic>
#include <functional>
#include <string>
#include <thread>

// YouTube Data API v3 client for one live chat: chat-ID lookup plus the
// streamList reconnect loop. See CLAUDE.md "Transport" for the measured
// behaviour this is built around (10.4 s server cap, pageToken resume,
// 5 quota units per connection).
namespace chiripy::youtube {

struct ChatMessage {
	std::string id;
	std::string type; // textMessageEvent, superChatEvent, messageDeletedEvent, ...
	std::string author;
	std::string author_channel_id;
	std::string text; // displayMessage
	std::string published_at;
	bool is_owner = false;
	bool is_moderator = false;
	bool is_member = false; // isChatSponsor
	bool is_verified = false;
	std::string deleted_message_id; // for messageDeletedEvent
};

struct ChatLookup {
	std::string chat_id;
	std::string error; // empty on success; otherwise a sentence for the user
};

// Blocking. One videos.list call (1 quota unit).
ChatLookup resolve_chat_id(const std::string &api_key, const std::string &video_id);

class ChatStream {
public:
	using MessageHandler = std::function<void(const ChatMessage &)>;
	// Human-readable state changes: connected, ended, errors. Also the
	// support channel, per CLAUDE.md, so keep these sentences plain.
	using StatusHandler = std::function<void(const std::string &)>;

	ChatStream() = default;
	~ChatStream() { stop(); }
	ChatStream(const ChatStream &) = delete;
	ChatStream &operator=(const ChatStream &) = delete;

	// Handlers run on the worker thread.
	void start(std::string api_key, std::string chat_id, MessageHandler on_message, StatusHandler on_status);
	void stop(); // blocks until the worker has exited (bounded by curl's abort check)
	bool running() const { return running_.load(); }

private:
	void run();
	// One HTTPS connection. Returns the delay in seconds before the next
	// attempt, or a negative value to stop for good.
	double connect_once();

	std::string api_key_;
	std::string chat_id_;
	std::string page_token_;
	MessageHandler on_message_;
	StatusHandler on_status_;
	std::thread worker_;
	std::atomic<bool> stop_requested_{false};
	std::atomic<bool> running_{false};
	int backoff_ = 0; // consecutive failures, drives exponential backoff
};

} // namespace chiripy::youtube
