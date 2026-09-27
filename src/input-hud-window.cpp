#include "input-hud-window.hpp"

#include <QCoreApplication>
#include <QFont>
#include <QGuiApplication>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QSettings>
#include <QDir>
#include <QFileInfo>

#include <obs-module.h>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

namespace {
constexpr int kScreenMargin = 22;
const QColor kIdleFill(12, 22, 20, 205);
const QColor kIdleStroke(225, 238, 233, 235);
const QColor kActiveFill(84, 170, 51, 245);
const QColor kActiveStroke(193, 255, 165, 255);
const QColor kPanelFill(5, 15, 13, 155);
const QColor kMouseGlow(80, 205, 72, 255);
const QColor kText(240, 247, 244, 255);
}

#ifdef Q_OS_WIN
ClatashaInputHudWindow *ClatashaInputHudWindow::hookInstance_ = nullptr;
#endif

ClatashaInputHudWindow::ClatashaInputHudWindow(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ClatashaInputHUD"));
    setWindowTitle(QStringLiteral("Clatasha Input HUD"));
    setWindowFlags(
        Qt::Tool |
        Qt::FramelessWindowHint |
        Qt::WindowStaysOnTopHint |
        Qt::WindowDoesNotAcceptFocus);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NativeWindow, true);
    setWindowFlag(Qt::WindowTransparentForInput, true);

    loadSettings();
    rebuildLayout();
    wheelClock_.start();

#ifdef Q_OS_WIN
    const HWND hwnd =
        reinterpret_cast<HWND>(winId());
    if (hwnd)
        SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

    hookInstance_ = this;

    HMODULE inputHudModule = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(
            &ClatashaInputHudWindow::mouseHookProc),
        &inputHudModule);

    mouseHook_ = SetWindowsHookExW(
        WH_MOUSE_LL,
        &ClatashaInputHudWindow::mouseHookProc,
        inputHudModule,
        0);
#endif

    pollTimer_.setInterval(16);
    QObject::connect(
        &pollTimer_,
        &QTimer::timeout,
        this,
        [this]() { pollInputs(); });
    pollTimer_.start();

    positionHud();
    if (enabled_)
        show();
    else
        hide();
}

ClatashaInputHudWindow::~ClatashaInputHudWindow()
{
#ifdef Q_OS_WIN
    if (mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    if (hookInstance_ == this)
        hookInstance_ = nullptr;
#endif
}

QString ClatashaInputHudWindow::settingsFilePath() const
{
    char *path =
        obs_module_config_path(
            "settings.ini");
    if (!path)
        return {};

    const QString result =
        QString::fromUtf8(path);
    bfree(path);
    return result;
}

void ClatashaInputHudWindow::loadSettings()
{
    const QString path =
        settingsFilePath();
    if (path.isEmpty())
        return;

    QDir().mkpath(
        QFileInfo(path).absolutePath());
    QSettings settings(
        path,
        QSettings::IniFormat);

    enabled_ =
        settings.value(
                    QStringLiteral("inputHud/enabled"),
                    false)
            .toBool();
    opacityPercent_ =
        std::clamp(
            settings.value(
                        QStringLiteral("inputHud/opacity"),
                        92)
                .toInt(),
            20,
            100);
    scalePercent_ =
        std::clamp(
            settings.value(
                        QStringLiteral("inputHud/scale"),
                        100)
                .toInt(),
            60,
            160);
}

void ClatashaInputHudWindow::saveSettings() const
{
    const QString path =
        settingsFilePath();
    if (path.isEmpty())
        return;

    QDir().mkpath(
        QFileInfo(path).absolutePath());
    QSettings settings(
        path,
        QSettings::IniFormat);
    settings.setValue(
        QStringLiteral("inputHud/enabled"),
        enabled_);
    settings.setValue(
        QStringLiteral("inputHud/opacity"),
        opacityPercent_);
    settings.setValue(
        QStringLiteral("inputHud/scale"),
        scalePercent_);
    settings.sync();
}

void ClatashaInputHudWindow::setEnabled(bool enabled)
{
    enabled_ = enabled;
    if (enabled_) {
        positionHud();
        show();
        raise();
    } else {
        hide();
    }
}

void ClatashaInputHudWindow::setOpacityPercent(int value)
{
    opacityPercent_ =
        std::clamp(value, 20, 100);
    update();
}

void ClatashaInputHudWindow::setScalePercent(int value)
{
    scalePercent_ =
        std::clamp(value, 60, 160);
    rebuildLayout();
    positionHud();
    update();
}

void ClatashaInputHudWindow::rebuildLayout()
{
    const qreal s =
        static_cast<qreal>(scalePercent_) /
        100.0;
    setFixedSize(
        qRound(baseWidth_ * s),
        qRound(baseHeight_ * s));

    keys_.clear();

    const qreal key = 38.0;
    const qreal gap = 6.0;
    const qreal x0 = 18.0;
    const qreal y0 = 18.0;

    auto add = [&](const QString &label,
                   int vk,
                   qreal x,
                   qreal y,
                   qreal w = 38.0,
                   qreal h = 38.0) {
        keys_.push_back(
            KeyDef{
                label,
                vk,
                QRectF(x, y, w, h)});
    };

    add(QStringLiteral("Esc"), VK_ESCAPE, x0, y0, 44);
    add(QStringLiteral("F1"), VK_F1, x0 + 92, y0, 44);

    const qreal row1 = y0 + key + gap;
    add(QStringLiteral("~"), VK_OEM_3, x0, row1);
    add(QStringLiteral("1"), '1', x0 + 44, row1);
    add(QStringLiteral("2"), '2', x0 + 88, row1);
    add(QStringLiteral("3"), '3', x0 + 132, row1);
    add(QStringLiteral("4"), '4', x0 + 176, row1);

    const qreal row2 = row1 + key + gap;
    add(QStringLiteral("Tab"), VK_TAB, x0, row2, 56);
    add(QStringLiteral("Q"), 'Q', x0 + 62, row2);
    add(QStringLiteral("W"), 'W', x0 + 106, row2);
    add(QStringLiteral("E"), 'E', x0 + 150, row2);
    add(QStringLiteral("R"), 'R', x0 + 194, row2);

    const qreal row3 = row2 + key + gap;
    add(QStringLiteral("Caps"), VK_CAPITAL, x0, row3, 62);
    add(QStringLiteral("A"), 'A', x0 + 68, row3);
    add(QStringLiteral("S"), 'S', x0 + 112, row3);
    add(QStringLiteral("D"), 'D', x0 + 156, row3);
    add(QStringLiteral("F"), 'F', x0 + 200, row3);
    add(QStringLiteral("G"), 'G', x0 + 244, row3);

    const qreal row4 = row3 + key + gap;
    add(QStringLiteral("Shift"), VK_LSHIFT, x0, row4, 82);
    add(QStringLiteral("Z"), 'Z', x0 + 88, row4);
    add(QStringLiteral("X"), 'X', x0 + 132, row4);
    add(QStringLiteral("C"), 'C', x0 + 176, row4);
    add(QStringLiteral("V"), 'V', x0 + 220, row4);

    const qreal row5 = row4 + key + gap;
    add(QStringLiteral("Ctrl"), VK_LCONTROL, x0, row5, 56);
    add(QStringLiteral("Alt"), VK_LMENU, x0 + 102, row5, 56);
    add(QStringLiteral("Space"), VK_SPACE, x0 + 164, row5, 220);
}

void ClatashaInputHudWindow::positionHud()
{
    QScreen *screen =
        QGuiApplication::primaryScreen();
    if (!screen)
        return;

    const QRect area =
        screen->availableGeometry();
    move(
        area.left() + kScreenMargin,
        area.bottom() -
            height() -
            kScreenMargin +
            1);
}

bool ClatashaInputHudWindow::keyDown(int vk) const
{
#ifdef Q_OS_WIN
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
    Q_UNUSED(vk);
    return false;
#endif
}

void ClatashaInputHudWindow::pollInputs()
{
    if (!enabled_)
        return;

    if (wheelFlashStartMs_ >= 0 &&
        wheelClock_.elapsed() -
                wheelFlashStartMs_ >
            160) {
        wheelDirection_ = 0;
        wheelFlashStartMs_ = -1;
    }

    update();
}

void ClatashaInputHudWindow::drawKey(
    QPainter &p,
    const KeyDef &key,
    bool active)
{
    const QColor fill =
        active ? kActiveFill : kIdleFill;
    const QColor stroke =
        active ? kActiveStroke : kIdleStroke;

    QPainterPath path;
    path.addRoundedRect(key.rect, 7.0, 7.0);

    p.fillPath(path, fill);
    p.setPen(QPen(stroke, active ? 2.0 : 1.5));
    p.drawPath(path);

    if (active) {
        p.save();
        p.setPen(
            QPen(
                QColor(98, 255, 101, 120),
                5.0));
        p.drawPath(path);
        p.restore();
    }

    QFont font(
        QStringLiteral("Segoe UI"),
        key.label.size() > 4 ? 9 : 10,
        QFont::DemiBold);
    p.setFont(font);
    p.setPen(kText);
    p.drawText(
        key.rect,
        Qt::AlignCenter,
        key.label);
}

void ClatashaInputHudWindow::drawMouse(QPainter &p)
{
    const QRectF area(430.0, 18.0, 165.0, 218.0);

    const bool left = keyDown(VK_LBUTTON);
    const bool right = keyDown(VK_RBUTTON);
    const bool middle = keyDown(VK_MBUTTON);
    const bool side1 = keyDown(VK_XBUTTON1);
    const bool side2 = keyDown(VK_XBUTTON2);

    QPainterPath body;
    body.moveTo(area.center().x(), area.top() + 5);
    body.cubicTo(
        area.left() + 16, area.top() + 8,
        area.left() + 9, area.top() + 70,
        area.left() + 12, area.top() + 125);
    body.cubicTo(
        area.left() + 15, area.bottom() - 25,
        area.center().x() - 28, area.bottom() - 4,
        area.center().x(), area.bottom());
    body.cubicTo(
        area.center().x() + 28, area.bottom() - 4,
        area.right() - 15, area.bottom() - 25,
        area.right() - 12, area.top() + 125);
    body.cubicTo(
        area.right() - 9, area.top() + 70,
        area.right() - 16, area.top() + 8,
        area.center().x(), area.top() + 5);

    p.setPen(QPen(kIdleStroke, 2.0));
    p.setBrush(QColor(8, 21, 18, 215));
    p.drawPath(body);

    const qreal cx = area.center().x();
    const qreal top = area.top() + 8;
    const qreal splitY = area.top() + 95;

    QPainterPath leftButton;
    leftButton.moveTo(cx - 2, top);
    leftButton.lineTo(cx - 2, splitY);
    leftButton.cubicTo(
        area.left() + 24, splitY - 12,
        area.left() + 15, area.top() + 55,
        area.left() + 18, area.top() + 24);
    leftButton.cubicTo(
        area.left() + 34, area.top() + 10,
        cx - 25, top + 1,
        cx - 2, top);

    QPainterPath rightButton;
    rightButton.moveTo(cx + 2, top);
    rightButton.lineTo(cx + 2, splitY);
    rightButton.cubicTo(
        area.right() - 24, splitY - 12,
        area.right() - 15, area.top() + 55,
        area.right() - 18, area.top() + 24);
    rightButton.cubicTo(
        area.right() - 34, area.top() + 10,
        cx + 25, top + 1,
        cx + 2, top);

    auto drawRegion =
        [&](const QPainterPath &path,
            bool active) {
            p.fillPath(
                path,
                active
                    ? QColor(61, 183, 61, 230)
                    : QColor(16, 31, 27, 220));
            p.setPen(
                QPen(
                    active
                        ? kActiveStroke
                        : kIdleStroke,
                    active ? 2.2 : 1.3));
            p.drawPath(path);

            if (active) {
                p.save();
                p.setPen(
                    QPen(
                        QColor(90, 255, 95, 100),
                        7.0));
                p.drawPath(path);
                p.restore();
            }
        };

    drawRegion(leftButton, left);
    drawRegion(rightButton, right);

    const QRectF wheel(
        cx - 10,
        area.top() + 25,
        20,
        51);
    p.setPen(
        QPen(
            middle
                ? kActiveStroke
                : kIdleStroke,
            middle ? 2.0 : 1.3));
    p.setBrush(
        middle
            ? kActiveFill
            : QColor(17, 35, 30, 235));
    p.drawRoundedRect(wheel, 8, 8);

    if (wheelDirection_ != 0) {
        const QPointF center =
            wheel.center();
        QPainterPath arrow;

        if (wheelDirection_ > 0) {
            arrow.moveTo(
                center.x(),
                center.y() - 10);
            arrow.lineTo(
                center.x() - 5,
                center.y() - 2);
            arrow.lineTo(
                center.x() + 5,
                center.y() - 2);
        } else {
            arrow.moveTo(
                center.x(),
                center.y() + 10);
            arrow.lineTo(
                center.x() - 5,
                center.y() + 2);
            arrow.lineTo(
                center.x() + 5,
                center.y() + 2);
        }

        arrow.closeSubpath();
        p.fillPath(arrow, kMouseGlow);
    }

    const QRectF sideOne(
        area.left() + 3,
        area.top() + 89,
        11,
        35);
    const QRectF sideTwo(
        area.left() + 3,
        area.top() + 130,
        11,
        29);

    p.setPen(QPen(kIdleStroke, 1.2));
    p.setBrush(
        side1
            ? kActiveFill
            : QColor(13, 29, 25, 230));
    p.drawRoundedRect(sideOne, 4, 4);
    p.setBrush(
        side2
            ? kActiveFill
            : QColor(13, 29, 25, 230));
    p.drawRoundedRect(sideTwo, 4, 4);

    const QRectF badge(
        cx - 13,
        area.top() + 84,
        26,
        26);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(55, 139, 45, 235));
    p.drawEllipse(badge);
    p.setPen(Qt::white);

    QFont badgeFont(
        QStringLiteral("Segoe UI"),
        9,
        QFont::Bold);
    p.setFont(badgeFont);
    p.drawText(
        badge,
        Qt::AlignCenter,
        QStringLiteral("C"));
}

void ClatashaInputHudWindow::paintEvent(QPaintEvent *)
{
    if (!enabled_)
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const qreal scale =
        static_cast<qreal>(width()) /
        static_cast<qreal>(baseWidth_);
    p.scale(scale, scale);
    p.setOpacity(
        static_cast<qreal>(
            opacityPercent_) /
        100.0);

    QPainterPath panel;
    panel.addRoundedRect(
        QRectF(
            0.5,
            0.5,
            baseWidth_ - 1.0,
            baseHeight_ - 1.0),
        14.0,
        14.0);
    p.fillPath(panel, kPanelFill);
    p.setPen(
        QPen(
            QColor(255, 255, 255, 34),
            1.0));
    p.drawPath(panel);

    for (const KeyDef &key : keys_)
        drawKey(
            p,
            key,
            keyDown(key.vk));

    drawMouse(p);
}

#ifdef Q_OS_WIN
LRESULT CALLBACK ClatashaInputHudWindow::mouseHookProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam)
{
    if (nCode >= 0 &&
        hookInstance_ &&
        wParam == WM_MOUSEWHEEL) {
        const auto *data =
            reinterpret_cast<MSLLHOOKSTRUCT *>(
                lParam);
        const int delta =
            GET_WHEEL_DELTA_WPARAM(
                data->mouseData);

        QMetaObject::invokeMethod(
            hookInstance_,
            [delta]() {
                if (hookInstance_)
                    hookInstance_->handleMouseWheel(
                        delta);
            },
            Qt::QueuedConnection);
    }

    return CallNextHookEx(
        nullptr,
        nCode,
        wParam,
        lParam);
}

void ClatashaInputHudWindow::handleMouseWheel(int delta)
{
    if (!enabled_)
        return;

    wheelDirection_ =
        delta > 0 ? 1 : -1;
    wheelFlashStartMs_ =
        wheelClock_.elapsed();
    update();
}
#endif
