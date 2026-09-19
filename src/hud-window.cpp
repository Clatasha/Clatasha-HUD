#include "hud-window.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/config-file.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QByteArray>
#include <QCoreApplication>

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QScreen>
#include <QSettings>
#include <QStorageInfo>
#include <QStringList>

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
    presentMonProcess_ = new QProcess(this);
    presentMonPipeServer_ = new QLocalServer(this);
#ifdef Q_OS_WIN
    presentMonProcess_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#endif
    connect(presentMonProcess_, &QProcess::readyReadStandardOutput, this, [this]() {
        readPresentMonOutput();
    });
    connect(presentMonProcess_, &QProcess::readyReadStandardError, this, [this]() {
        const QByteArray chunk = presentMonProcess_->readAllStandardError();
        presentMonErrorBuffer_.append(chunk);

        const QByteArray lower = presentMonErrorBuffer_.toLower();
        if (lower.contains("administrative privileges") ||
            lower.contains("performance log users") ||
            lower.contains("access denied")) {
            presentMonAccessDenied_ = true;
        }

        if (!chunk.isEmpty())
            blog(LOG_WARNING, "[Clatasha HUD] PresentMon: %s", chunk.constData());
    });
    connect(presentMonProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        gameFpsValid_ = false;
    });
    connect(presentMonProcess_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int, QProcess::ExitStatus) {
                gameFpsValid_ = false;
                if (!stoppingPresentMon_ && presentMonAccessDenied_ &&
                    trackedGamePid_ != 0 && !elevatedPresentMonStarted_) {
                    QTimer::singleShot(0, this, [this]() {
                        startElevatedPresentMon(trackedGamePid_);
                    });
                }
            });

    connect(presentMonPipeServer_, &QLocalServer::newConnection, this, [this]() {
        if (presentMonPipeSocket_) {
            presentMonPipeSocket_->disconnect(this);
            presentMonPipeSocket_->deleteLater();
            presentMonPipeSocket_ = nullptr;
        }

        presentMonPipeSocket_ = presentMonPipeServer_->nextPendingConnection();
        if (!presentMonPipeSocket_)
            return;

        connect(presentMonPipeSocket_, &QLocalSocket::readyRead, this, [this]() {
            readPresentMonPipe();
        });
        connect(presentMonPipeSocket_, &QLocalSocket::disconnected, this, [this]() {
            gameFpsValid_ = false;
        });

        readPresentMonPipe();
    });

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
    stopPresentMon();

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
    presentMonBuffer_.clear();
    presentMonErrorBuffer_.clear();
    fpsChains_.clear();
    processIdColumn_ = -1;
    swapChainColumn_ = -1;
    cpuStartTimeColumn_ = -1;
    captureTimeScale_ = 1.0;
    parsedFrameLogCount_ = 0;
    gameFps_ = 0.0;
    gameFpsValid_ = false;
}

void ClatashaHudWindow::stopPresentMon()
{
    stoppingPresentMon_ = true;

    if (presentMonProcess_ && presentMonProcess_->state() != QProcess::NotRunning) {
        presentMonProcess_->kill();
        presentMonProcess_->waitForFinished(250);
    }

    if (presentMonPipeSocket_) {
        presentMonPipeSocket_->disconnect(this);
        presentMonPipeSocket_->abort();
        presentMonPipeSocket_->deleteLater();
        presentMonPipeSocket_ = nullptr;
    }

    if (presentMonPipeServer_ && presentMonPipeServer_->isListening()) {
        const QString name = presentMonPipeServer_->serverName();
        presentMonPipeServer_->close();
        QLocalServer::removeServer(name);
    }

#ifdef Q_OS_WIN
    if (elevatedPresentMonHandle_ != 0) {
        CloseHandle(reinterpret_cast<HANDLE>(elevatedPresentMonHandle_));
        elevatedPresentMonHandle_ = 0;
    }
#endif

    presentMonAccessDenied_ = false;
    elevatedPresentMonStarted_ = false;
    resetGameFps();
    stoppingPresentMon_ = false;
}

void ClatashaHudWindow::startPresentMon(quint32)
{
    if (!presentMonProcess_)
        return;

    if (presentMonProcess_->state() != QProcess::NotRunning || elevatedPresentMonStarted_)
        return;

    resetGameFps();
    presentMonAccessDenied_ = false;

    char *modulePath = obs_module_file("PresentMon-2.5.1-x64.exe");
    if (!modulePath) {
        blog(LOG_WARNING, "[Clatasha HUD] PresentMon executable path unavailable");
        return;
    }

    const QString executable = QString::fromUtf8(modulePath);
    bfree(modulePath);

    if (!QFileInfo::exists(executable)) {
        blog(LOG_WARNING, "[Clatasha HUD] PresentMon executable not found: %s",
             executable.toUtf8().constData());
        return;
    }

    QStringList args{
        QStringLiteral("--output_stdout"),
        QStringLiteral("--no_console_stats"),
        QStringLiteral("--no_track_gpu"),
        QStringLiteral("--no_track_input"),
        QStringLiteral("--exclude_dropped"),
        QStringLiteral("--no_track_display"),
        QStringLiteral("--session_name"),
        QStringLiteral("ClatashaHUD_%1").arg(QCoreApplication::applicationPid()),
        QStringLiteral("--stop_existing_session"),
    };

    presentMonProcess_->setProgram(executable);
    presentMonProcess_->setArguments(args);
    presentMonProcess_->setProcessChannelMode(QProcess::SeparateChannels);
    presentMonProcess_->start();

    blog(LOG_INFO, "[Clatasha HUD] Started global PresentMon collector");
}

void ClatashaHudWindow::startElevatedPresentMon(quint32)
{
#ifdef Q_OS_WIN
    if (!presentMonPipeServer_ || elevatedPresentMonStarted_)
        return;

    char *presentMonPathRaw = obs_module_file("PresentMon-2.5.1-x64.exe");
    char *helperPathRaw = obs_module_file("clatasha-fps-helper.exe");

    if (!presentMonPathRaw || !helperPathRaw) {
        if (presentMonPathRaw)
            bfree(presentMonPathRaw);
        if (helperPathRaw)
            bfree(helperPathRaw);
        blog(LOG_WARNING, "[Clatasha HUD] FPS helper or PresentMon path unavailable");
        return;
    }

    const QString presentMonPath = QString::fromUtf8(presentMonPathRaw);
    const QString helperPath = QString::fromUtf8(helperPathRaw);
    bfree(presentMonPathRaw);
    bfree(helperPathRaw);

    if (!QFileInfo::exists(presentMonPath) || !QFileInfo::exists(helperPath)) {
        blog(LOG_WARNING, "[Clatasha HUD] FPS helper or PresentMon executable missing");
        return;
    }

    const QString serverName =
        QStringLiteral("ClatashaHUD_%1").arg(GetCurrentProcessId());

    QLocalServer::removeServer(serverName);
    if (!presentMonPipeServer_->listen(serverName)) {
        blog(LOG_WARNING, "[Clatasha HUD] Could not create PresentMon pipe: %s",
             presentMonPipeServer_->errorString().toUtf8().constData());
        return;
    }

    const QString pipePath = presentMonPipeServer_->fullServerName();
    const QString parameters =
        QStringLiteral("\"%1\" \"%2\" %3")
            .arg(QDir::toNativeSeparators(presentMonPath))
            .arg(pipePath)
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
        const DWORD error = GetLastError();
        blog(LOG_WARNING, "[Clatasha HUD] Elevated FPS helper launch failed: %lu", error);
        presentMonPipeServer_->close();
        QLocalServer::removeServer(serverName);
        return;
    }

    elevatedPresentMonHandle_ = reinterpret_cast<quintptr>(info.hProcess);
    elevatedPresentMonStarted_ = true;
    resetGameFps();

    blog(LOG_INFO, "[Clatasha HUD] Elevated FPS helper started");
#else
    Q_UNUSED(pid);
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
    if (trackedGamePid_ != newPid) {
        trackedGamePid_ = newPid;
        fpsChains_.clear();
        gameFps_ = 0.0;
        gameFpsValid_ = false;

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
    }

    if (presentMonProcess_ &&
        presentMonProcess_->state() == QProcess::NotRunning &&
        !elevatedPresentMonStarted_) {
        startPresentMon(0);
    }
#endif
}


static QStringList splitCsvFields(const QString &line)
{
    QStringList fields;
    QString current;
    bool quoted = false;

    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);

        if (ch == QLatin1Char('"')) {
            if (quoted && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                current += QLatin1Char('"');
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (ch == QLatin1Char(',') && !quoted) {
            fields.push_back(current);
            current.clear();
        } else {
            current += ch;
        }
    }

    fields.push_back(current);
    return fields;
}

void ClatashaHudWindow::processPresentMonLine(const QByteArray &rawLine)
{
    QString line = QString::fromUtf8(rawLine).trimmed();
    if (line.isEmpty())
        return;

    if (!line.isEmpty() && line.front() == QChar(0xFEFF))
        line.remove(0, 1);

    const QStringList fields = splitCsvFields(line);

    if (processIdColumn_ < 0) {
        int processIndex = -1;
        int swapIndex = -1;
        int timeIndex = -1;
        double timeScale = 1.0;
        QString timeName;

        for (int i = 0; i < fields.size(); ++i) {
            const QString name = fields.at(i).trimmed();

            if (name.compare(QStringLiteral("ProcessID"), Qt::CaseInsensitive) == 0) {
                processIndex = i;
            } else if (name.compare(QStringLiteral("SwapChainAddress"), Qt::CaseInsensitive) == 0) {
                swapIndex = i;
            } else if (name.compare(QStringLiteral("CPUStartTime"), Qt::CaseInsensitive) == 0 ||
                       name.compare(QStringLiteral("CPUStartTimeInMs"), Qt::CaseInsensitive) == 0 ||
                       name.compare(QStringLiteral("CPUStartQPCTime"), Qt::CaseInsensitive) == 0 ||
                       name.compare(QStringLiteral("TimeInMs"), Qt::CaseInsensitive) == 0) {
                timeIndex = i;
                timeScale = 1.0;
                timeName = name;
            } else if (name.compare(QStringLiteral("CPUStartTimeInSeconds"), Qt::CaseInsensitive) == 0 ||
                       name.compare(QStringLiteral("TimeInSeconds"), Qt::CaseInsensitive) == 0) {
                timeIndex = i;
                timeScale = 1000.0;
                timeName = name;
            }
        }

        if (processIndex >= 0) {
            processIdColumn_ = processIndex;
            swapChainColumn_ = swapIndex;
            cpuStartTimeColumn_ = timeIndex;
            captureTimeScale_ = timeScale;

            blog(LOG_INFO,
                 "[Clatasha HUD] PresentMon CSV header detected: pid=%d swap=%d time=%d (%s)",
                 processIdColumn_,
                 swapChainColumn_,
                 cpuStartTimeColumn_,
                 timeName.isEmpty() ? "arrival-time fallback" : timeName.toUtf8().constData());
        } else if (line.contains(QStringLiteral("Application"), Qt::CaseInsensitive)) {
            blog(LOG_WARNING,
                 "[Clatasha HUD] PresentMon CSV header missing ProcessID: %s",
                 line.left(400).toUtf8().constData());
        }
        return;
    }

    if (fields.size() <= processIdColumn_)
        return;

    bool pidOk = false;
    const quint32 rowPid = fields.at(processIdColumn_).toUInt(&pidOk);
    if (!pidOk || rowPid == 0)
        return;

    double captureTimeMs = static_cast<double>(gameFpsClock_.elapsed());
    if (cpuStartTimeColumn_ >= 0 && fields.size() > cpuStartTimeColumn_) {
        bool timeOk = false;
        const double parsed = fields.at(cpuStartTimeColumn_).toDouble(&timeOk);
        if (timeOk && std::isfinite(parsed))
            captureTimeMs = parsed * captureTimeScale_;
    }

    QString swapChain = QStringLiteral("default");
    if (swapChainColumn_ >= 0 && fields.size() > swapChainColumn_) {
        const QString parsedSwap = fields.at(swapChainColumn_).trimmed();
        if (!parsedSwap.isEmpty())
            swapChain = parsedSwap;
    }

    if (rowPid != trackedGamePid_)
        return;

    auto &samples = fpsChains_[swapChain];
    if (!samples.captureTimesMs.isEmpty()) {
        const double last = samples.captureTimesMs.back();

        if (captureTimeMs < last) {
            samples.captureTimesMs.clear();
        } else if (captureTimeMs == last) {
            samples.lastSeenMs = gameFpsClock_.elapsed();
            return;
        }
    }

    samples.captureTimesMs.push_back(captureTimeMs);
    samples.lastSeenMs = gameFpsClock_.elapsed();

    const double cutoff = captureTimeMs - 1000.0;
    while (samples.captureTimesMs.size() > 2 &&
           samples.captureTimesMs.front() < cutoff) {
        samples.captureTimesMs.removeFirst();
    }

    if (parsedFrameLogCount_ < 3) {
        blog(LOG_INFO,
             "[Clatasha HUD] Parsed game frame: pid=%u swap=%s time=%.3f",
             rowPid,
             swapChain.toUtf8().constData(),
             captureTimeMs);
        ++parsedFrameLogCount_;
    }
}

void ClatashaHudWindow::readPresentMonOutput()
{
    if (!presentMonProcess_)
        return;

    presentMonBuffer_.append(presentMonProcess_->readAllStandardOutput());

    int newline = -1;
    while ((newline = presentMonBuffer_.indexOf('\n')) >= 0) {
        const QByteArray line = presentMonBuffer_.left(newline);
        presentMonBuffer_.remove(0, newline + 1);
        processPresentMonLine(line);
    }
}

void ClatashaHudWindow::readPresentMonPipe()
{
    if (!presentMonPipeSocket_)
        return;

    presentMonBuffer_.append(presentMonPipeSocket_->readAll());

    int newline = -1;
    while ((newline = presentMonBuffer_.indexOf('\n')) >= 0) {
        const QByteArray line = presentMonBuffer_.left(newline);
        presentMonBuffer_.remove(0, newline + 1);
        processPresentMonLine(line);
    }
}

void ClatashaHudWindow::updateGameFps()
{
    const qint64 now = gameFpsClock_.elapsed();
    double bestFps = 0.0;
    bool found = false;

    for (auto it = fpsChains_.begin(); it != fpsChains_.end();) {
        if (now - it->lastSeenMs > 5000) {
            it = fpsChains_.erase(it);
            continue;
        }

        const auto &times = it->captureTimesMs;
        if (now - it->lastSeenMs <= 2500 && times.size() >= 3) {
            const double spanMs = times.back() - times.front();

            if (spanMs >= 120.0) {
                const double fps =
                    (static_cast<double>(times.size()) - 1.0) * 1000.0 / spanMs;

                if (std::isfinite(fps) && fps > 0.0 && fps < 2000.0 &&
                    (!found || fps > bestFps)) {
                    bestFps = fps;
                    found = true;
                }
            }
        }

        ++it;
    }

    if (found) {
        gameFps_ = bestFps;
        gameFpsValid_ = true;
    } else {
        gameFps_ = 0.0;
        gameFpsValid_ = false;
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
    updateGameFps();

    static int fpsDiagnosticTicks = 0;
    if (++fpsDiagnosticTicks >= 50) {
        fpsDiagnosticTicks = 0;
        blog(LOG_INFO,
             "[Clatasha HUD] FPS status: target_pid=%u collector=%s elevated=%s fps=%s",
             trackedGamePid_,
             presentMonProcess_ && presentMonProcess_->state() != QProcess::NotRunning ? "running" : "stopped",
             elevatedPresentMonStarted_ ? "yes" : "no",
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

    drawSegmentedMeter(p, 5, 8, 3, 32, desktopLevel_.load(std::memory_order_relaxed));
    drawSegmentedMeter(p, 11, 8, 3, 32, micLevel_.load(std::memory_order_relaxed));

    const bool sessionActive = recordingActive_ || streamingActive_;
    const QColor gameFpsColor = sessionActive ? QColor(232, 24, 43) : QColor(45, 143, 255);
    const QString gameFpsText = gameFpsValid_
                                    ? QString::number(static_cast<int>(std::lround(gameFps_)))
                                    : QStringLiteral("--");
    const QString obsFpsText =
        QStringLiteral("/%1").arg(static_cast<int>(std::lround(obsFps_)));

    p.setPen(gameFpsColor);
    const int gameFontSize = gameFpsText.size() >= 3 ? 21 : 24;
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
