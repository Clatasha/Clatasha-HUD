#include "hud-window.hpp"

#include <obs-frontend-api.h>

#include <QFont>
#include <QHBoxLayout>
#include <QVBoxLayout>

ClatashaHudWindow::ClatashaHudWindow(QWidget *parent) : QWidget(parent)
{
    setWindowTitle(QStringLiteral("Clatasha HUD"));
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setWindowFlag(Qt::WindowTransparentForInput, true);

    auto *panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("hudPanel"));
    panel->setStyleSheet(QStringLiteral(
        "#hudPanel {"
        " background: rgba(12, 14, 18, 220);"
        " border: 1px solid rgba(255,255,255,45);"
        " border-radius: 10px;"
        "}"
        "QLabel { color: white; }"));

    titleLabel_ = new QLabel(QStringLiteral("CLATASHA HUD"), panel);
    QFont titleFont = titleLabel_->font();
    titleFont.setBold(true);
    titleFont.setPointSize(11);
    titleLabel_->setFont(titleFont);

    recordingLabel_ = new QLabel(QStringLiteral("REC   IDLE"), panel);
    streamingLabel_ = new QLabel(QStringLiteral("LIVE  IDLE"), panel);

    QFont statusFont = recordingLabel_->font();
    statusFont.setFamily(QStringLiteral("Consolas"));
    statusFont.setPointSize(10);
    recordingLabel_->setFont(statusFont);
    streamingLabel_->setFont(statusFont);

    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(14, 10, 14, 10);
    panelLayout->setSpacing(4);
    panelLayout->addWidget(titleLabel_);
    panelLayout->addWidget(recordingLabel_);
    panelLayout->addWidget(streamingLabel_);

    auto *rootLayout = new QHBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->addWidget(panel);

    connect(&refreshTimer_, &QTimer::timeout, this, [this]() { refresh(); });
    refreshTimer_.start(250);

    refresh();
    adjustSize();
    move(24, 24);
}

QString ClatashaHudWindow::formatElapsed(qint64 milliseconds)
{
    const qint64 totalSeconds = milliseconds / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;

    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

void ClatashaHudWindow::refresh()
{
    const bool recordingActive = obs_frontend_recording_active();
    const bool streamingActive = obs_frontend_streaming_active();

    if (recordingActive && !recordingWasActive_) {
        recordingTimer_.restart();
    } else if (!recordingActive && recordingWasActive_) {
        recordingTimer_.invalidate();
    }

    if (streamingActive && !streamingWasActive_) {
        streamingTimer_.restart();
    } else if (!streamingActive && streamingWasActive_) {
        streamingTimer_.invalidate();
    }

    recordingWasActive_ = recordingActive;
    streamingWasActive_ = streamingActive;

    recordingLabel_->setText(
        recordingActive && recordingTimer_.isValid()
            ? QStringLiteral("REC   %1").arg(formatElapsed(recordingTimer_.elapsed()))
            : QStringLiteral("REC   IDLE"));

    streamingLabel_->setText(
        streamingActive && streamingTimer_.isValid()
            ? QStringLiteral("LIVE  %1").arg(formatElapsed(streamingTimer_.elapsed()))
            : QStringLiteral("LIVE  IDLE"));
}
