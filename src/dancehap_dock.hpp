// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 Don't Blink
//
// dancehap_dock.hpp — Qt6 dock panel for DanceHAP (Phase 3, Étape 6).
// Minimal dock: Load .dhp, Play/Stop, timecode, markers, DLayer indicators.
// Qt code is guarded by #ifdef DANCEHAP_HAVE_QT — only defined when Qt6 Widgets
// is found by CMake. In stub mode (no OBS) or when Qt6 is absent, the dock
// compiles as a no-op (register/unregister are empty functions).

#pragma once

#ifdef DANCEHAP_HAVE_QT

#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;
class QListWidget;
class QListWidgetItem;

/// Minimal DanceHAP dock widget shown in OBS.
/// Emits signals that the composite source connects to.
class DanceHAPDock : public QWidget {
    Q_OBJECT
public:
    explicit DanceHAPDock(QWidget *parent = nullptr);
    ~DanceHAPDock() override;

    // UI updates (called from the composite source via signals/slots)
    void setShowName(const QString &name);
    void setTimecode(double current, double total);
    void setMarkers(const QStringList &markerNames);
    void setDLayerIndicators(bool d1Active, bool d2Active, bool d3Active);

signals:
    void loadShowFileRequested(const QString &path);
    void playRequested();
    void stopRequested();
    void markerJumpRequested(int markerIndex);

private slots:
    void onLoadClicked();
    void onPlayClicked();
    void onStopClicked();
    void onMarkerDoubleClicked(QListWidgetItem *item);

private:
    QLabel *show_name_label_    = nullptr;
    QLabel *timecode_label_     = nullptr;
    QPushButton *load_btn_      = nullptr;
    QPushButton *play_btn_      = nullptr;
    QPushButton *stop_btn_      = nullptr;
    QListWidget *markers_list_  = nullptr;
    QLabel *dlayer_status_label_ = nullptr;
};

#endif // DANCEHAP_HAVE_QT

/// Register the dock with OBS frontend (called from obs_module_load).
/// No-op in stub mode.
void register_dancehap_dock(void);

/// Unregister and destroy the dock (called from obs_module_unload).
void unregister_dancehap_dock(void);