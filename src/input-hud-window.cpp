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
const QColor kActiveFill(0, 164, 255, 245);
const QColor kActiveStroke(124, 226, 255, 255);
const QColor kPanelFill(5, 15, 20, 155);
const QColor kMouseGlow(0, 198, 255, 255);
const QColor kText(240, 247, 244, 255);
}

#ifdef Q_OS_WIN
ClatashaInputHudWindow *ClatashaInputHudWindow::hookInstance_ = nullptr;
#endif

QStringList ClatashaInputHudWindow::fpsDefaultKeyIds()
{
    return {
        QStringLiteral("esc"),
        QStringLiteral("f1"),
        QStringLiteral("tilde"),
        QStringLiteral("1"),
        QStringLiteral("2"),
        QStringLiteral("3"),
        QStringLiteral("4"),
        QStringLiteral("tab"),
        QStringLiteral("q"),
        QStringLiteral("w"),
        QStringLiteral("e"),
        QStringLiteral("r"),
        QStringLiteral("caps"),
        QStringLiteral("a"),
        QStringLiteral("s"),
        QStringLiteral("d"),
        QStringLiteral("f"),
        QStringLiteral("g"),
        QStringLiteral("shift"),
        QStringLiteral("z"),
        QStringLiteral("x"),
        QStringLiteral("c"),
        QStringLiteral("v"),
        QStringLiteral("ctrl"),
        QStringLiteral("alt"),
        QStringLiteral("space"),
    };
}

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

    const QString storedKeys =
        settings.value(
                    QStringLiteral("inputHud/selectedKeys"),
                    fpsDefaultKeyIds().join(
                        QLatin1Char(',')))
            .toString();

    selectedKeyIds_ =
        storedKeys.split(
            QLatin1Char(','),
            Qt::SkipEmptyParts);

    if (selectedKeyIds_.isEmpty())
        selectedKeyIds_ = fpsDefaultKeyIds();

    mouseControls_ =
        settings.value(
                    QStringLiteral("inputHud/mouseControls"),
                    static_cast<quint32>(MouseAll))
            .toUInt();
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
    settings.setValue(
        QStringLiteral("inputHud/selectedKeys"),
        selectedKeyIds_.join(
            QLatin1Char(',')));
    settings.setValue(
        QStringLiteral("inputHud/mouseControls"),
        mouseControls_);
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

void ClatashaInputHudWindow::setSelectedKeyIds(
    const QStringList &ids)
{
    selectedKeyIds_ = ids;
    if (selectedKeyIds_.isEmpty())
        selectedKeyIds_ = fpsDefaultKeyIds();

    rebuildLayout();
    update();
}

void ClatashaInputHudWindow::setMouseControls(
    quint32 controls)
{
    mouseControls_ =
        controls &
        static_cast<quint32>(MouseAll);
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

    auto add = [&](const QString &id,
                   const QString &label,
                   int vk,
                   qreal x,
                   qreal y,
                   qreal w = 38.0,
                   qreal h = 38.0) {
        if (!selectedKeyIds_.contains(
                id,
                Qt::CaseInsensitive)) {
            return;
        }

        keys_.push_back(
            KeyDef{
                id,
                label,
                vk,
                QRectF(x, y, w, h)});
    };

    add(QStringLiteral("esc"), QStringLiteral("Esc"), VK_ESCAPE, x0, y0, 44);
    add(QStringLiteral("f1"), QStringLiteral("F1"), VK_F1, x0 + 92, y0, 44);

    const qreal row1 = y0 + key + gap;
    add(QStringLiteral("tilde"), QStringLiteral("~"), VK_OEM_3, x0, row1);
    add(QStringLiteral("1"), QStringLiteral("1"), '1', x0 + 44, row1);
    add(QStringLiteral("2"), QStringLiteral("2"), '2', x0 + 88, row1);
    add(QStringLiteral("3"), QStringLiteral("3"), '3', x0 + 132, row1);
    add(QStringLiteral("4"), QStringLiteral("4"), '4', x0 + 176, row1);

    const qreal row2 = row1 + key + gap;
    add(QStringLiteral("tab"), QStringLiteral("Tab"), VK_TAB, x0, row2, 56);
    add(QStringLiteral("q"), QStringLiteral("Q"), 'Q', x0 + 62, row2);
    add(QStringLiteral("w"), QStringLiteral("W"), 'W', x0 + 106, row2);
    add(QStringLiteral("e"), QStringLiteral("E"), 'E', x0 + 150, row2);
    add(QStringLiteral("r"), QStringLiteral("R"), 'R', x0 + 194, row2);

    const qreal row3 = row2 + key + gap;
    add(QStringLiteral("caps"), QStringLiteral("Caps"), VK_CAPITAL, x0, row3, 62);
    add(QStringLiteral("a"), QStringLiteral("A"), 'A', x0 + 68, row3);
    add(QStringLiteral("s"), QStringLiteral("S"), 'S', x0 + 112, row3);
    add(QStringLiteral("d"), QStringLiteral("D"), 'D', x0 + 156, row3);
    add(QStringLiteral("f"), QStringLiteral("F"), 'F', x0 + 200, row3);
    add(QStringLiteral("g"), QStringLiteral("G"), 'G', x0 + 244, row3);

    const qreal row4 = row3 + key + gap;
    add(QStringLiteral("shift"), QStringLiteral("Shift"), VK_LSHIFT, x0, row4, 82);
    add(QStringLiteral("z"), QStringLiteral("Z"), 'Z', x0 + 88, row4);
    add(QStringLiteral("x"), QStringLiteral("X"), 'X', x0 + 132, row4);
    add(QStringLiteral("c"), QStringLiteral("C"), 'C', x0 + 176, row4);
    add(QStringLiteral("v"), QStringLiteral("V"), 'V', x0 + 220, row4);

    const qreal row5 = row4 + key + gap;
    add(QStringLiteral("ctrl"), QStringLiteral("Ctrl"), VK_LCONTROL, x0, row5, 56);
    add(QStringLiteral("alt"), QStringLiteral("Alt"), VK_LMENU, x0 + 102, row5, 56);
    add(QStringLiteral("space"), QStringLiteral("Space"), VK_SPACE, x0 + 164, row5, 220);
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
                QColor(0, 190, 255, 145),
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
    const QRectF area(
        430.0,
        18.0,
        165.0,
        242.0);

    const bool left =
        (mouseControls_ & MouseLeft) != 0 &&
        keyDown(VK_LBUTTON);
    const bool right =
        (mouseControls_ & MouseRight) != 0 &&
        keyDown(VK_RBUTTON);
    const bool middle =
        (mouseControls_ & MouseMiddle) != 0 &&
        keyDown(VK_MBUTTON);

    // Windows' XBUTTON numbering is opposite to the front/back physical
    // placement used by the Clatasha mouse drawing on the test mouse.
    const bool sideFront =
        (mouseControls_ & MouseFront) != 0 &&
        keyDown(VK_XBUTTON2);
    const bool sideBack =
        (mouseControls_ & MouseBack) != 0 &&
        keyDown(VK_XBUTTON1);

    const qreal cx =
        area.center().x();
    const qreal top =
        area.top() + 4.0;
    const qreal buttonBottom =
        area.top() + 96.0;

    QPainterPath body;
    body.moveTo(
        cx,
        top);
    body.cubicTo(
        area.left() + 26,
        top + 2,
        area.left() + 12,
        area.top() + 58,
        area.left() + 12,
        area.top() + 126);
    body.cubicTo(
        area.left() + 12,
        area.bottom() - 34,
        cx - 34,
        area.bottom() - 3,
        cx,
        area.bottom());
    body.cubicTo(
        cx + 34,
        area.bottom() - 3,
        area.right() - 12,
        area.bottom() - 34,
        area.right() - 12,
        area.top() + 126);
    body.cubicTo(
        area.right() - 12,
        area.top() + 58,
        area.right() - 26,
        top + 2,
        cx,
        top);
    body.closeSubpath();

    p.setPen(
        QPen(
            kIdleStroke,
            2.0));
    p.setBrush(
        QColor(
            8,
            21,
            27,
            220));
    p.drawPath(body);

    // Clean, non-overlapping top click zones. Each half is clipped to the
    // mouse shell, so the center divider remains crisp.
    QPainterPath leftClip;
    leftClip.addRect(
        QRectF(
            area.left(),
            top,
            cx - area.left() - 2.0,
            buttonBottom - top));

    QPainterPath rightClip;
    rightClip.addRect(
        QRectF(
            cx + 2.0,
            top,
            area.right() - cx - 2.0,
            buttonBottom - top));

    const QPainterPath leftButton =
        body.intersected(leftClip);
    const QPainterPath rightButton =
        body.intersected(rightClip);

    auto drawButtonRegion =
        [&](const QPainterPath &path,
            bool active) {
            p.save();
            p.setPen(Qt::NoPen);
            p.fillPath(
                path,
                active
                    ? kActiveFill
                    : QColor(
                          15,
                          29,
                          36,
                          225));

            if (active) {
                p.setPen(
                    QPen(
                        QColor(
                            0,
                            195,
                            255,
                            120),
                        6.0));
                p.drawPath(path);
            }
            p.restore();
        };

    drawButtonRegion(
        leftButton,
        left);
    drawButtonRegion(
        rightButton,
        right);

    // Redraw shell and separators over the fills to keep the geometry clean.
    p.setPen(
        QPen(
            kIdleStroke,
            2.0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(body);

    p.setPen(
        QPen(
            kIdleStroke,
            1.6));
    p.drawLine(
        QPointF(
            cx,
            top + 1),
        QPointF(
            cx,
            buttonBottom - 5));

    QPainterPath topDivider;
    topDivider.moveTo(
        area.left() + 15,
        buttonBottom);
    topDivider.cubicTo(
        area.left() + 46,
        buttonBottom + 20,
        cx - 22,
        buttonBottom + 20,
        cx,
        buttonBottom + 6);
    topDivider.cubicTo(
        cx + 22,
        buttonBottom + 20,
        area.right() - 46,
        buttonBottom + 20,
        area.right() - 15,
        buttonBottom);
    p.drawPath(topDivider);

    const QRectF wheel(
        cx - 10,
        area.top() + 26,
        20,
        52);

    p.setPen(
        QPen(
            middle
                ? kActiveStroke
                : kIdleStroke,
            middle
                ? 2.0
                : 1.3));
    p.setBrush(
        middle
            ? kActiveFill
            : QColor(
                  17,
                  35,
                  41,
                  235));
    p.drawRoundedRect(
        wheel,
        8,
        8);

    if (middle) {
        p.save();
        p.setPen(
            QPen(
                QColor(
                    0,
                    195,
                    255,
                    120),
                5.0));
        p.drawRoundedRect(
            wheel,
            8,
            8);
        p.restore();
    }

    if ((mouseControls_ & MouseWheel) != 0 &&
        wheelDirection_ != 0) {
        const QPointF center =
            wheel.center();

        QPainterPath arrow;
        if (wheelDirection_ > 0) {
            arrow.moveTo(
                center.x(),
                center.y() - 11);
            arrow.lineTo(
                center.x() - 5,
                center.y() - 2);
            arrow.lineTo(
                center.x() + 5,
                center.y() - 2);
        } else {
            arrow.moveTo(
                center.x(),
                center.y() + 11);
            arrow.lineTo(
                center.x() - 5,
                center.y() + 2);
            arrow.lineTo(
                center.x() + 5,
                center.y() + 2);
        }
        arrow.closeSubpath();
        p.fillPath(
            arrow,
            kMouseGlow);
    }

    // Front button is physically above the rear button in this side view.
    const QRectF sideFrontRect(
        area.left() + 2,
        area.top() + 95,
        12,
        37);
    const QRectF sideBackRect(
        area.left() + 2,
        area.top() + 139,
        12,
        31);

    auto drawSide =
        [&](const QRectF &rect,
            bool active) {
            p.setPen(
                QPen(
                    active
                        ? kActiveStroke
                        : kIdleStroke,
                    active
                        ? 1.8
                        : 1.2));
            p.setBrush(
                active
                    ? kActiveFill
                    : QColor(
                          13,
                          29,
                          35,
                          235));
            p.drawRoundedRect(
                rect,
                4,
                4);
        };

    drawSide(
        sideFrontRect,
        sideFront);
    drawSide(
        sideBackRect,
        sideBack);

    const QRectF badge(
        cx - 13,
        area.top() + 102,
        26,
        26);

    p.setPen(Qt::NoPen);
    p.setBrush(
        QColor(
            0,
            145,
            225,
            245));
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
    if (!enabled_ ||
        (mouseControls_ & MouseWheel) == 0) {
        return;
    }

    wheelDirection_ =
        delta > 0 ? 1 : -1;
    wheelFlashStartMs_ =
        wheelClock_.elapsed();
    update();
}
#endif
