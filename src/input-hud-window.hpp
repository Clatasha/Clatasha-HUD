#pragma once

#include <QElapsedTimer>
#include <QRectF>
#include <QString>
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
    explicit ClatashaInputHudWindow(QWidget *parent = nullptr);
    ~ClatashaInputHudWindow() override;

    bool enabled() const { return enabled_; }
    int opacityPercent() const { return opacityPercent_; }
    int scalePercent() const { return scalePercent_; }

    void setEnabled(bool enabled);
    void setOpacityPercent(int value);
    void setScalePercent(int value);
    void positionHud();
    void saveSettings() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    struct KeyDef {
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

    bool enabled_ = false;
    int opacityPercent_ = 92;
    int scalePercent_ = 100;
    int wheelDirection_ = 0;
    qint64 wheelFlashStartMs_ = -1;

    int baseWidth_ = 620;
    int baseHeight_ = 296;
};
