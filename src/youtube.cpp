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

#include "youtube.hpp"

#include "json_splitter.hpp"

#include <obs-data.h>

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>

namespace chiripy::youtube {

namespace {

constexpr const char *kApiBase = "https://youtube.googleapis.com/youtube/v3/";
constexpr const char *kUserAgent = "chiripy/0.1 (OBS plugin)";
// The server closes every stream at ~10.4 s; anything beyond this is a hung
// connection, not a slow one.
constexpr long kStreamTimeoutSec = 60;
constexpr long kLookupTimeoutSec = 15;
// While OBS is streaming but YouTube has not marked the video live yet.
constexpr double kLookupRetrySec = 5.0;

std::atomic<long long> g_units{0};

struct CurlHandle {
	CURL *h = curl_easy_init();
	curl_slist *headers = nullptr;
	~CurlHandle()
	{
		curl_slist_free_all(headers);
		curl_easy_cleanup(h);
	}
};

// JSON parsing is libobs's obs_data (jansson): already in this process,
// already trusted by OBS for its own files. These RAII wrappers keep the C
// refcounting out of the logic below.
struct Data {
	obs_data_t *d = nullptr;
	explicit Data(obs_data_t *p) : d(p) {}
	static Data from_json(const std::string &text) { return Data(obs_data_create_from_json(text.c_str())); }
	Data(const Data &) = delete;
	Data &operator=(const Data &) = delete;
	~Data() { obs_data_release(d); }
	explicit operator bool() const { return d != nullptr; }
	// Missing keys yield "" / false / 0 / null from obs_data; no throws.
	std::string str(const char *key) const
	{
		const char *s = obs_data_get_string(d, key);
		return s ? s : "";
	}
	bool flag(const char *key) const { return obs_data_get_bool(d, key); }
	Data obj(const char *key) const { return Data(obs_data_get_obj(d, key)); }
	bool has(const char *key) const { return obs_data_has_user_value(d, key); }
};

struct DataArray {
	obs_data_array_t *a = nullptr;
	explicit DataArray(obs_data_array_t *p) : a(p) {}
	DataArray(const DataArray &) = delete;
	DataArray &operator=(const DataArray &) = delete;
	~DataArray() { obs_data_array_release(a); }
	size_t size() const { return a ? obs_data_array_count(a) : 0; }
	Data item(size_t i) const { return Data(obs_data_array_item(a, i)); }
};

std::string escape(CURL *h, const std::string &s)
{
	char *e = curl_easy_escape(h, s.c_str(), static_cast<int>(s.size()));
	std::string out = e ? e : "";
	curl_free(e);
	return out;
}

// The key travels as a header, never in the URL, so it stays out of any
// proxy or curl verbose log.
void set_common(CurlHandle &c, const std::string &api_key, long timeout)
{
	c.headers = curl_slist_append(c.headers, ("x-goog-api-key: " + api_key).c_str());
	c.headers = curl_slist_append(c.headers, "Accept: application/json");
	curl_easy_setopt(c.h, CURLOPT_HTTPHEADER, c.headers);
	curl_easy_setopt(c.h, CURLOPT_USERAGENT, kUserAgent);
	curl_easy_setopt(c.h, CURLOPT_TIMEOUT, timeout);
	curl_easy_setopt(c.h, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(c.h, CURLOPT_NOSIGNAL, 1L);
	// No Accept-Encoding: a gzip layer would buffer the server's per-message
	// flushes and cost the latency this plugin exists to remove.
}

size_t write_to_string(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
	return size * nmemb;
}

// Google error bodies: {"error":{"code":403,"message":"...","errors":[{"reason":"liveChatEnded",...}]}}
// (array-wrapped on the streaming endpoint). Returns the reason, or the
// status if there is no reason, or "" if the document is not an error.
std::string error_reason(const Data &doc, std::string *message = nullptr)
{
	if (!doc || !doc.has("error"))
		return "";
	const Data err = doc.obj("error");
	if (!err)
		return "";
	if (message)
		*message = err.str("message");
	const DataArray errors(obs_data_get_array(err.d, "errors"));
	if (errors.size() > 0) {
		if (const std::string reason = errors.item(0).str("reason"); !reason.empty())
			return reason;
	}
	if (const std::string status = err.str("status"); !status.empty())
		return status;
	return "unknown";
}

// The FAQ in code form: every documented failure as one plain sentence.
std::string explain(const std::string &reason, long http, const std::string &message)
{
	if (reason == "liveChatEnded")
		return "The stream has ended, so its chat is closed.";
	if (reason == "liveChatDisabled")
		return "Live chat is turned off for this stream (YouTube Studio > Stream settings > Live chat).";
	if (reason == "liveChatNotFound")
		return "No live chat found for that video ID. Check the ID in the YouTube Studio URL.";
	if (reason == "rateLimitExceeded")
		return "YouTube asked us to slow down (rateLimitExceeded); backing off.";
	if (reason == "quotaExceeded")
		return "Your YouTube API quota for today is used up. It resets at midnight Pacific time; see the quota page in Google Cloud Console.";
	if (reason == "forbidden" || reason == "accessNotConfigured")
		return "This API key is not allowed to use the YouTube Data API v3. Enable it in Google Cloud Console (APIs & Services > Library) and check the key's API restrictions.";
	if (reason == "badRequest" && message.find("API key not valid") != std::string::npos)
		return "The API key is not valid. Paste it again from Google Cloud Console > APIs & Services > Credentials.";
	if (reason == "pageTokenInvalid")
		return "The resume token expired; reconnecting from the current chat position.";
	if (http == 403 && message.find("unregistered callers") != std::string::npos)
		return "No API key was sent. Enter your key in the Chiripy dock.";
	return "YouTube returned HTTP " + std::to_string(http) + " (" + (reason.empty() ? "no reason" : reason) +
	       "): " + message;
}

ChatMessage parse_message(const Data &item)
{
	ChatMessage m;
	m.id = item.str("id");
	if (const Data sn = item.obj("snippet")) {
		m.type = sn.str("type");
		m.text = sn.str("displayMessage");
		m.published_at = sn.str("publishedAt");
		if (const Data del = sn.obj("messageDeletedDetails"))
			m.deleted_message_id = del.str("deletedMessageId");
		if (const Data ban = sn.obj("userBannedDetails"))
			if (const Data who = ban.obj("bannedUserDetails"))
				m.banned_channel_id = who.str("channelId");
		if (const Data sc = sn.obj("superChatDetails"))
			m.amount = sc.str("amountDisplayString");
		else if (const Data ss = sn.obj("superStickerDetails"))
			m.amount = ss.str("amountDisplayString");
	}
	if (const Data a = item.obj("authorDetails")) {
		m.author = a.str("displayName");
		m.author_channel_id = a.str("channelId");
		m.is_owner = a.flag("isChatOwner");
		m.is_moderator = a.flag("isChatModerator");
		m.is_member = a.flag("isChatSponsor");
		m.is_verified = a.flag("isVerified");
	}
	return m;
}

} // namespace

ChatLookup resolve_chat_id(const std::string &api_key, const std::string &video_id)
{
	ChatLookup r;
	CurlHandle c;
	if (!c.h) {
		r.error = "Could not initialise libcurl.";
		return r;
	}
	g_units += 1;
	set_common(c, api_key, kLookupTimeoutSec);
	const std::string url = std::string(kApiBase) + "videos?part=liveStreamingDetails&id=" + escape(c.h, video_id);
	std::string body;
	curl_easy_setopt(c.h, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c.h, CURLOPT_WRITEFUNCTION, write_to_string);
	curl_easy_setopt(c.h, CURLOPT_WRITEDATA, &body);

	const CURLcode rc = curl_easy_perform(c.h);
	if (rc != CURLE_OK) {
		r.error = std::string("Could not reach YouTube: ") + curl_easy_strerror(rc);
		r.retryable = true;
		return r;
	}
	long http = 0;
	curl_easy_getinfo(c.h, CURLINFO_RESPONSE_CODE, &http);

	const Data doc = Data::from_json(body);
	if (!doc) {
		r.error = "YouTube returned something that is not JSON (HTTP " + std::to_string(http) + ").";
		return r;
	}
	std::string message;
	if (const std::string reason = error_reason(doc, &message); !reason.empty() || http != 200) {
		r.error = explain(reason, http, message);
		return r;
	}
	const DataArray items(obs_data_get_array(doc.d, "items"));
	if (items.size() == 0) {
		r.error =
			"No video with that ID. Check the ID in the YouTube Studio URL (studio.youtube.com/video/<ID>/livestreaming).";
		return r;
	}
	if (const Data details = items.item(0).obj("liveStreamingDetails"))
		r.chat_id = details.str("activeLiveChatId");
	if (r.chat_id.empty()) {
		r.error = "That video has no active live chat yet. Waiting for it to go live.";
		r.retryable = true;
	}
	return r;
}

std::string test_key(const std::string &api_key)
{
	CurlHandle c;
	if (!c.h)
		return "Could not initialise libcurl.";
	g_units += 1;
	set_common(c, api_key, kLookupTimeoutSec);
	// "Me at the zoo": the first video ever uploaded, and still there.
	const std::string url = std::string(kApiBase) + "videos?part=id&id=jNQXAC9IVRw";
	std::string body;
	curl_easy_setopt(c.h, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c.h, CURLOPT_WRITEFUNCTION, write_to_string);
	curl_easy_setopt(c.h, CURLOPT_WRITEDATA, &body);
	const CURLcode rc = curl_easy_perform(c.h);
	if (rc != CURLE_OK)
		return std::string("Could not reach YouTube: ") + curl_easy_strerror(rc);
	long http = 0;
	curl_easy_getinfo(c.h, CURLINFO_RESPONSE_CODE, &http);
	const Data doc = Data::from_json(body);
	if (!doc)
		return "YouTube returned something that is not JSON (HTTP " + std::to_string(http) + ").";
	std::string message;
	if (const std::string reason = error_reason(doc, &message); !reason.empty() || http != 200)
		return explain(reason, http, message);
	return "";
}

std::string extract_video_id(const std::string &text)
{
	auto trim = [](std::string t) {
		while (!t.empty() && isspace(static_cast<unsigned char>(t.back())))
			t.pop_back();
		size_t i = 0;
		while (i < t.size() && isspace(static_cast<unsigned char>(t[i])))
			++i;
		return t.substr(i);
	};
	const std::string t = trim(text);
	auto id_after = [&](const std::string &marker) -> std::string {
		const size_t p = t.find(marker);
		if (p == std::string::npos)
			return "";
		size_t e = p + marker.size(), b = e;
		while (e < t.size() && (isalnum(static_cast<unsigned char>(t[e])) || t[e] == '-' || t[e] == '_'))
			++e;
		return t.substr(b, e - b);
	};
	for (const char *marker : {"studio.youtube.com/video/", "watch?v=", "youtu.be/", "/live/", "v="})
		if (const std::string id = id_after(marker); id.size() == 11)
			return id;
	return t; // hopefully a bare ID; the lookup will say if not
}

long long units_used()
{
	return g_units.load();
}

void ChatStream::start(std::string api_key, std::string video_id, MessageHandler on_message, StatusHandler on_status,
		       StateHandler on_state)
{
	stop();
	api_key_ = std::move(api_key);
	video_id_ = std::move(video_id);
	chat_id_.clear();
	on_message_ = std::move(on_message);
	on_status_ = std::move(on_status);
	on_state_ = std::move(on_state);
	page_token_.clear();
	backoff_ = 0;
	stop_requested_ = false;
	set_state(State::Connecting);
	worker_ = std::thread([this] { run(); });
}

void ChatStream::stop()
{
	stop_requested_ = true;
	if (worker_.joinable())
		worker_.join();
	if (state_ != State::Idle)
		set_state(State::Idle);
}

void ChatStream::set_state(State s)
{
	state_ = s;
	if (on_state_)
		on_state_(s);
}

// Sleeps in small steps so stop() returns promptly. False if stopped.
bool ChatStream::sleep_unless_stopped(double seconds)
{
	auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
	while (std::chrono::steady_clock::now() < until) {
		if (stop_requested_)
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return !stop_requested_;
}

void ChatStream::run()
{
	// Phase 1: video -> chat ID, retrying while the video is not live yet.
	bool waiting_logged = false;
	while (!stop_requested_) {
		const ChatLookup lookup = resolve_chat_id(api_key_, video_id_);
		if (lookup.error.empty()) {
			chat_id_ = lookup.chat_id;
			break;
		}
		if (!lookup.retryable) {
			on_status_(lookup.error);
			set_state(State::Stopped);
			return;
		}
		if (!waiting_logged) {
			on_status_(lookup.error);
			waiting_logged = true;
		}
		if (!sleep_unless_stopped(kLookupRetrySec))
			return;
	}
	if (stop_requested_)
		return;

	// Phase 2: the reconnect loop.
	while (!stop_requested_) {
		const double delay = connect_once();
		if (delay < 0)
			break;
		if (!sleep_unless_stopped(delay))
			break;
	}
	if (!stop_requested_)
		set_state(State::Stopped);
}

double ChatStream::connect_once()
{
	CurlHandle c;
	if (!c.h) {
		on_status_("Could not initialise libcurl.");
		return -1;
	}
	set_common(c, api_key_, kStreamTimeoutSec);

	std::string url = std::string(kApiBase) +
			  "liveChat/messages/stream?part=id,snippet,authorDetails&liveChatId=" + escape(c.h, chat_id_);
	if (!page_token_.empty())
		url += "&pageToken=" + escape(c.h, page_token_);

	// Everything the splitter hands us is either a message page or an error.
	std::string error_reason_seen, error_message_seen;
	bool got_page = false;
	JsonArraySplitter splitter([&](const std::string &text) {
		const Data obj = Data::from_json(text);
		if (!obj)
			return;
		if (const std::string reason = error_reason(obj, &error_message_seen); !reason.empty()) {
			error_reason_seen = reason;
			return;
		}
		got_page = true;
		if (state_ != State::Connected)
			set_state(State::Connected);
		if (obj.has("nextPageToken"))
			page_token_ = obj.str("nextPageToken");
		const DataArray items(obs_data_get_array(obj.d, "items"));
		for (size_t i = 0; i < items.size(); ++i)
			on_message_(parse_message(items.item(i)));
	});

	struct Ctx {
		JsonArraySplitter *splitter;
		std::atomic<bool> *stop;
	} ctx{&splitter, &stop_requested_};

	curl_easy_setopt(c.h, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c.h, CURLOPT_WRITEDATA, &ctx);
	curl_easy_setopt(
		c.h, CURLOPT_WRITEFUNCTION, +[](char *ptr, size_t size, size_t nmemb, void *ud) -> size_t {
			static_cast<Ctx *>(ud)->splitter->feed(ptr, size * nmemb);
			return size * nmemb;
		});
	// The progress callback is how a blocking transfer learns about stop():
	// returning non-zero aborts it within libcurl's ~1 s tick.
	curl_easy_setopt(c.h, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(c.h, CURLOPT_XFERINFODATA, &ctx);
	curl_easy_setopt(
		c.h, CURLOPT_XFERINFOFUNCTION, +[](void *ud, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
			return static_cast<Ctx *>(ud)->stop->load() ? 1 : 0;
		});

	g_units += 5;
	const CURLcode rc = curl_easy_perform(c.h);
	if (stop_requested_)
		return -1;

	long http = 0;
	curl_easy_getinfo(c.h, CURLINFO_RESPONSE_CODE, &http);

	if (rc != CURLE_OK && rc != CURLE_ABORTED_BY_CALLBACK) {
		++backoff_;
		const double delay = std::min(60.0, 2.0 * (1 << std::min(backoff_, 5)));
		on_status_(std::string("Connection to YouTube failed: ") + curl_easy_strerror(rc) + ". Retrying in " +
			   std::to_string(static_cast<int>(delay)) + " s.");
		return delay;
	}

	if (!error_reason_seen.empty() || http != 200) {
		const std::string what = explain(error_reason_seen, http, error_message_seen);
		if (error_reason_seen == "liveChatEnded" || error_reason_seen == "liveChatDisabled" ||
		    error_reason_seen == "liveChatNotFound" || error_reason_seen == "quotaExceeded" ||
		    error_reason_seen == "forbidden" || error_reason_seen == "accessNotConfigured" ||
		    error_reason_seen == "badRequest") {
			on_status_(what);
			return -1; // nothing a retry can fix; the user has to act
		}
		if (error_reason_seen == "pageTokenInvalid") {
			page_token_.clear();
			on_status_(what);
			return 1;
		}
		++backoff_;
		const double delay = std::min(60.0, 2.0 * (1 << std::min(backoff_, 5)));
		on_status_(what + " Retrying in " + std::to_string(static_cast<int>(delay)) + " s.");
		return delay;
	}

	// Normal end of a ~10.4 s window: reconnect at once with the token.
	if (got_page)
		backoff_ = 0;
	if (splitter.incomplete())
		on_status_("YouTube closed the connection mid-message; reconnecting.");
	return 0;
}

} // namespace chiripy::youtube
