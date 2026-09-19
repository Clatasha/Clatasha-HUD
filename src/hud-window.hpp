#pragma once

#include <atomic>

#include <QElapsedTimer>
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

    void setOpacityPercent(int value);
    void setLocation(const QString &location);
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

    double fps_ = 0.0;
    QString timerText_ = QStringLiteral("0:00:00");
    QString diskText_ = QStringLiteral("-- GB");

    int spinnerAngle_ = 0;
    int audioRefreshTicks_ = 0;
    int opacityPercent_ = 50;
    QString location_ = QStringLiteral("top-right");
};
