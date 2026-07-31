// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// dancehap_dock.cpp — Qt6 dock implementation (Phase 3, Étape 6).
// Qt code is guarded by #ifdef DANCEHAP_HAVE_QT.
// The OBS frontend API calls (obs_frontend_add_dock_by_id etc.) are guarded
// by #ifdef DANCEHAP_HAVE_OBS inside the DANCEHAP_HAVE_QT block.

#include "dancehap_dock.hpp"
#include "obs_compat.hpp"

#ifdef DANCEHAP_HAVE_QT

#ifdef DANCEHAP_HAVE_OBS
#include <obs-frontend-api.h>
#endif

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QListWidget>
#include <QListWidgetItem>

// ---------------------------------------------------------------------------
// DanceHAPDock
// ---------------------------------------------------------------------------

DanceHAPDock::DanceHAPDock(QWidget *parent) : QWidget(parent)
{
    auto *main_layout = new QVBoxLayout(this);

    // Show name
    show_name_label_ = new QLabel("Show: (none)", this);
    main_layout->addWidget(show_name_label_);

    // Load button
    auto *file_layout = new QHBoxLayout();
    load_btn_ = new QPushButton("Load .dhp...", this);
    file_layout->addWidget(load_btn_);
    main_layout->addLayout(file_layout);

    // Timecode
    timecode_label_ = new QLabel("00:00 / 00:00", this);
    timecode_label_->setAlignment(Qt::AlignCenter);
    main_layout->addWidget(timecode_label_);

    // Transport
    auto *transport_layout = new QHBoxLayout();
    play_btn_ = new QPushButton("Play", this);
    stop_btn_ = new QPushButton("Stop", this);
    transport_layout->addWidget(play_btn_);
    transport_layout->addWidget(stop_btn_);
    main_layout->addLayout(transport_layout);

    // Markers
    markers_list_ = new QListWidget(this);
    main_layout->addWidget(markers_list_, 1);

    // DLayer status
    dlayer_status_label_ = new QLabel("D1: -  D2: -  D3: -", this);
    main_layout->addWidget(dlayer_status_label_);

    // Connections
    connect(load_btn_, &QPushButton::clicked, this, &DanceHAPDock::onLoadClicked);
    connect(play_btn_, &QPushButton::clicked, this, &DanceHAPDock::onPlayClicked);
    connect(stop_btn_, &QPushButton::clicked, this, &DanceHAPDock::onStopClicked);
    connect(markers_list_, &QListWidget::itemDoubleClicked,
            this, &DanceHAPDock::onMarkerDoubleClicked);
}

DanceHAPDock::~DanceHAPDock() = default;

void DanceHAPDock::onLoadClicked()
{
    QString path = QFileDialog::getOpenFileName(
        this, "Load DanceHAP Show File", QString(),
        "DanceHAP Show (*.dhp)");
    if (!path.isEmpty()) {
        emit loadShowFileRequested(path);
        show_name_label_->setText("Show: " + QFileInfo(path).baseName());
    }
}

void DanceHAPDock::onPlayClicked()
{
    emit playRequested();
}

void DanceHAPDock::onStopClicked()
{
    emit stopRequested();
}

void DanceHAPDock::onMarkerDoubleClicked(QListWidgetItem *item)
{
    if (!item) return;
    int idx = markers_list_->row(item);
    emit markerJumpRequested(idx);
}

void DanceHAPDock::setShowName(const QString &name)
{
    show_name_label_->setText("Show: " + name);
}

void DanceHAPDock::setTimecode(double current, double total)
{
    auto formatTime = [](double seconds) -> QString {
        int m = static_cast<int>(seconds) / 60;
        int s = static_cast<int>(seconds) % 60;
        return QString("%1:%2")
            .arg(m, 2, 10, QChar('0'))
            .arg(s, 2, 10, QChar('0'));
    };
    timecode_label_->setText(formatTime(current) + " / " + formatTime(total));
}

void DanceHAPDock::setMarkers(const QStringList &markerNames)
{
    markers_list_->clear();
    for (const auto &name : markerNames) {
        markers_list_->addItem(name);
    }
}

void DanceHAPDock::setDLayerIndicators(bool d1Active, bool d2Active, bool d3Active)
{
    auto status = [](bool active) -> QString { return active ? "ON" : "--"; };
    dlayer_status_label_->setText(
        "D1: " + status(d1Active) + "  D2: " + status(d2Active) +
        "  D3: " + status(d3Active));
}

#endif // DANCEHAP_HAVE_QT

// ---------------------------------------------------------------------------
// Registration (no-op when Qt6 or OBS is not available)
// ---------------------------------------------------------------------------

#if defined(DANCEHAP_HAVE_QT) && defined(DANCEHAP_HAVE_OBS)
static DanceHAPDock *g_dock = nullptr;
#endif

void register_dancehap_dock(void)
{
#if defined(DANCEHAP_HAVE_QT) && defined(DANCEHAP_HAVE_OBS)
    if (g_dock) return;
    g_dock = new DanceHAPDock();
    obs_frontend_add_dock_by_id("dancehap_dock", "DanceHAP", g_dock);
#endif
}

void unregister_dancehap_dock(void)
{
#if defined(DANCEHAP_HAVE_QT) && defined(DANCEHAP_HAVE_OBS)
    if (g_dock) {
        obs_frontend_remove_dock("dancehap_dock");
        delete g_dock;
        g_dock = nullptr;
    }
#endif
}