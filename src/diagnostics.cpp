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

#include "diagnostics.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QByteArray>
#include <QSysInfo>
#include <QUrl>

#include <ctime>
#include <deque>
#include <mutex>

namespace chiripy::diag {

namespace {

constexpr size_t kKeep = 40;
// GitHub answers 414 for over-long URLs and publishes no limit; 2000 chars
// percent-encode to roughly 4-6 KB, well inside what it accepts. The newest
// lines win when trimming.
constexpr size_t kMaxErrorsChars = 2000;
constexpr const char *kNewIssue = "https://github.com/coffeerunhobby/chiripy/issues/new";

std::mutex mtx;
std::deque<std::string> kept;

std::string stamp()
{
	const std::time_t t = std::time(nullptr);
	std::tm tmv{};
#ifdef _WIN32
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	char buf[16];
	std::strftime(buf, sizeof buf, "%H:%M:%S", &tmv);
	return buf;
}

std::string param(const char *key, const std::string &value)
{
	return std::string("&") + key + "=" + QUrl::toPercentEncoding(QString::fromStdString(value)).toStdString();
}

} // namespace

void note(const std::string &line)
{
	std::lock_guard<std::mutex> lock(mtx);
	kept.push_back(stamp() + "  " + line);
	while (kept.size() > kKeep)
		kept.pop_front();
}

void error(const char *where, const char *what)
{
	obs_log(LOG_ERROR, "internal error in %s: %s", where, what);
	note(std::string("ERROR in ") + where + ": " + what);
}

std::string recent()
{
	std::lock_guard<std::mutex> lock(mtx);
	std::string out;
	for (const std::string &line : kept) {
		out += line;
		out += '\n';
	}
	return out;
}

std::string environment()
{
	return std::string("Chiripy: ") + PLUGIN_VERSION + "\nOBS Studio: " + obs_get_version_string() +
	       "\nQt (runtime): " + qVersion() + "\nOS: " + QSysInfo::prettyProductName().toStdString() + " (" +
	       QSysInfo::currentCpuArchitecture().toStdString() + ")";
}

std::string issue_url(const std::string &state)
{
	std::string errors = recent();
	if (errors.size() > kMaxErrorsChars)
		errors = "...\n" + errors.substr(errors.size() - kMaxErrorsChars);
	if (errors.empty())
		errors = "(none)";
	return std::string(kNewIssue) + "?template=bug_report.yml" +
	       param("environment", environment() + "\nState: " + state) + param("errors", errors);
}

} // namespace chiripy::diag
