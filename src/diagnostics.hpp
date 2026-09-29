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

#include <exception>
#include <string>
#include <utility>

// Error barriers and problem reports.
//
// Every place where OBS, libcurl, Qt or a std::thread calls into plugin code
// runs its body through guard(): a C++ exception escaping into those C
// callbacks is undefined behaviour and usually takes OBS down with it. A
// caught exception is logged with where it happened and kept for the report.
//
// Reports are user-initiated only (the dock's "Report a problem" button opens
// a pre-filled GitHub issue in the browser; nothing is sent until the user
// submits it). The kept lines are Chiripy's own status and error sentences --
// never chat messages, never the API key.
namespace chiripy::diag {

// Keeps a line for the next report (thread-safe; the newest ~40 are kept).
void note(const std::string &line);
// Logs an internal error via blog() and keeps it for the report.
void error(const char *where, const char *what);

std::string recent();      // kept lines, oldest first
std::string environment(); // Chiripy, OBS, Qt, OS versions
// https://github.com/.../issues/new?template=bug_report.yml&... with the
// environment, the connection state and the recent lines filled in.
std::string issue_url(const std::string &state);

// Runs f, swallowing and recording any exception. Returns false if f threw.
template<typename F> bool guard(const char *where, F &&f) noexcept
{
	try {
		std::forward<F>(f)();
		return true;
	} catch (const std::exception &e) {
		error(where, e.what());
	} catch (...) {
		error(where, "unknown exception");
	}
	return false;
}

} // namespace chiripy::diag
