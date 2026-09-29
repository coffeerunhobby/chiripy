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

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QWidget>

#include <string>

namespace chiripy {

// The "Chiripy" dock: API key, video ID, connect/disconnect, "Test key", a
// status line in plain English, a quota estimate and the deep links. With no
// support channel this panel is the support channel, so every message it
// shows must say what to do next.
class Dock : public QWidget {
	Q_OBJECT

public:
	explicit Dock(QWidget *parent = nullptr);

private slots:
	void on_save_connect();
	void on_disconnect();
	void on_test_key();
	void on_open_stream();

private:
	void show_state(youtube::State state, const std::string &status);
	void refresh_quota();
	std::string key_from_field() const;

	QLineEdit *api_key_ = nullptr;
	QLineEdit *video_id_ = nullptr;
	QToolButton *open_ = nullptr; // opens the watch page of the video ID
	QPushButton *connect_ = nullptr;
	QPushButton *disconnect_ = nullptr;
	QPushButton *test_ = nullptr;
	QLabel *state_ = nullptr;
	QLabel *status_ = nullptr;
	QLabel *quota_ = nullptr;
	bool has_saved_key_ = false;
};

// Creates the dock and registers it with OBS. UI thread only.
void register_dock();

} // namespace chiripy
