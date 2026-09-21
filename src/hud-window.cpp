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
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSettings>
#include <QStorageInfo>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
constexpr int kHudWidth = 145;
constexpr int kHudHeight = 50;
constexpr int kScreenMargin = 12;
constexpr qint64 kFpsHoldMs = 2000;
constexpr double kFpsSmoothingAlpha = 0.45;

const char kLogoBase64[] =
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

    gameFpsClock_.start();
    startFpsHelper();

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
    stopFpsHelper();

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
    const QByteArray bytes = QByteArray::fromBase64(kLogoBase64);
    const bool loaded = logo_.loadFromData(bytes, "PNG");
    blog(loaded ? LOG_INFO : LOG_ERROR,
         "[Clatasha HUD] Embedded logo %s",
         loaded ? "loaded" : "failed to load");
}


void ClatashaHudWindow::resetGameFps()
{
    gameFps_ = 0.0;
    gameFpsValid_ = false;
    lastValidGameFpsMs_ = -1;
}

QString ClatashaHudWindow::fpsStateFilePath() const
{
    char *path = obs_module_config_path("fps-state.csv");
    if (!path)
        return {};

    const QString result = QString::fromUtf8(path);
    bfree(path);
    return result;
}

void ClatashaHudWindow::stopFpsHelper()
{
#ifdef Q_OS_WIN
    if (fpsHelperHandle_ != 0) {
        HANDLE process = reinterpret_cast<HANDLE>(fpsHelperHandle_);
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 1000);
        }
        CloseHandle(process);
        fpsHelperHandle_ = 0;
    }
#endif
    fpsHelperStarted_ = false;
    resetGameFps();
}

void ClatashaHudWindow::startFpsHelper()
{
#ifdef Q_OS_WIN
    if (fpsHelperStarted_)
        return;

    char *helperPathRaw = obs_module_file("clatasha-fps-helper.exe");
    if (!helperPathRaw) {
        blog(LOG_WARNING, "[Clatasha HUD] FPS helper path unavailable");
        return;
    }

    const QString helperPath = QString::fromUtf8(helperPathRaw);
    bfree(helperPathRaw);

    if (!QFileInfo::exists(helperPath)) {
        blog(LOG_WARNING, "[Clatasha HUD] FPS helper executable missing: %s",
             helperPath.toUtf8().constData());
        return;
    }

    const QString statePath = fpsStateFilePath();
    if (statePath.isEmpty()) {
        blog(LOG_WARNING, "[Clatasha HUD] FPS state path unavailable");
        return;
    }

    QDir().mkpath(QFileInfo(statePath).absolutePath());
    QFile::remove(statePath);

    const QString parameters =
        QStringLiteral("\"%1\" %2")
            .arg(QDir::toNativeSeparators(statePath))
            .arg(GetCurrentProcessId());

    const std::wstring helperWide = QDir::toNativeSeparators(helperPath).toStdWString();
    const std::wstring paramsWide = parameters.toStdWString();

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = helperWide.c_str();
    info.lpParameters = paramsWide.c_str();
    info.nShow = SW_HIDE;

    if (!ShellExecuteExW(&info)) {
        blog(LOG_WARNING, "[Clatasha HUD] FPS helper launch failed: %lu", GetLastError());
        return;
    }

    fpsHelperHandle_ = reinterpret_cast<quintptr>(info.hProcess);
    fpsHelperStarted_ = true;
    blog(LOG_INFO, "[Clatasha HUD] Direct ETW FPS helper started");
#endif
}

void ClatashaHudWindow::updateForegroundGame()
{
#ifdef Q_OS_WIN
    HWND foreground = GetForegroundWindow();
    if (!foreground)
        return;

    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);

    if (pid == 0 || pid == GetCurrentProcessId())
        return;

    const quint32 newPid = static_cast<quint32>(pid);
    if (trackedGamePid_ == newPid)
        return;

    trackedGamePid_ = newPid;
    resetGameFps();

    wchar_t imagePath[MAX_PATH] = {};
    QString processName;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, imagePath, &size))
            processName = QFileInfo(QString::fromWCharArray(imagePath)).fileName();
        CloseHandle(process);
    }

    if (processName.isEmpty()) {
        blog(LOG_INFO, "[Clatasha HUD] FPS target PID %u", newPid);
    } else {
        blog(LOG_INFO, "[Clatasha HUD] FPS target PID %u (%s)",
             newPid, processName.toUtf8().constData());
    }
#endif
}

void ClatashaHudWindow::readFpsState()
{
    const qint64 now = gameFpsClock_.elapsed();

    const auto expireIfStale = [this, now]() {
        if (gameFpsValid_ && lastValidGameFpsMs_ >= 0 &&
            now - lastValidGameFpsMs_ >= kFpsHoldMs) {
            gameFps_ = 0.0;
            gameFpsValid_ = false;
            lastValidGameFpsMs_ = -1;
        }
    };

    const QString path = fpsStateFilePath();
    if (path.isEmpty()) {
        expireIfStale();
        return;
    }

    QFileInfo info(path);
    if (!info.exists()) {
        expireIfStale();
        return;
    }

    const qint64 modified = info.lastModified().toMSecsSinceEpoch();
    if (modified == lastFpsStateMtimeMs_) {
        expireIfStale();
        return;
    }

    lastFpsStateMtimeMs_ = modified;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        expireIfStale();
        return;
    }

    bool found = false;
    double fps = 0.0;

    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;

        const QList<QByteArray> fields = line.split(',');
        if (fields.size() != 2)
            continue;

        bool pidOk = false;
        bool fpsOk = false;
        const quint32 pid = fields.at(0).toUInt(&pidOk);
        const double value = fields.at(1).toDouble(&fpsOk);

        if (pidOk && fpsOk && pid == trackedGamePid_ &&
            std::isfinite(value) && value > 0.0 && value < 2000.0) {
            fps = value;
            found = true;
            break;
        }
    }

    if (found) {
        if (gameFpsValid_) {
            const double delta = std::abs(fps - gameFps_);
            const double relativeDelta = gameFps_ > 1.0 ? delta / gameFps_ : 1.0;
            const double alpha = relativeDelta >= 0.20 ? 0.70 : kFpsSmoothingAlpha;
            gameFps_ += (fps - gameFps_) * alpha;
        } else {
            gameFps_ = fps;
        }

        gameFpsValid_ = true;
        lastValidGameFpsMs_ = now;
    } else {
        expireIfStale();
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

    obsFps_ = obs_get_active_fps();
    spinnerAngle_ = sessionActive ? (spinnerAngle_ + 20) % 360 : 0;

    if (++gameTargetRefreshTicks_ >= 5) {
        gameTargetRefreshTicks_ = 0;
        updateForegroundGame();
    }

    if (++fpsStateRefreshTicks_ >= 2) {
        fpsStateRefreshTicks_ = 0;
        readFpsState();
    }

    static int fpsDiagnosticTicks = 0;
    if (++fpsDiagnosticTicks >= 50) {
        fpsDiagnosticTicks = 0;
        bool helperAlive = false;
#ifdef Q_OS_WIN
        if (fpsHelperHandle_ != 0)
            helperAlive =
                WaitForSingleObject(reinterpret_cast<HANDLE>(fpsHelperHandle_), 0) == WAIT_TIMEOUT;
#endif
        blog(LOG_INFO,
             "[Clatasha HUD] FPS status: target_pid=%u helper=%s fps=%s",
             trackedGamePid_,
             helperAlive ? "running" : "stopped",
             gameFpsValid_ ? QString::number(gameFps_, 'f', 1).toUtf8().constData() : "--");
    }

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

    drawSegmentedMeter(p, 4, 5, 3, 29, desktopLevel_.load(std::memory_order_relaxed));
    drawSegmentedMeter(p, 13, 5, 3, 29, micLevel_.load(std::memory_order_relaxed));

    // Tiny source-identification icons under the audio meters:
    // monitor for Desktop Audio, microphone for Mic/Aux.
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(174, 181, 187), 0.85));
    p.setBrush(Qt::NoBrush);

    // Desktop monitor icon, deliberately separated from the microphone.
    p.drawRoundedRect(QRectF(1.5, 37.5, 7.0, 5.0), 0.8, 0.8);
    p.drawLine(QPointF(5.0, 42.5), QPointF(5.0, 44.5));
    p.drawLine(QPointF(3.0, 44.5), QPointF(7.0, 44.5));

    // Microphone icon.
    p.drawRoundedRect(QRectF(11.7, 37.2, 3.6, 5.8), 1.8, 1.8);
    p.drawArc(QRectF(10.8, 39.6, 5.4, 4.8), 180 * 16, 180 * 16);
    p.drawLine(QPointF(13.5, 44.3), QPointF(13.5, 46.0));
    p.drawLine(QPointF(11.8, 46.0), QPointF(15.2, 46.0));
    p.restore();

    const bool sessionActive = recordingActive_ || streamingActive_;
    const QColor gameFpsColor = sessionActive ? QColor(232, 24, 43) : QColor(45, 143, 255);
    const QString gameFpsText = gameFpsValid_
                                    ? QString::number(static_cast<int>(std::lround(gameFps_)))
                                    : QStringLiteral("--");
    const QString obsFpsText =
        QStringLiteral("/%1").arg(static_cast<int>(std::lround(obsFps_)));

    p.setPen(gameFpsColor);
    const int gameFontSize = gameFpsText.size() >= 4 ? 19 : 24;
    p.setFont(QFont(QStringLiteral("Segoe UI"), gameFontSize, QFont::Bold));
    p.drawText(QRect(16, -3, 58, 36), Qt::AlignRight | Qt::AlignVCenter, gameFpsText);

    p.setPen(QColor(165, 171, 176));
    p.setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold));
    p.drawText(QRect(74, 4, 29, 23), Qt::AlignLeft | Qt::AlignVCenter, obsFpsText);

    p.setPen(QColor(205, 209, 212));
    p.setFont(QFont(QStringLiteral("Segoe UI"), 8, QFont::Normal));
    p.drawText(QRect(23, 29, 69, 14), Qt::AlignCenter, timerText_);
    const QPointF spinnerCenter(110.0, 24.0);
    const QRectF spinnerRect(spinnerCenter.x() - 9.0, spinnerCenter.y() - 9.0, 18.0, 18.0);

    if (sessionActive) {
        p.setPen(QPen(QColor(226, 25, 47), 2.0, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(spinnerRect, (90 - spinnerAngle_) * 16, -255 * 16);
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(158, 164, 169));
        p.drawEllipse(QPointF(104.0, 24.0), 1.5, 1.5);
        p.drawEllipse(QPointF(110.0, 24.0), 1.5, 1.5);
        p.drawEllipse(QPointF(116.0, 24.0), 1.5, 1.5);
    }

    p.setPen(QColor(165, 171, 176));
    p.setFont(QFont(QStringLiteral("Segoe UI"), 7, QFont::Normal));
    p.drawText(QRect(94, 37, 38, 11), Qt::AlignCenter, diskText_);

    if (!logo_.isNull()) {
        const QRect logoRect(129, 2, 14, 14);
        p.save();
        p.setOpacity(opacityPercent_ / 100.0);
        p.drawPixmap(logoRect, logo_, logo_.rect());
        p.restore();
    }
}
