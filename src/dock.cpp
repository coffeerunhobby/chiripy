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

#include "dock.hpp"

#include "controller.hpp"
#include "diagnostics.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>
#include <plugin-support.h>

#include <QDesktopServices>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <thread>

namespace chiripy {

namespace {

constexpr const char *kDockId = "chiripy_dock";
constexpr double kDailyUnits = 10000.0;
constexpr double kUnitsPerHour = 1730.0; // 346 connections/h x 5, measured

constexpr const char *kKeysUrl = "https://console.cloud.google.com/apis/credentials";
constexpr const char *kStudioUrl = "https://studio.youtube.com/";

// Ticks and anchors; readable on OBS's dark and light themes.
const char *kTickOk = "✓";     // ✓
const char *kTickFailed = "✗"; // ✗
const char *kAnchor = "↗";     // ↗
const char *kGreen = "#3fb950";
const char *kRed = "#f85149";
const char *kAnchorCss = "QToolButton { color: #4493f8; border: none; background: transparent;"
			 " font-size: 15px; padding: 0 3px; }"
			 "QToolButton:hover { color: #79b8ff; }";

const char *T(const char *key)
{
	return obs_module_text(key);
}

// Hop to the UI thread from a worker (the test runs off-thread).
void on_ui(std::function<void()> fn)
{
	auto *boxed = new std::function<void()>(std::move(fn));
	obs_queue_task(
		OBS_TASK_UI,
		[](void *p) {
			auto *f = static_cast<std::function<void()> *>(p);
			diag::guard("dock UI task", [f] { (*f)(); });
			delete f;
		},
		boxed, false);
}

QToolButton *anchor(QWidget *parent)
{
	auto *b = new QToolButton(parent);
	b->setText(QString::fromUtf8(kAnchor));
	b->setAutoRaise(true);
	b->setCursor(Qt::PointingHandCursor);
	b->setStyleSheet(kAnchorCss);
	return b;
}

QLabel *row_label(QWidget *parent)
{
	auto *l = new QLabel(parent);
	l->setTextFormat(Qt::RichText);
	l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	return l;
}

} // namespace

Dock::Dock(QWidget *parent) : QWidget(parent)
{
	auto *root = new QVBoxLayout(this);

	// Settings rows, OBS style: label (right-aligned, tick after the text) |
	// input (stretches) | blue anchor to where the value comes from.
	auto *grid = new QGridLayout();
	grid->setHorizontalSpacing(6);
	grid->setColumnStretch(1, 1);

	key_text_ = T("Chiripy.Dock.ApiKey");
	key_label_ = row_label(this);
	api_key_ = new QLineEdit(this);
	api_key_->setEchoMode(QLineEdit::Password);
	key_anchor_ = anchor(this);
	key_anchor_->setToolTip(T("Chiripy.Dock.KeyAnchorTip"));
	grid->addWidget(key_label_, 0, 0);
	grid->addWidget(api_key_, 0, 1);
	grid->addWidget(key_anchor_, 0, 2);

	id_text_ = T("Chiripy.Dock.VideoId");
	id_label_ = row_label(this);
	video_id_ = new QLineEdit(this);
	video_id_->setPlaceholderText(T("Chiripy.Dock.VideoIdHint"));
	id_anchor_ = anchor(this);
	grid->addWidget(id_label_, 1, 0);
	grid->addWidget(video_id_, 1, 1);
	grid->addWidget(id_anchor_, 1, 2);
	root->addLayout(grid);

	auto *buttons = new QHBoxLayout();
	connect_ = new QPushButton(T("Chiripy.Dock.Connect"), this);
	disconnect_ = new QPushButton(T("Chiripy.Dock.Disconnect"), this);
	test_ = new QPushButton(T("Chiripy.Dock.TestKey"), this);
	test_->setToolTip(T("Chiripy.Dock.TestTip"));
	buttons->addWidget(connect_);
	buttons->addWidget(disconnect_);
	buttons->addWidget(test_);
	root->addLayout(buttons);

	state_ = new QLabel(this);
	state_->setTextFormat(Qt::PlainText);
	root->addWidget(state_);

	status_ = new QLabel(this);
	status_->setWordWrap(true);
	status_->setTextFormat(Qt::PlainText);
	root->addWidget(status_);

	quota_ = new QLabel(this);
	quota_->setWordWrap(true);
	quota_->setTextFormat(Qt::PlainText);
	root->addWidget(quota_);

	// Kept on purpose: these become the numbered tutorial steps.
	auto *links = new QLabel(this);
	links->setTextFormat(Qt::RichText);
	links->setOpenExternalLinks(true);
	links->setWordWrap(true);
	links->setText(
		QString("<a href=\"https://console.cloud.google.com/apis/credentials\">%1</a><br>"
			"<a href=\"https://console.cloud.google.com/apis/library/youtube.googleapis.com\">%2</a><br>"
			"<a href=\"https://console.cloud.google.com/apis/api/youtube.googleapis.com/quotas\">%3</a><br>"
			"<a href=\"https://studio.youtube.com/\">%4</a>")
			.arg(T("Chiripy.Dock.LinkKeys"), T("Chiripy.Dock.LinkEnable"), T("Chiripy.Dock.LinkQuota"),
			     T("Chiripy.Dock.LinkStudio")));
	root->addWidget(links);

	report_ = new QPushButton(T("Chiripy.Dock.Report"), this);
	report_->setToolTip(T("Chiripy.Dock.ReportHint"));
	root->addWidget(report_);
	root->addStretch(1);

	// Qt slots behind exception barriers: an exception through the Qt event
	// loop is fatal to OBS.
	auto guarded = [this](const char *where, void (Dock::*fn)()) {
		return [this, where, fn] {
			diag::guard(where, [this, fn] { (this->*fn)(); });
		};
	};
	connect(connect_, &QPushButton::clicked, this, guarded("Save & connect", &Dock::on_save_connect));
	connect(video_id_, &QLineEdit::returnPressed, this, guarded("Save & connect", &Dock::on_save_connect));
	connect(disconnect_, &QPushButton::clicked, this, guarded("Disconnect", &Dock::on_disconnect));
	connect(test_, &QPushButton::clicked, this, guarded("Test key", &Dock::on_test));
	connect(api_key_, &QLineEdit::returnPressed, this, guarded("Test key", &Dock::on_test));
	connect(key_anchor_, &QToolButton::clicked, this, guarded("key anchor", &Dock::on_key_anchor));
	connect(id_anchor_, &QToolButton::clicked, this, guarded("video anchor", &Dock::on_id_anchor));
	connect(report_, &QPushButton::clicked, this, guarded("report", &Dock::on_report));
	connect(video_id_, &QLineEdit::textChanged, this, [this](const QString &) {
		refresh_id_anchor();
		// An edited ID has not been checked yet.
		if (state_now_ != youtube::State::Connected)
			set_tick(id_label_, id_text_, Tick::Unset);
	});

	// Saved values: the key never comes back into the field, only a hint
	// that one is stored; leaving the field empty keeps it.
	const config::Settings s = controller::settings();
	has_saved_key_ = !s.api_key.empty();
	api_key_->setPlaceholderText(T(has_saved_key_ ? "Chiripy.Dock.ApiKeySaved" : "Chiripy.Dock.ApiKeyHint"));
	set_tick(key_label_, key_text_, has_saved_key_ ? Tick::Ok : Tick::Unset);
	set_tick(id_label_, id_text_, Tick::Unset);
	video_id_->setText(QString::fromStdString(s.video_id));
	refresh_id_anchor();
	disconnect_->setEnabled(false);

	controller::set_listener([this](youtube::State st, const std::string &text) {
		diag::guard("dock update", [&] { show_state(st, text); });
	});

	auto *quota_timer = new QTimer(this);
	connect(quota_timer, &QTimer::timeout, this, &Dock::refresh_quota);
	quota_timer->start(30000);
	refresh_quota();
}

// "YouTube API key ✓" -- after the label text: a green tick when OK, a red
// cross when it failed, nothing while not set or not checked yet.
void Dock::set_tick(QLabel *label, const QString &text, Tick t)
{
	if (t == Tick::Unset) {
		label->setText(text.toHtmlEscaped());
		return;
	}
	const bool ok = t == Tick::Ok;
	label->setText(text.toHtmlEscaped() +
		       QString(" <span style=\"color:%1; font-weight:bold;\">%2</span>")
			       .arg(ok ? kGreen : kRed, QString::fromUtf8(ok ? kTickOk : kTickFailed)));
}

// The video ID's anchor: with an ID, the stream itself; without one, YouTube
// Studio, where the ID is found (studio.youtube.com/video/<ID>/livestreaming).
void Dock::refresh_id_anchor()
{
	const bool has_id = !youtube::extract_video_id(video_id_->text().toStdString()).empty();
	id_anchor_->setToolTip(T(has_id ? "Chiripy.Dock.OpenStream" : "Chiripy.Dock.IdAnchorTip"));
}

std::string Dock::key_from_field() const
{
	return api_key_->text().trimmed().toStdString();
}

bool Dock::store_typed_key()
{
	const std::string key = key_from_field();
	if (!key.empty()) {
		controller::save_key(key);
		has_saved_key_ = true;
		api_key_->clear();
		api_key_->setPlaceholderText(T("Chiripy.Dock.ApiKeySaved"));
	}
	if (!has_saved_key_) {
		status_->setText(T("Chiripy.Dock.NeedKey"));
		set_tick(key_label_, key_text_, Tick::Unset);
		return false;
	}
	return true;
}

void Dock::on_save_connect()
{
	if (!store_typed_key())
		return;
	controller::save_and_connect("", video_id_->text().toStdString());
	video_id_->setText(QString::fromStdString(controller::settings().video_id));
}

void Dock::on_disconnect()
{
	controller::disconnect();
}

// Checks everything the dock knows without connecting: the key (one cheap
// videos.list) and, if a video ID is filled in, that the video exists and has
// an active chat (one more videos.list). Both ticks show the result.
void Dock::on_test()
{
	if (!store_typed_key())
		return;
	const std::string key = controller::settings().api_key;
	const std::string id = youtube::extract_video_id(video_id_->text().toStdString());
	test_->setEnabled(false);
	status_->setText(T("Chiripy.Dock.Testing"));
	std::thread([this, key, id] {
		std::string key_error, id_error;
		bool id_checked = false, id_waiting = false;
		if (!diag::guard("Test key", [&] {
			    key_error = youtube::test_key(key);
			    if (key_error.empty() && !id.empty()) {
				    const youtube::ChatLookup lookup = youtube::resolve_chat_id(key, id);
				    id_checked = true;
				    id_error = lookup.error;
				    id_waiting = !lookup.error.empty() && lookup.retryable;
			    }
		    }))
			key_error = "The test hit an internal error; use \"Report a problem\" to send us the details.";
		on_ui([this, key_error, id_error, id_checked, id_waiting] {
			test_->setEnabled(true);
			set_tick(key_label_, key_text_, key_error.empty() ? Tick::Ok : Tick::Failed);
			if (id_checked && state_now_ != youtube::State::Connected)
				set_tick(id_label_, id_text_,
					 id_error.empty() ? Tick::Ok
					 : id_waiting     ? Tick::Unset
							  : Tick::Failed);
			if (!key_error.empty())
				status_->setText(QString::fromStdString(key_error));
			else if (id_checked && !id_error.empty())
				status_->setText(QString::fromStdString(id_error));
			else
				status_->setText(T(id_checked ? "Chiripy.Dock.AllOk" : "Chiripy.Dock.KeyOk"));
			refresh_quota();
		});
	}).detach();
}

void Dock::on_key_anchor()
{
	QDesktopServices::openUrl(QUrl(kKeysUrl));
}

void Dock::on_id_anchor()
{
	const std::string id = youtube::extract_video_id(video_id_->text().toStdString());
	QDesktopServices::openUrl(
		id.empty() ? QUrl(kStudioUrl)
			   : QUrl(QString("https://www.youtube.com/watch?v=") + QString::fromStdString(id)));
}

void Dock::on_report()
{
	QDesktopServices::openUrl(
		QUrl::fromEncoded(QByteArray::fromStdString(diag::issue_url(state_->text().toStdString()))));
}

void Dock::show_state(youtube::State state, const std::string &status)
{
	state_now_ = state;
	const char *label = "Chiripy.Dock.StateIdle";
	switch (state) {
	case youtube::State::Connecting:
		label = "Chiripy.Dock.StateConnecting";
		break;
	case youtube::State::Connected:
		label = "Chiripy.Dock.StateConnected";
		break;
	case youtube::State::Stopped:
		label = "Chiripy.Dock.StateStopped";
		break;
	case youtube::State::Idle:
		break;
	}
	state_->setText(T(label));
	status_->setText(QString::fromStdString(status));

	if (state == youtube::State::Connected) {
		set_tick(id_label_, id_text_, Tick::Ok);
		set_tick(key_label_, key_text_, Tick::Ok); // a live connection proves the key
	} else if (state == youtube::State::Stopped) {
		set_tick(id_label_, id_text_, Tick::Failed);
	} else if (state == youtube::State::Connecting) {
		set_tick(id_label_, id_text_, Tick::Unset);
	}

	const bool busy = state == youtube::State::Connecting || state == youtube::State::Connected;
	disconnect_->setEnabled(busy);

	const QString id = QString::fromStdString(controller::settings().video_id);
	if (video_id_->text() != id && !video_id_->hasFocus())
		video_id_->setText(id);
	refresh_quota();
}

void Dock::refresh_quota()
{
	if (!isVisible())
		return;
	const long long used = youtube::units_used();
	const double left_hours = (kDailyUnits - static_cast<double>(used)) / kUnitsPerHour;
	quota_->setText(QString(T("Chiripy.Dock.Quota")).arg(used).arg(left_hours < 0 ? 0.0 : left_hours, 0, 'f', 1));
}

void register_dock()
{
	auto *dock = new Dock();
	if (!obs_frontend_add_dock_by_id(kDockId, T("Chiripy.Dock.Title"), dock))
		obs_log(LOG_ERROR, "could not register the Chiripy dock");
}

} // namespace chiripy
