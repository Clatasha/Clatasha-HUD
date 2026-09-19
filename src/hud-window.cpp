#include "hud-window.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/config-file.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSettings>
#include <QStorageInfo>

namespace {
constexpr int kHudWidth = 143;
constexpr int kHudHeight = 48;
constexpr int kScreenMargin = 12;

const char kLogoFallbackBase64[] =
"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";

void drawSegmentedMeter(QPainter &p, int x, int y, int w, int h, float level)
{
    constexpr int segments = 6;
    constexpr int gap = 1;
    const int segmentHeight = (h - gap * (segments - 1)) / segments;
    const int active = qBound(0, static_cast<int>(std::ceil(level * segments)), segments);

    for (int i = 0; i < segments; ++i) {
        const int sy = y + h - segmentHeight - i * (segmentHeight + gap);
        const bool on = i < active;
        p.setPen(Qt::NoPen);
        p.setBrush(on ? QColor(31, 218, 102) : QColor(49, 55, 59, 205));
        p.drawRoundedRect(QRect(x, sy, w, segmentHeight), 0.8, 0.8);
    }
}
} // namespace

ClatashaHudWindow::ClatashaHudWindow(QWidget *parent) : QWidget(parent)
{
    setWindowTitle(QStringLiteral("Clatasha HUD"));
    setFixedSize(kHudWidth, kHudHeight);
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setWindowFlag(Qt::WindowTransparentForInput, true);

    loadSettings();
    loadLogo();
    setWindowOpacity(1.0);

    desktopMeter_ = obs_volmeter_create(OBS_FADER_LOG);
    micMeter_ = obs_volmeter_create(OBS_FADER_LOG);

    if (desktopMeter_)
        obs_volmeter_add_callback(desktopMeter_, desktopMeterUpdated, this);
    if (micMeter_)
        obs_volmeter_add_callback(micMeter_, micMeterUpdated, this);

    refreshAudioSources();

    connect(&refreshTimer_, &QTimer::timeout, this, [this]() { refresh(); });
    refreshTimer_.start(100);

    refresh();
    QTimer::singleShot(0, this, [this]() { positionHud(); });
}

ClatashaHudWindow::~ClatashaHudWindow()
{
    if (desktopMeter_) {
        obs_volmeter_remove_callback(desktopMeter_, desktopMeterUpdated, this);
        obs_volmeter_detach_source(desktopMeter_);
        obs_volmeter_destroy(desktopMeter_);
        desktopMeter_ = nullptr;
    }

    if (micMeter_) {
        obs_volmeter_remove_callback(micMeter_, micMeterUpdated, this);
        obs_volmeter_detach_source(micMeter_);
        obs_volmeter_destroy(micMeter_);
        micMeter_ = nullptr;
    }

    if (desktopSource_) {
        obs_source_release(desktopSource_);
        desktopSource_ = nullptr;
    }

    if (micSource_) {
        obs_source_release(micSource_);
        micSource_ = nullptr;
    }
}

QString ClatashaHudWindow::settingsFilePath() const
{
    char *path = obs_module_config_path("settings.ini");
    if (!path)
        return {};

    const QString result = QString::fromUtf8(path);
    bfree(path);
    return result;
}

void ClatashaHudWindow::loadSettings()
{
    const QString path = settingsFilePath();
    if (path.isEmpty())
        return;

    QDir().mkpath(QFileInfo(path).absolutePath());
    QSettings settings(path, QSettings::IniFormat);
    opacityPercent_ = qBound(10, settings.value(QStringLiteral("hud/opacity"), 50).toInt(), 100);
    location_ = settings.value(QStringLiteral("hud/location"), QStringLiteral("top-right")).toString();
}

void ClatashaHudWindow::saveSettings() const
{
    const QString path = settingsFilePath();
    if (path.isEmpty())
        return;

    QDir().mkpath(QFileInfo(path).absolutePath());
    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(QStringLiteral("hud/opacity"), opacityPercent_);
    settings.setValue(QStringLiteral("hud/location"), location_);
    settings.sync();
}

void ClatashaHudWindow::loadLogo()
{
    bool loaded = false;
    char *path = obs_module_file("clatasha-logo.png");

    if (path) {
        loaded = logo_.load(QString::fromUtf8(path));
        blog(loaded ? LOG_INFO : LOG_WARNING,
             "[Clatasha HUD] Logo %s: %s",
             loaded ? "loaded" : "failed to load",
             path);
        bfree(path);
    }

    if (!loaded) {
        loaded = logo_.loadFromData(QByteArray::fromBase64(kLogoFallbackBase64), "PNG");
        blog(loaded ? LOG_INFO : LOG_WARNING,
             "[Clatasha HUD] Embedded logo fallback %s",
             loaded ? "loaded" : "failed");
    }
}

void ClatashaHudWindow::setOpacityPercent(int value)
{
    opacityPercent_ = qBound(10, value, 100);
    update();
}

void ClatashaHudWindow::setLocation(const QString &location)
{
    static const QStringList valid = {
        QStringLiteral("top-left"),
        QStringLiteral("top-right"),
        QStringLiteral("bottom-left"),
        QStringLiteral("bottom-right"),
    };

    location_ = valid.contains(location) ? location : QStringLiteral("top-right");
    positionHud();
}

void ClatashaHudWindow::positionHud()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const QRect area = screen->availableGeometry();
    int x = area.left() + kScreenMargin;
    int y = area.top() + kScreenMargin;

    if (location_.endsWith(QStringLiteral("right")))
        x = area.right() - width() - kScreenMargin + 1;
    if (location_.startsWith(QStringLiteral("bottom")))
        y = area.bottom() - height() - kScreenMargin + 1;

    move(x, y);
}

QString ClatashaHudWindow::formatElapsed(qint64 milliseconds)
{
    const qint64 totalSeconds = milliseconds / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;

    return QStringLiteral("%1:%2:%3")
        .arg(hours)
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

float ClatashaHudWindow::meterValue(const float peak[MAX_AUDIO_CHANNELS])
{
    float best = -60.0f;

    for (int i = 0; i < MAX_AUDIO_CHANNELS; ++i) {
        if (std::isfinite(peak[i]))
            best = std::max(best, peak[i]);
    }

    return std::clamp((best + 60.0f) / 60.0f, 0.0f, 1.0f);
}

void ClatashaHudWindow::desktopMeterUpdated(void *param,
                                            const float[MAX_AUDIO_CHANNELS],
                                            const float peak[MAX_AUDIO_CHANNELS],
                                            const float[MAX_AUDIO_CHANNELS])
{
    auto *hud = static_cast<ClatashaHudWindow *>(param);
    hud->desktopLevel_.store(meterValue(peak), std::memory_order_relaxed);
}

void ClatashaHudWindow::micMeterUpdated(void *param,
                                        const float[MAX_AUDIO_CHANNELS],
                                        const float peak[MAX_AUDIO_CHANNELS],
                                        const float[MAX_AUDIO_CHANNELS])
{
    auto *hud = static_cast<ClatashaHudWindow *>(param);
    hud->micLevel_.store(meterValue(peak), std::memory_order_relaxed);
}

void ClatashaHudWindow::attachDesktopSource(obs_source_t *source)
{
    if (source == desktopSource_) {
        if (source)
            obs_source_release(source);
        return;
    }

    if (desktopMeter_)
        obs_volmeter_detach_source(desktopMeter_);
    if (desktopSource_)
        obs_source_release(desktopSource_);

    desktopSource_ = source;

    if (desktopMeter_ && desktopSource_)
        obs_volmeter_attach_source(desktopMeter_, desktopSource_);
}

void ClatashaHudWindow::attachMicSource(obs_source_t *source)
{
    if (source == micSource_) {
        if (source)
            obs_source_release(source);
        return;
    }

    if (micMeter_)
        obs_volmeter_detach_source(micMeter_);
    if (micSource_)
        obs_source_release(micSource_);

    micSource_ = source;

    if (micMeter_ && micSource_)
        obs_volmeter_attach_source(micMeter_, micSource_);
}

void ClatashaHudWindow::refreshAudioSources()
{
    // OBS global audio channels: 1 = Desktop Audio, 3 = Mic/Aux.
    attachDesktopSource(obs_get_output_source(1));
    attachMicSource(obs_get_output_source(3));
}

QString ClatashaHudWindow::recordingPath() const
{
    config_t *config = obs_frontend_get_profile_config();
    if (!config)
        return QDir::homePath();

    const char *mode = config_get_string(config, "Output", "Mode");
    const char *path = nullptr;

    if (mode && std::strcmp(mode, "Advanced") == 0) {
        const char *type = config_get_string(config, "AdvOut", "RecType");
        path = (type && std::strcmp(type, "Standard") != 0)
                   ? config_get_string(config, "AdvOut", "FFFilePath")
                   : config_get_string(config, "AdvOut", "RecFilePath");
    } else {
        path = config_get_string(config, "SimpleOutput", "FilePath");
    }

    return (path && *path) ? QString::fromUtf8(path) : QDir::homePath();
}

QString ClatashaHudWindow::diskSpaceText() const
{
    QStorageInfo storage(recordingPath());
    if (!storage.isValid() || !storage.isReady())
        storage = QStorageInfo::root();

    const double gib = static_cast<double>(storage.bytesAvailable()) / (1024.0 * 1024.0 * 1024.0);
    return QStringLiteral("%1 GB").arg(QString::number(gib, 'f', gib >= 100.0 ? 0 : 1));
}

void ClatashaHudWindow::refresh()
{
    recordingActive_ = obs_frontend_recording_active();
    streamingActive_ = obs_frontend_streaming_active();

    const bool sessionActive = recordingActive_ || streamingActive_;

    if (sessionActive && !sessionWasActive_)
        sessionTimer_.restart();
    else if (!sessionActive && sessionWasActive_)
        sessionTimer_.invalidate();

    sessionWasActive_ = sessionActive;
    timerText_ = (sessionActive && sessionTimer_.isValid())
                     ? formatElapsed(sessionTimer_.elapsed())
                     : QStringLiteral("0:00:00");

    fps_ = obs_get_active_fps();
    spinnerAngle_ = sessionActive ? (spinnerAngle_ + 20) % 360 : 0;

    if (++audioRefreshTicks_ >= 20) {
        audioRefreshTicks_ = 0;
        refreshAudioSources();
        diskText_ = diskSpaceText();
    }

    update();
}

void ClatashaHudWindow::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QPainterPath panel;
    panel.addRoundedRect(QRectF(0.5, 0.5, width() - 1.0, height() - 1.0), 4.0, 4.0);
    const int panelAlpha = qBound(0, qRound(238.0 * opacityPercent_ / 100.0), 255);
    const int borderAlpha = qBound(0, qRound(105.0 * opacityPercent_ / 100.0), 255);
    p.fillPath(panel, QColor(8, 10, 12, panelAlpha));
    p.setPen(QPen(QColor(100, 109, 116, borderAlpha), 1.0));
    p.drawPath(panel);

    drawSegmentedMeter(p, 5, 8, 3, 32, desktopLevel_.load(std::memory_order_relaxed));
    drawSegmentedMeter(p, 11, 8, 3, 32, micLevel_.load(std::memory_order_relaxed));

    p.setPen(QColor(232, 24, 43));
    p.setFont(QFont(QStringLiteral("Segoe UI"), 19, QFont::Bold));
    p.drawText(QRect(18, 0, 52, 29), Qt::AlignCenter,
               QString::number(static_cast<int>(std::lround(fps_))));

    p.setPen(QColor(205, 209, 212));
    p.setFont(QFont(QStringLiteral("Segoe UI"), 7, QFont::Normal));
    p.drawText(QRect(27, 27, 61, 13), Qt::AlignCenter, timerText_);

    const bool sessionActive = recordingActive_ || streamingActive_;
    const QPointF spinnerCenter(104.0, 23.0);
    const QRectF spinnerRect(spinnerCenter.x() - 10.0, spinnerCenter.y() - 10.0, 20.0, 20.0);

    if (sessionActive) {
        p.setPen(QPen(QColor(226, 25, 47), 2.0, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(spinnerRect, (90 - spinnerAngle_) * 16, -255 * 16);
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(158, 164, 169));
        p.drawEllipse(QPointF(98.0, 23.0), 1.5, 1.5);
        p.drawEllipse(QPointF(104.0, 23.0), 1.5, 1.5);
        p.drawEllipse(QPointF(110.0, 23.0), 1.5, 1.5);
    }

    p.setPen(QColor(165, 171, 176));
    p.setFont(QFont(QStringLiteral("Segoe UI"), 6, QFont::Normal));
    p.drawText(QRect(91, 35, 38, 10), Qt::AlignCenter, diskText_);

    if (!logo_.isNull()) {
        const QRect logoRect(123, 2, 18, 18);
        p.save();
        p.setOpacity(1.0);
        p.drawPixmap(logoRect, logo_, logo_.rect());
        p.restore();
    }
}
