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

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.h>
#include <plugin-support.h>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>
#include <thread>

namespace chiripy {

namespace {

constexpr const char *kDockId = "chiripy_dock";
constexpr double kDailyUnits = 10000.0;
constexpr double kUnitsPerHour = 1730.0; // 346 connections/h x 5, measured

const char *T(const char *key)
{
	return obs_module_text(key);
}

// Hop to the UI thread from a worker (Test key runs off-thread).
void on_ui(std::function<void()> fn)
{
	auto *boxed = new std::function<void()>(std::move(fn));
	obs_queue_task(
		OBS_TASK_UI,
		[](void *p) {
			auto *f = static_cast<std::function<void()> *>(p);
			(*f)();
			delete f;
		},
		boxed, false);
}

} // namespace

Dock::Dock(QWidget *parent) : QWidget(parent)
{
	auto *root = new QVBoxLayout(this);
	auto *form = new QFormLayout();

	api_key_ = new QLineEdit(this);
	api_key_->setEchoMode(QLineEdit::Password);
	form->addRow(T("Chiripy.Dock.ApiKey"), api_key_);

	video_id_ = new QLineEdit(this);
	video_id_->setPlaceholderText(T("Chiripy.Dock.VideoIdHint"));
	form->addRow(T("Chiripy.Dock.VideoId"), video_id_);
	root->addLayout(form);

	auto *buttons = new QHBoxLayout();
	connect_ = new QPushButton(T("Chiripy.Dock.Connect"), this);
	disconnect_ = new QPushButton(T("Chiripy.Dock.Disconnect"), this);
	test_ = new QPushButton(T("Chiripy.Dock.TestKey"), this);
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
	root->addStretch(1);

	connect(connect_, &QPushButton::clicked, this, &Dock::on_save_connect);
	connect(disconnect_, &QPushButton::clicked, this, &Dock::on_disconnect);
	connect(test_, &QPushButton::clicked, this, &Dock::on_test_key);
	connect(video_id_, &QLineEdit::returnPressed, this, &Dock::on_save_connect);

	// Saved values: the key never comes back into the field, only a hint
	// that one is stored; leaving the field empty keeps it.
	const config::Settings s = controller::settings();
	has_saved_key_ = !s.api_key.empty();
	api_key_->setPlaceholderText(T(has_saved_key_ ? "Chiripy.Dock.ApiKeySaved" : "Chiripy.Dock.ApiKeyHint"));
	video_id_->setText(QString::fromStdString(s.video_id));

	controller::set_listener([this](youtube::State st, const std::string &text) { show_state(st, text); });

	auto *quota_timer = new QTimer(this);
	connect(quota_timer, &QTimer::timeout, this, &Dock::refresh_quota);
	quota_timer->start(30000);
	refresh_quota();
}

std::string Dock::key_from_field() const
{
	return api_key_->text().trimmed().toStdString();
}

void Dock::on_save_connect()
{
	const std::string key = key_from_field();
	if (key.empty() && !has_saved_key_) {
		status_->setText(T("Chiripy.Dock.NeedKey"));
		return;
	}
	controller::save_and_connect(key, video_id_->text().toStdString());
	if (!key.empty()) {
		has_saved_key_ = true;
		api_key_->clear();
		api_key_->setPlaceholderText(T("Chiripy.Dock.ApiKeySaved"));
	}
	video_id_->setText(QString::fromStdString(controller::settings().video_id));
}

void Dock::on_disconnect()
{
	controller::disconnect();
}

void Dock::on_test_key()
{
	std::string key = key_from_field();
	if (key.empty())
		key = controller::settings().api_key;
	if (key.empty()) {
		status_->setText(T("Chiripy.Dock.NeedKey"));
		return;
	}
	test_->setEnabled(false);
	status_->setText(T("Chiripy.Dock.Testing"));
	// One videos.list on a worker; the result hops back to the UI thread.
	std::thread([this, key] {
		const std::string error = youtube::test_key(key);
		on_ui([this, error] {
			test_->setEnabled(true);
			status_->setText(error.empty() ? T("Chiripy.Dock.KeyOk") : QString::fromStdString(error));
			refresh_quota();
		});
	}).detach();
}

void Dock::show_state(youtube::State state, const std::string &status)
{
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
	const bool busy = state == youtube::State::Connecting || state == youtube::State::Connected;
	disconnect_->setEnabled(busy);
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
