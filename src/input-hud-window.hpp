#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include <QWidget>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

class QPainter;

class ClatashaInputHudWindow final : public QWidget {
public:
    enum MouseControl : quint32 {
        MouseLeft = 1u << 0,
        MouseRight = 1u << 1,
        MouseMiddle = 1u << 2,
        MouseWheel = 1u << 3,
        MouseFront = 1u << 4,
        MouseBack = 1u << 5,
        MouseAll =
            MouseLeft |
            MouseRight |
            MouseMiddle |
            MouseWheel |
            MouseFront |
            MouseBack,
    };

    explicit ClatashaInputHudWindow(QWidget *parent = nullptr);
    ~ClatashaInputHudWindow() override;

    bool enabled() const { return enabled_; }
    int opacityPercent() const { return opacityPercent_; }
    int scalePercent() const { return scalePercent_; }
    QStringList selectedKeyIds() const { return selectedKeyIds_; }
    quint32 mouseControls() const { return mouseControls_; }
    int outputMode() const { return outputMode_; }
    QImage latestFrame() const;

    static QStringList fpsDefaultKeyIds();

    void setEnabled(bool enabled);
    void setOpacityPercent(int value);
    void setScalePercent(int value);
    void setSelectedKeyIds(const QStringList &ids);
    void setMouseControls(quint32 controls);
    void setOutputMode(int mode);
    void positionHud();
    void saveSettings() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct KeyDef {
        QString id;
        QString label;
        int vk = 0;
        QRectF rect;
    };

    void loadSettings();
    void rebuildLayout();
    void pollInputs();
    bool keyDown(int vk) const;
    void drawKey(QPainter &p, const KeyDef &key, bool active);
    void drawMouse(QPainter &p);
    void drawContent(QPainter &p);
    void refreshRenderedFrame();
    QString settingsFilePath() const;

#ifdef Q_OS_WIN
    static LRESULT CALLBACK mouseHookProc(
        int nCode,
        WPARAM wParam,
        LPARAM lParam);
    void handleMouseWheel(int delta);
    HHOOK mouseHook_ = nullptr;
    static ClatashaInputHudWindow *hookInstance_;
#endif

    QTimer pollTimer_;
    QElapsedTimer wheelClock_;
    QVector<KeyDef> keys_;
    QStringList selectedKeyIds_;
    quint32 mouseControls_ = MouseAll;
    int outputMode_ = 0;

    mutable QMutex frameMutex_;
    QImage renderedFrame_;

    bool enabled_ = false;
    int opacityPercent_ = 92;
    int scalePercent_ = 100;
    int wheelDirection_ = 0;
    qint64 wheelFlashStartMs_ = -1;

    int baseWidth_ = 620;
    int baseHeight_ = 296;
    qreal mouseBaseX_ = 430.0;
};
