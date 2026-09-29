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

#include <cstddef>
#include <functional>
#include <string>

namespace chiripy {

// Splits the streamList response -- a JSON array whose elements arrive one
// server push at a time, `[` ... `{obj}` `,{obj}` ... `]` -- into complete
// top-level objects as the bytes land, so each push is parsed the moment it
// is whole rather than when the array closes ~10 s later.
//
// It is not a JSON parser: it only tracks string literals (with escapes) and
// brace depth. Anything at depth 0 that is not `{` (the array brackets,
// commas, whitespace) is skipped. Error responses are array-wrapped objects
// too, so they come out of the same path.
class JsonArraySplitter {
public:
	using Callback = std::function<void(const std::string &object_text)>;

	explicit JsonArraySplitter(Callback on_object) : on_object_(std::move(on_object)) {}

	void feed(const char *data, size_t len)
	{
		for (size_t i = 0; i < len; ++i) {
			const char c = data[i];
			if (depth_ == 0) {
				if (c != '{')
					continue;
				depth_ = 1;
				buffer_.assign(1, c);
				continue;
			}
			buffer_ += c;
			if (in_string_) {
				if (escape_)
					escape_ = false;
				else if (c == '\\')
					escape_ = true;
				else if (c == '"')
					in_string_ = false;
				continue;
			}
			if (c == '"')
				in_string_ = true;
			else if (c == '{')
				++depth_;
			else if (c == '}' && --depth_ == 0) {
				on_object_(buffer_);
				buffer_.clear();
			}
		}
	}

	// True while a partial object is buffered; a connection that closes here
	// was cut mid-message.
	bool incomplete() const { return depth_ != 0; }

	void reset()
	{
		depth_ = 0;
		in_string_ = false;
		escape_ = false;
		buffer_.clear();
	}

private:
	Callback on_object_;
	std::string buffer_;
	int depth_ = 0;
	bool in_string_ = false;
	bool escape_ = false;
};

} // namespace chiripy
