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
"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"
"SUVORK5CYII=";


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
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                   Qt::WindowDoesNotAcceptFocus);
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

    const std::wstring helperWide =
        QDir::toNativeSeparators(helperPath).toStdWString();
    const std::wstring paramsWide = parameters.toStdWString();

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = helperWide.c_str();
    info.lpParameters = paramsWide.c_str();
    info.nShow = SW_HIDE;

    if (!ShellExecuteExW(&info)) {
        blog(LOG_WARNING,
             "[Clatasha HUD] FPS helper elevated launch failed: %lu",
             GetLastError());
        return;
    }

    fpsHelperHandle_ = reinterpret_cast<quintptr>(info.hProcess);
    fpsHelperStarted_ = true;
    lastFpsStateMtimeMs_ = 0;
    blog(LOG_INFO, "[Clatasha HUD] Direct ETW FPS helper started elevated");
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
    trackedGameExecutable_.clear();
    detectedGameRenderer_.clear();
    gameRenderer_.clear();
    openGlHookStatus_.clear();
    openGlDrawArmed_ = false;
    openGlDrawCount_ = 0;
    openGlRenderMode_ = 0;
    openGlDrawStage_ = 0;
    gameFullscreen_ = false;
    rendererMissSamples_ = 0;
    update();

    wchar_t imagePath[MAX_PATH] = {};
    QString processName;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, imagePath, &size))
            processName = QFileInfo(QString::fromWCharArray(imagePath)).fileName();
        CloseHandle(process);
    }

    trackedGameExecutable_ = processName;

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
    bool targetRowFound = false;
    double fps = 0.0;
    QString renderer;
    QString openGlHookStatus;
    bool openGlDrawArmed = false;
    quint64 openGlDrawCount = 0;
    int openGlRenderMode = 0;
    int openGlDrawStage = 0;
    bool fullscreen = false;

    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;

        const QList<QByteArray> fields = line.split(',');
        if (fields.size() < 2)
            continue;

        bool pidOk = false;
        bool fpsOk = false;
        const quint32 pid = fields.at(0).toUInt(&pidOk);
        const double value = fields.at(1).toDouble(&fpsOk);

        if (!pidOk || pid != trackedGamePid_)
            continue;

        targetRowFound = true;
        if (fields.size() >= 3)
            renderer = QString::fromUtf8(fields.at(2)).trimmed().toUpper();
        if (fields.size() >= 4)
            fullscreen = fields.at(3).trimmed() == "1";
        if (fields.size() >= 5)
            openGlHookStatus =
                QString::fromUtf8(fields.at(4)).trimmed().toLower();
        if (fields.size() >= 7)
            openGlDrawArmed = fields.at(6).trimmed() == "1";
        if (fields.size() >= 8) {
            bool drawCountOk = false;
            const quint64 parsedDrawCount =
                fields.at(7).toULongLong(&drawCountOk);
            if (drawCountOk)
                openGlDrawCount = parsedDrawCount;
        }
        if (fields.size() >= 9) {
            bool renderModeOk = false;
            const int parsedRenderMode =
                fields.at(8).toInt(&renderModeOk);
            if (renderModeOk)
                openGlRenderMode = parsedRenderMode;
        }
        if (fields.size() >= 10) {
            bool drawStageOk = false;
            const int parsedDrawStage =
                fields.at(9).toInt(&drawStageOk);
            if (drawStageOk)
                openGlDrawStage = parsedDrawStage;
        }

        if (fpsOk && std::isfinite(value) && value > 0.0 && value < 2000.0) {
            fps = value;
            found = true;
        }
        break;
    }

    if (targetRowFound && !renderer.isEmpty()) {
        rendererMissSamples_ = 0;
        detectedGameRenderer_ = renderer;
    } else if (++rendererMissSamples_ >= 4) {
        detectedGameRenderer_.clear();
        rendererMissSamples_ = 4;
    }

    const QString activeRenderer =
        fullscreen ? detectedGameRenderer_ : QString();
    const bool stateChanged =
        activeRenderer != gameRenderer_ || fullscreen != gameFullscreen_;

    if (stateChanged) {
        gameRenderer_ = activeRenderer;
        gameFullscreen_ = fullscreen;

        if (gameFullscreen_ && !gameRenderer_.isEmpty()) {
            blog(LOG_INFO,
                 "[Clatasha HUD] Fullscreen renderer switch candidate PID %u: %s",
                 trackedGamePid_,
                 gameRenderer_.toUtf8().constData());
        } else if (!gameFullscreen_) {
            blog(LOG_INFO,
                 "[Clatasha HUD] Renderer target PID %u returned to desktop/windowed mode",
                 trackedGamePid_);
        }
    }

    if (openGlHookStatus != openGlHookStatus_) {
        openGlHookStatus_ = openGlHookStatus;
        if (!openGlHookStatus_.isEmpty()) {
            blog(LOG_INFO,
                 "[Clatasha HUD] OpenGL present hook PID %u: %s",
                 trackedGamePid_,
                 openGlHookStatus_.toUtf8().constData());
        }
    }

    if (openGlDrawArmed != openGlDrawArmed_) {
        openGlDrawArmed_ = openGlDrawArmed;
        blog(LOG_INFO,
             "[Clatasha HUD] OpenGL fullscreen HUD PID %u: %s",
             trackedGamePid_,
             openGlDrawArmed_ ? "armed" : "idle");
    }
    openGlDrawCount_ = openGlDrawCount;

    if (openGlRenderMode != openGlRenderMode_) {
        openGlRenderMode_ = openGlRenderMode;
        if (openGlRenderMode_ != 0) {
            blog(LOG_INFO,
                 "[Clatasha HUD] OpenGL fullscreen renderer PID %u: %s",
                 trackedGamePid_,
                 openGlRenderMode_ == 1 ? "modern" : "fallback");
        }
    }

    if (openGlDrawStage != openGlDrawStage_) {
        openGlDrawStage_ = openGlDrawStage;
        const char *stageName = "unknown";
        switch (openGlDrawStage_) {
        case 0: stageName = "idle"; break;
        case 10: stageName = "eligible"; break;
        case 11: stageName = "renderer-entry"; break;
        case 15: stageName = "fallback-entry"; break;
        case 20: stageName = "renderer-ready"; break;
        case 30: stageName = "viewport-ready"; break;
        case 40: stageName = "state-captured"; break;
        case 50: stageName = "overlay-state"; break;
        case 60: stageName = "vertices-uploaded"; break;
        case 70: stageName = "draw-returned"; break;
        case 80: stageName = "state-restored"; break;
        case 90: stageName = "complete"; break;
        }
        blog(LOG_INFO,
             "[Clatasha HUD] OpenGL draw stage PID %u: %d (%s)",
             trackedGamePid_,
             openGlDrawStage_,
             stageName);
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
    replayBufferActive_ = obs_frontend_replay_buffer_active();

    if (replayBufferActive_)
        replayShimmerTicks_ = (replayShimmerTicks_ + 1) % 1000000;
    else
        replayShimmerTicks_ = 0;

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
        if (fpsHelperHandle_ != 0) {
            HANDLE process = reinterpret_cast<HANDLE>(fpsHelperHandle_);
            const DWORD waitResult = WaitForSingleObject(process, 0);
            helperAlive = waitResult == WAIT_TIMEOUT;

            if (!helperAlive) {
                DWORD exitCode = 0;
                GetExitCodeProcess(process, &exitCode);
                CloseHandle(process);
                fpsHelperHandle_ = 0;
                fpsHelperStarted_ = false;
                fpsHelperStableChecks_ = 0;
                resetGameFps();

                blog(LOG_WARNING,
                     "[Clatasha HUD] FPS helper stopped unexpectedly (exit code %lu)",
                     exitCode);

                // A successful ShellExecute only means Windows launched the
                // helper. ETW initialization can still fail immediately
                // afterward. Retry once rather than leaving the HUD stuck at
                // "--" for the rest of the OBS session.
                if (fpsHelperRestartAttempts_ < 1) {
                    ++fpsHelperRestartAttempts_;
                    blog(LOG_INFO,
                         "[Clatasha HUD] Retrying FPS helper after early exit");
                    startFpsHelper();
                }
            } else {
                if (++fpsHelperStableChecks_ >= 6) {
                    // After roughly 30 seconds of stable operation, allow one
                    // future recovery attempt if the helper later terminates.
                    fpsHelperStableChecks_ = 6;
                    fpsHelperRestartAttempts_ = 0;
                }
            }
        }
#endif
        const char *openGlStageName = "unknown";
        switch (openGlDrawStage_) {
        case 0: openGlStageName = "idle"; break;
        case 10: openGlStageName = "eligible"; break;
        case 11: openGlStageName = "renderer-entry"; break;
        case 15: openGlStageName = "fallback-entry"; break;
        case 20: openGlStageName = "renderer-ready"; break;
        case 30: openGlStageName = "viewport-ready"; break;
        case 40: openGlStageName = "state-captured"; break;
        case 50: openGlStageName = "overlay-state"; break;
        case 60: openGlStageName = "vertices-uploaded"; break;
        case 70: openGlStageName = "draw-returned"; break;
        case 80: openGlStageName = "state-restored"; break;
        case 90: openGlStageName = "complete"; break;
        }

        blog(LOG_INFO,
             "[Clatasha HUD] FPS status: target_pid=%u helper=%s fps=%s ogl_hook=%s ogl_draw=%s draws=%llu ogl_render=%s ogl_stage=%d(%s)",
             trackedGamePid_,
             helperAlive ? "running" : "stopped",
             gameFpsValid_ ? QString::number(gameFps_, 'f', 1).toUtf8().constData() : "--",
             openGlHookStatus_.isEmpty()
                 ? "off"
                 : openGlHookStatus_.toUtf8().constData(),
             openGlDrawArmed_ ? "armed" : "idle",
             static_cast<unsigned long long>(openGlDrawCount_),
             openGlRenderMode_ == 1
                 ? "modern"
                 : (openGlRenderMode_ == 2 ? "fallback" : "idle"),
             openGlDrawStage_,
             openGlStageName);
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

    drawSegmentedMeter(p, 5, 5, 3, 29, desktopLevel_.load(std::memory_order_relaxed));
    drawSegmentedMeter(p, 17, 5, 3, 29, micLevel_.load(std::memory_order_relaxed));

    // Tiny source-identification icons under the audio meters:
    // monitor for Desktop Audio, microphone for Mic/Aux.
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(174, 181, 187), 0.85));
    p.setBrush(Qt::NoBrush);

    // Desktop monitor icon, deliberately separated from the microphone.
    p.drawRoundedRect(QRectF(2.5, 37.5, 7.0, 5.0), 0.8, 0.8);
    p.drawLine(QPointF(6.0, 42.5), QPointF(6.0, 44.5));
    p.drawLine(QPointF(4.0, 44.5), QPointF(8.0, 44.5));

    // Microphone icon.
    p.drawRoundedRect(QRectF(15.2, 37.2, 3.6, 5.8), 1.8, 1.8);
    p.drawArc(QRectF(14.3, 39.6, 5.4, 4.8), 180 * 16, 180 * 16);
    p.drawLine(QPointF(17.0, 44.3), QPointF(17.0, 46.0));
    p.drawLine(QPointF(15.3, 46.0), QPointF(18.7, 46.0));
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
    p.drawText(QRect(22, -3, 52, 36), Qt::AlignRight | Qt::AlignVCenter, gameFpsText);

    p.setPen(QColor(165, 171, 176));
    p.setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold));
    p.drawText(QRect(74, 4, 29, 23), Qt::AlignLeft | Qt::AlignVCenter, obsFpsText);

    if (!gameRenderer_.isEmpty()) {
        const QRectF rendererRect(102.0, 2.0, 23.0, 10.0);
        p.setPen(QPen(QColor(96, 106, 116, 170), 0.8));
        p.setBrush(QColor(22, 27, 32, 205));
        p.drawRoundedRect(rendererRect, 2.0, 2.0);

        p.setPen(QColor(210, 216, 221));
        p.setFont(QFont(QStringLiteral("Segoe UI"), 6, QFont::Bold));
        p.drawText(rendererRect, Qt::AlignCenter, gameRenderer_);
    }

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

    // Replay Buffer indicator. The bolt is hidden while Replay Buffer is off
    // and uses a soft electric shimmer while it is armed.
    if (replayBufferActive_) {
        const double wave = (std::sin(replayShimmerTicks_ * 0.42) + 1.0) * 0.5;
        const int glowAlpha = 28 + static_cast<int>(54.0 * wave);
        const int boltAlpha = 190 + static_cast<int>(65.0 * wave);

        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);

        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 204, 64, glowAlpha));
        p.drawEllipse(QPointF(135.0, 29.5), 6.5 + wave, 7.0 + wave);

        QPainterPath bolt;
        bolt.moveTo(135.4, 21.0);
        bolt.lineTo(130.7, 29.4);
        bolt.lineTo(134.2, 29.4);
        bolt.lineTo(132.4, 37.2);
        bolt.lineTo(139.4, 27.1);
        bolt.lineTo(135.8, 27.1);
        bolt.closeSubpath();

        p.setPen(QPen(QColor(255, 248, 212, boltAlpha), 0.75));
        p.setBrush(QColor(255, 205, 55, boltAlpha));
        p.drawPath(bolt);

        // Small traveling glint makes the indicator read as shimmering rather
        // than simply blinking.
        const double glintY = 23.0 + std::fmod(replayShimmerTicks_ * 0.85, 10.0);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 150 + static_cast<int>(90.0 * wave)));
        p.drawEllipse(QPointF(136.7, glintY), 0.85, 0.85);

        p.restore();
    }

    if (!logo_.isNull()) {
        const QRect logoRect(127, 1, 16, 16);
        p.save();
        p.setOpacity(opacityPercent_ / 100.0);
        p.drawPixmap(logoRect, logo_, logo_.rect());
        p.restore();
    }
}
