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
#include <QString>
#include <QToolButton>
#include <QWidget>

#include <string>

namespace chiripy {

// The "Chiripy" dock (owner's layout, 2026-09-29). Each setting is a row
// with the label right-aligned like the rest of OBS's settings, a status tick
// after the label text, the input, and a blue anchor that opens the page
// where that value comes from -- so the dock explains itself:
//
//   YouTube API key ✓  [•••••        ]  ↗   Cloud Console > Credentials
//     Live video ID ✓  [_WIQmpX0xwQ  ]  ↗   the stream (ID filled) / Studio (empty)
//   [ Save & connect ]  [ Disconnect ]  [ Test key ]
//
// then the status sentence, quota estimate, the links (the future numbered
// tutorial steps) and "Report a problem". Ticks after the label: green ✓ = OK, red ✗ = failed,
// nothing while not set or not checked yet. With no support channel this
// panel is the support channel, so every message it shows must say what to
// do next.
class Dock : public QWidget {
	Q_OBJECT

public:
	explicit Dock(QWidget *parent = nullptr);

private:
	enum class Tick { Unset, Ok, Failed };

	void on_save_connect();
	void on_disconnect();
	void on_test();       // save the typed key, test it, and check the video ID
	void on_key_anchor(); // where to get an API key
	void on_id_anchor();  // the stream if an ID is filled in, else YouTube Studio
	void on_report();
	void show_state(youtube::State state, const std::string &status);
	void refresh_quota();
	void refresh_id_anchor();
	void set_tick(QLabel *label, const QString &text, Tick t);
	std::string key_from_field() const;
	bool store_typed_key(); // false if there is no key at all

	QLabel *key_label_ = nullptr;
	QLabel *id_label_ = nullptr;
	QString key_text_;
	QString id_text_;
	QLineEdit *api_key_ = nullptr;
	QLineEdit *video_id_ = nullptr;
	QToolButton *key_anchor_ = nullptr;
	QToolButton *id_anchor_ = nullptr;
	QPushButton *connect_ = nullptr;
	QPushButton *disconnect_ = nullptr;
	QPushButton *test_ = nullptr;
	QLabel *state_ = nullptr;
	QLabel *status_ = nullptr;
	QLabel *quota_ = nullptr;
	QPushButton *report_ = nullptr;
	bool has_saved_key_ = false;
	youtube::State state_now_ = youtube::State::Idle;
};

// Creates the dock and registers it with OBS. UI thread only.
void register_dock();

} // namespace chiripy
