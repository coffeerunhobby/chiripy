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

// Tests for JsonArraySplitter. Builds without OBS:
//   clang++ -std=c++17 -I src tests/splitter_test.cpp

#include "json_splitter.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using chiripy::JsonArraySplitter;

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok)
		++failures;
}

std::vector<std::string> split(const std::string &input, size_t chunk)
{
	std::vector<std::string> out;
	JsonArraySplitter s([&](const std::string &o) { out.push_back(o); });
	for (size_t i = 0; i < input.size(); i += chunk)
		s.feed(input.data() + i, std::min(chunk, input.size() - i));
	return out;
}

// Shape of a real streamList response: history page, a later push, and the
// trailing empty element the server sends before closing.
const std::string kStream = R"([{
  "kind": "youtube#liveChatMessageListResponse",
  "nextPageToken": "GLr21sGG-JYD",
  "items": [{"id": "a", "snippet": {"type": "textMessageEvent", "displayMessage": "brace { in text } and \"quote\""}}]
}
,{
  "nextPageToken": "GM38tqiH-JYD",
  "items": [{"id": "b", "snippet": {"displayMessage": "backslash \\ then quote \\\" still in string }"}}]
}
,{
  "pageInfo": {"totalResults": 0}, "nextPageToken": "GM38tqiH-JYD", "items": []
}
])";

} // namespace

int main()
{
	for (size_t chunk : {1u, 7u, 64u, 100000u}) {
		const auto objs = split(kStream, chunk);
		char what[64];
		std::snprintf(what, sizeof what, "3 objects with %zu-byte chunks", chunk);
		check(objs.size() == 3, what);
		if (objs.size() == 3) {
			check(objs[0].front() == '{' && objs[0].back() == '}', "  first object is brace-delimited");
			check(objs[0].find("\"a\"") != std::string::npos, "  first object is the history page");
			check(objs[1].find("still in string }") != std::string::npos,
			      "  brace inside escaped-quote string did not split");
			check(objs[2].find("\"items\": []") != std::string::npos, "  trailing empty element delivered");
		}
	}

	// Array-wrapped error body arrives through the same path.
	const auto err = split("[{\n  \"error\": {\"code\": 400, \"message\": \"API key not valid.\"}\n}\n]", 5);
	check(err.size() == 1 && err[0].find("\"error\"") != std::string::npos, "error object delivered");

	// Partial object is reported, and nothing is emitted for it.
	{
		std::vector<std::string> out;
		JsonArraySplitter s([&](const std::string &o) { out.push_back(o); });
		const char *partial = "[{\"items\": [{\"id\": \"x\"";
		s.feed(partial, std::strlen(partial));
		check(out.empty() && s.incomplete(), "cut mid-object: nothing emitted, incomplete() true");
		s.reset();
		check(!s.incomplete(), "reset clears state");
	}

	// Whitespace, commas and brackets at depth 0 are ignored; an empty array
	// yields nothing.
	check(split("[ ]", 1).empty(), "empty array yields no objects");
	check(split("  [\n{}\n,\n{}\n]  ", 3).size() == 2, "two empty objects");

	std::printf("%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
	return failures ? 1 : 0;
}
