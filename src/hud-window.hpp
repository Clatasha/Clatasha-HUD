#pragma once

#include <atomic>

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <obs.h>
#include <obs-audio-controls.h>

class ClatashaHudWindow final : public QWidget {
public:
    explicit ClatashaHudWindow(QWidget *parent = nullptr);
    ~ClatashaHudWindow() override;

    int opacityPercent() const { return opacityPercent_; }
    QString location() const { return location_; }
    bool recordingVisibilityWarningsEnabled() const
    {
        return recordingVisibilityWarningsEnabled_;
    }
    const QPixmap &logoPixmap() const { return logo_; }

    void setOpacityPercent(int value);
    void setLocation(const QString &location);
    void setRecordingVisibilityWarningsEnabled(bool enabled);
    void positionHud();
    void saveSettings() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void refresh();
    void refreshAudioSources();
    void attachDesktopSource(obs_source_t *source);
    void attachMicSource(obs_source_t *source);
    void loadSettings();
    void loadLogo();
    void updateForegroundGame();
    void startFpsHelper();
    void stopFpsHelper();
    void readFpsState();
    void resetGameFps();
    void updateHudRenderPathWarning(bool fullscreenHudActive);
    void showRecordingVisibilityWarning(bool recorded);
    int recordingVisibilityWarningHeight() const;
    QString fpsStateFilePath() const;
    QString recordingPath() const;
    QString diskSpaceText() const;
    QString settingsFilePath() const;

    static QString formatElapsed(qint64 milliseconds);
    static float meterValue(const float peak[MAX_AUDIO_CHANNELS]);
    static void desktopMeterUpdated(void *param,
                                    const float magnitude[MAX_AUDIO_CHANNELS],
                                    const float peak[MAX_AUDIO_CHANNELS],
                                    const float inputPeak[MAX_AUDIO_CHANNELS]);
    static void micMeterUpdated(void *param,
                                const float magnitude[MAX_AUDIO_CHANNELS],
                                const float peak[MAX_AUDIO_CHANNELS],
                                const float inputPeak[MAX_AUDIO_CHANNELS]);

    QTimer refreshTimer_;
    QElapsedTimer sessionTimer_;
    QElapsedTimer gameFpsClock_;
    QElapsedTimer recordingWarningClock_;
    quintptr fpsHelperHandle_ = 0;
    bool fpsHelperStarted_ = false;
    int fpsHelperRestartAttempts_ = 0;
    int fpsHelperStableChecks_ = 0;
    qint64 lastFpsStateMtimeMs_ = 0;
    qint64 lastValidGameFpsMs_ = -1;

    obs_volmeter_t *desktopMeter_ = nullptr;
    obs_volmeter_t *micMeter_ = nullptr;
    obs_source_t *desktopSource_ = nullptr;
    obs_source_t *micSource_ = nullptr;

    std::atomic<float> desktopLevel_{0.0f};
    std::atomic<float> micLevel_{0.0f};

    QPixmap logo_;

    bool sessionWasActive_ = false;
    bool recordingActive_ = false;
    bool streamingActive_ = false;
    bool replayBufferActive_ = false;

    double obsFps_ = 0.0;
    double gameFps_ = 0.0;
    bool gameFpsValid_ = false;
    quint32 trackedGamePid_ = 0;
    QString trackedGameExecutable_;
    QString detectedGameRenderer_;
    QString gameRenderer_;
    QString openGlHookStatus_;
    bool openGlDrawArmed_ = false;
    quint64 openGlDrawCount_ = 0;
    int openGlRenderMode_ = 0;
    int openGlDrawStage_ = 0;
    int openGlLiveFpsSent_ = 0;
    int openGlLiveFpsAck_ = 0;
    int openGlLiveObsFpsSent_ = 0;
    int openGlLiveObsFpsAck_ = 0;
    int openGlLiveTimerSent_ = 0;
    int openGlLiveTimerAck_ = 0;
    int openGlLiveAudioSent_ = 0;
    int openGlLiveAudioAck_ = 0;
    int openGlLiveStatusSent_ = 0;
    int openGlLiveStatusAck_ = 0;
    // -1 unknown, 0 Clatasha injected before OBS graphics-hook,
    // 1 OBS graphics-hook was already resident.
    int obsHookPreexisting_ = -1;
    bool gameFullscreen_ = false;
    int rendererMissSamples_ = 0;
    int gameTargetRefreshTicks_ = 0;
    int fpsStateRefreshTicks_ = 0;
    QString timerText_ = QStringLiteral("0:00:00");
    QString diskText_ = QStringLiteral("-- GB");

    int spinnerAngle_ = 0;
    int replayShimmerTicks_ = 0;
    int audioRefreshTicks_ = 0;
    int opacityPercent_ = 50;
    QString location_ = QStringLiteral("top-right");
    bool recordingVisibilityWarningsEnabled_ = true;
    bool hudRenderPathKnown_ = false;
    bool fullscreenHudActive_ = false;
    bool recordingWarningRecorded_ = false;
    qint64 recordingWarningStartMs_ = -1;
};
