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

    if (storedKeys ==
        QStringLiteral("__none__")) {
        selectedKeyIds_.clear();
    } else {
        selectedKeyIds_ =
            storedKeys.split(
                QLatin1Char(','),
                Qt::SkipEmptyParts);
    }

    mouseControls_ =
        settings.value(
                    QStringLiteral("inputHud/mouseControls"),
                    static_cast<quint32>(MouseAll))
            .toUInt();

    outputMode_ =
        std::clamp(
            settings.value(
                        QStringLiteral("inputHud/outputMode"),
                        0)
                .toInt(),
            0,
            2);
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
        selectedKeyIds_.isEmpty()
            ? QStringLiteral("__none__")
            : selectedKeyIds_.join(
                  QLatin1Char(',')));
    settings.setValue(
        QStringLiteral("inputHud/mouseControls"),
        mouseControls_);
    settings.setValue(
        QStringLiteral("inputHud/outputMode"),
        outputMode_);
    settings.sync();
}

void ClatashaInputHudWindow::setEnabled(bool enabled)
{
    enabled_ = enabled;

    if (enabled_ && outputMode_ != 1) {
        positionHud();
        show();
        raise();
    } else {
        hide();
    }

    refreshRenderedFrame();
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
    rebuildLayout();
    update();
}

void ClatashaInputHudWindow::setMouseControls(
    quint32 controls)
{
    mouseControls_ =
        controls &
        static_cast<quint32>(MouseAll);
    rebuildLayout();
    refreshRenderedFrame();
    update();
}

void ClatashaInputHudWindow::setOutputMode(int mode)
{
    outputMode_ =
        std::clamp(mode, 0, 2);

    if (enabled_ && outputMode_ != 1) {
        positionHud();
        show();
        raise();
    } else {
        hide();
    }

    refreshRenderedFrame();
}

void ClatashaInputHudWindow::rebuildLayout()
{
    keys_.clear();

    const auto has =
        [&](const char *id) {
            return selectedKeyIds_.contains(
                QString::fromLatin1(id),
                Qt::CaseInsensitive);
        };

    const auto any =
        [&](std::initializer_list<const char *> ids) {
            for (const char *id : ids) {
                if (has(id))
                    return true;
            }
            return false;
        };

    const bool mainActive =
        any({
            "tilde","1","2","3","4","5","6","7","8","9","0","minus","equals","backspace",
            "tab","q","w","e","r","t","y","u","i","o","p","lbracket","rbracket","backslash",
            "caps","a","s","d","f","g","h","j","k","l","semicolon","quote","enter",
            "shift","z","x","c","v","b","n","m","comma","period","slash","rshift",
            "ctrl","lwin","alt","space","ralt","rwin","menu","rctrl"
        });

    const bool functionActive =
        any({
            "esc","f1","f2","f3","f4","f5","f6","f7","f8","f9","f10","f11","f12"
        });

    const bool navigationActive =
        any({
            "insert","home","pgup","delete","end","pgdn"
        });

    const bool arrowsActive =
        any({
            "up","left","down","right"
        });

    const bool numpadActive =
        any({
            "numlock","numdivide","nummultiply","numminus",
            "num7","num8","num9","numplus",
            "num4","num5","num6",
            "num1","num2","num3","numenter",
            "num0","numdecimal"
        });

    const bool mouseActive =
        mouseControls_ != 0;

    constexpr qreal pad = 18.0;
    constexpr qreal key = 38.0;
    constexpr qreal gap = 6.0;

    // Match the compact feel of the original FPS HUD. Sections keep their
    // real keyboard coordinates internally, but unused trailing columns no
    // longer reserve space before the next section.
    constexpr qreal sectionGap = 28.0;
    constexpr qreal mainHeight = 214.0;
    constexpr qreal functionHeight = 38.0;
    constexpr qreal navHeight = 82.0;
    constexpr qreal arrowHeight = 82.0;
    constexpr qreal numpadHeight = 214.0;
    constexpr qreal mouseWidth = 165.0;
    constexpr qreal mouseHeight = 242.0;
    constexpr qreal stackGap = 12.0;

    auto includeRight =
        [&](qreal &right,
            const char *id,
            qreal edge) {
            if (has(id))
                right = qMax(right, edge);
        };

    qreal mainRight = 0.0;
    includeRight(mainRight, "tilde", 38);
    includeRight(mainRight, "1", 82);
    includeRight(mainRight, "2", 126);
    includeRight(mainRight, "3", 170);
    includeRight(mainRight, "4", 214);
    includeRight(mainRight, "5", 258);
    includeRight(mainRight, "6", 302);
    includeRight(mainRight, "7", 346);
    includeRight(mainRight, "8", 390);
    includeRight(mainRight, "9", 434);
    includeRight(mainRight, "0", 478);
    includeRight(mainRight, "minus", 522);
    includeRight(mainRight, "equals", 566);
    includeRight(mainRight, "backspace", 654);

    includeRight(mainRight, "tab", 56);
    includeRight(mainRight, "q", 100);
    includeRight(mainRight, "w", 144);
    includeRight(mainRight, "e", 188);
    includeRight(mainRight, "r", 232);
    includeRight(mainRight, "t", 276);
    includeRight(mainRight, "y", 320);
    includeRight(mainRight, "u", 364);
    includeRight(mainRight, "i", 408);
    includeRight(mainRight, "o", 452);
    includeRight(mainRight, "p", 496);
    includeRight(mainRight, "lbracket", 540);
    includeRight(mainRight, "rbracket", 584);
    includeRight(mainRight, "backslash", 654);

    includeRight(mainRight, "caps", 68);
    includeRight(mainRight, "a", 112);
    includeRight(mainRight, "s", 156);
    includeRight(mainRight, "d", 200);
    includeRight(mainRight, "f", 244);
    includeRight(mainRight, "g", 288);
    includeRight(mainRight, "h", 332);
    includeRight(mainRight, "j", 376);
    includeRight(mainRight, "k", 420);
    includeRight(mainRight, "l", 464);
    includeRight(mainRight, "semicolon", 508);
    includeRight(mainRight, "quote", 552);
    includeRight(mainRight, "enter", 654);

    includeRight(mainRight, "shift", 90);
    includeRight(mainRight, "z", 134);
    includeRight(mainRight, "x", 178);
    includeRight(mainRight, "c", 222);
    includeRight(mainRight, "v", 266);
    includeRight(mainRight, "b", 310);
    includeRight(mainRight, "n", 354);
    includeRight(mainRight, "m", 398);
    includeRight(mainRight, "comma", 442);
    includeRight(mainRight, "period", 486);
    includeRight(mainRight, "slash", 530);
    includeRight(mainRight, "rshift", 654);

    includeRight(mainRight, "ctrl", 58);
    includeRight(mainRight, "lwin", 116);
    includeRight(mainRight, "alt", 174);
    includeRight(mainRight, "space", 404);
    includeRight(mainRight, "ralt", 462);
    includeRight(mainRight, "rwin", 520);
    includeRight(mainRight, "menu", 588);
    includeRight(mainRight, "rctrl", 654);

    qreal functionRight = 0.0;
    includeRight(functionRight, "esc", 44);
    includeRight(functionRight, "f1", 106);
    includeRight(functionRight, "f2", 150);
    includeRight(functionRight, "f3", 194);
    includeRight(functionRight, "f4", 238);
    includeRight(functionRight, "f5", 304);
    includeRight(functionRight, "f6", 348);
    includeRight(functionRight, "f7", 392);
    includeRight(functionRight, "f8", 436);
    includeRight(functionRight, "f9", 502);
    includeRight(functionRight, "f10", 546);
    includeRight(functionRight, "f11", 590);
    includeRight(functionRight, "f12", 634);

    qreal navRight = 0.0;
    includeRight(navRight, "insert", 38);
    includeRight(navRight, "home", 82);
    includeRight(navRight, "pgup", 126);
    includeRight(navRight, "delete", 38);
    includeRight(navRight, "end", 82);
    includeRight(navRight, "pgdn", 126);

    qreal arrowRight = 0.0;
    includeRight(arrowRight, "up", 82);
    includeRight(arrowRight, "left", 38);
    includeRight(arrowRight, "down", 82);
    includeRight(arrowRight, "right", 126);

    qreal numpadRight = 0.0;
    includeRight(numpadRight, "numlock", 38);
    includeRight(numpadRight, "numdivide", 82);
    includeRight(numpadRight, "nummultiply", 126);
    includeRight(numpadRight, "numminus", 170);
    includeRight(numpadRight, "num7", 38);
    includeRight(numpadRight, "num8", 82);
    includeRight(numpadRight, "num9", 126);
    includeRight(numpadRight, "numplus", 170);
    includeRight(numpadRight, "num4", 38);
    includeRight(numpadRight, "num5", 82);
    includeRight(numpadRight, "num6", 126);
    includeRight(numpadRight, "num1", 38);
    includeRight(numpadRight, "num2", 82);
    includeRight(numpadRight, "num3", 126);
    includeRight(numpadRight, "numenter", 170);
    includeRight(numpadRight, "num0", 82);
    includeRight(numpadRight, "numdecimal", 126);

    const qreal mainColumnWidth =
        qMax(mainRight, functionRight);
    const qreal mainColumnHeight =
        (mainActive ? mainHeight : 0.0) +
        (functionActive ? functionHeight : 0.0) +
        (mainActive && functionActive ? stackGap : 0.0);

    const qreal navColumnWidth =
        qMax(navRight, arrowRight);
    const qreal navColumnHeight =
        (navigationActive ? navHeight : 0.0) +
        (arrowsActive ? arrowHeight : 0.0) +
        (navigationActive && arrowsActive ? stackGap : 0.0);

    const qreal numpadColumnWidth =
        numpadRight;
    const qreal numpadColumnHeight =
        numpadActive ? numpadHeight : 0.0;

    const qreal mouseColumnWidth =
        mouseActive ? mouseWidth : 0.0;
    const qreal mouseColumnHeight =
        mouseActive ? mouseHeight : 0.0;

    QVector<qreal> activeWidths;
    if (mainColumnWidth > 0.0)
        activeWidths.push_back(mainColumnWidth);
    if (navColumnWidth > 0.0)
        activeWidths.push_back(navColumnWidth);
    if (numpadColumnWidth > 0.0)
        activeWidths.push_back(numpadColumnWidth);
    if (mouseColumnWidth > 0.0)
        activeWidths.push_back(mouseColumnWidth);

    qreal contentWidth = 0.0;
    for (qreal width : activeWidths)
        contentWidth += width;
    if (activeWidths.size() > 1)
        contentWidth +=
            sectionGap *
            (activeWidths.size() - 1);

    const qreal contentHeight =
        qMax(
            qMax(mainColumnHeight, navColumnHeight),
            qMax(numpadColumnHeight, mouseColumnHeight));

    baseWidth_ =
        qMax(
            140,
            qRound(contentWidth + pad * 2.0));
    baseHeight_ =
        qMax(
            120,
            qRound(contentHeight + pad * 2.0));

    const qreal scale =
        static_cast<qreal>(scalePercent_) /
        100.0;
    setFixedSize(
        qRound(baseWidth_ * scale),
        qRound(baseHeight_ * scale));

    auto add =
        [&](const char *id,
            const QString &label,
            int vk,
            qreal x,
            qreal y,
            qreal w = 38.0,
            qreal h = 38.0) {
            const QString keyId =
                QString::fromLatin1(id);

            if (!selectedKeyIds_.contains(
                    keyId,
                    Qt::CaseInsensitive)) {
                return;
            }

            keys_.push_back(
                KeyDef{
                    keyId,
                    label,
                    vk,
                    QRectF(x, y, w, h)});
        };

    qreal cursorX = pad;

    if (mainColumnWidth > 0.0) {
        const qreal columnY =
            baseHeight_ - pad - mainColumnHeight;

        qreal mainY = columnY;
        if (functionActive) {
            const qreal fy = columnY;

            add("esc", QStringLiteral("Esc"), VK_ESCAPE, cursorX, fy, 44);
            add("f1", QStringLiteral("F1"), VK_F1, cursorX + 68, fy);
            add("f2", QStringLiteral("F2"), VK_F2, cursorX + 112, fy);
            add("f3", QStringLiteral("F3"), VK_F3, cursorX + 156, fy);
            add("f4", QStringLiteral("F4"), VK_F4, cursorX + 200, fy);
            add("f5", QStringLiteral("F5"), VK_F5, cursorX + 266, fy);
            add("f6", QStringLiteral("F6"), VK_F6, cursorX + 310, fy);
            add("f7", QStringLiteral("F7"), VK_F7, cursorX + 354, fy);
            add("f8", QStringLiteral("F8"), VK_F8, cursorX + 398, fy);
            add("f9", QStringLiteral("F9"), VK_F9, cursorX + 464, fy);
            add("f10", QStringLiteral("F10"), VK_F10, cursorX + 508, fy);
            add("f11", QStringLiteral("F11"), VK_F11, cursorX + 552, fy);
            add("f12", QStringLiteral("F12"), VK_F12, cursorX + 596, fy);

            mainY +=
                functionHeight +
                (mainActive ? stackGap : 0.0);
        }

        if (mainActive) {
            const qreal r0 = mainY;
            const qreal r1 = r0 + key + gap;
            const qreal r2 = r1 + key + gap;
            const qreal r3 = r2 + key + gap;
            const qreal r4 = r3 + key + gap;

            add("tilde", QStringLiteral("~"), VK_OEM_3, cursorX, r0);
            add("1", QStringLiteral("1"), '1', cursorX + 44, r0);
            add("2", QStringLiteral("2"), '2', cursorX + 88, r0);
            add("3", QStringLiteral("3"), '3', cursorX + 132, r0);
            add("4", QStringLiteral("4"), '4', cursorX + 176, r0);
            add("5", QStringLiteral("5"), '5', cursorX + 220, r0);
            add("6", QStringLiteral("6"), '6', cursorX + 264, r0);
            add("7", QStringLiteral("7"), '7', cursorX + 308, r0);
            add("8", QStringLiteral("8"), '8', cursorX + 352, r0);
            add("9", QStringLiteral("9"), '9', cursorX + 396, r0);
            add("0", QStringLiteral("0"), '0', cursorX + 440, r0);
            add("minus", QStringLiteral("-"), VK_OEM_MINUS, cursorX + 484, r0);
            add("equals", QStringLiteral("="), VK_OEM_PLUS, cursorX + 528, r0);
            add("backspace", QStringLiteral("Back"), VK_BACK, cursorX + 572, r0, 82);

            add("tab", QStringLiteral("Tab"), VK_TAB, cursorX, r1, 56);
            add("q", QStringLiteral("Q"), 'Q', cursorX + 62, r1);
            add("w", QStringLiteral("W"), 'W', cursorX + 106, r1);
            add("e", QStringLiteral("E"), 'E', cursorX + 150, r1);
            add("r", QStringLiteral("R"), 'R', cursorX + 194, r1);
            add("t", QStringLiteral("T"), 'T', cursorX + 238, r1);
            add("y", QStringLiteral("Y"), 'Y', cursorX + 282, r1);
            add("u", QStringLiteral("U"), 'U', cursorX + 326, r1);
            add("i", QStringLiteral("I"), 'I', cursorX + 370, r1);
            add("o", QStringLiteral("O"), 'O', cursorX + 414, r1);
            add("p", QStringLiteral("P"), 'P', cursorX + 458, r1);
            add("lbracket", QStringLiteral("["), VK_OEM_4, cursorX + 502, r1);
            add("rbracket", QStringLiteral("]"), VK_OEM_6, cursorX + 546, r1);
            add("backslash", QStringLiteral("\\"), VK_OEM_5, cursorX + 590, r1, 64);

            add("caps", QStringLiteral("Caps"), VK_CAPITAL, cursorX, r2, 68);
            add("a", QStringLiteral("A"), 'A', cursorX + 74, r2);
            add("s", QStringLiteral("S"), 'S', cursorX + 118, r2);
            add("d", QStringLiteral("D"), 'D', cursorX + 162, r2);
            add("f", QStringLiteral("F"), 'F', cursorX + 206, r2);
            add("g", QStringLiteral("G"), 'G', cursorX + 250, r2);
            add("h", QStringLiteral("H"), 'H', cursorX + 294, r2);
            add("j", QStringLiteral("J"), 'J', cursorX + 338, r2);
            add("k", QStringLiteral("K"), 'K', cursorX + 382, r2);
            add("l", QStringLiteral("L"), 'L', cursorX + 426, r2);
            add("semicolon", QStringLiteral(";"), VK_OEM_1, cursorX + 470, r2);
            add("quote", QStringLiteral("'"), VK_OEM_7, cursorX + 514, r2);
            add("enter", QStringLiteral("Enter"), VK_RETURN, cursorX + 558, r2, 96);

            add("shift", QStringLiteral("Shift"), VK_LSHIFT, cursorX, r3, 90);
            add("z", QStringLiteral("Z"), 'Z', cursorX + 96, r3);
            add("x", QStringLiteral("X"), 'X', cursorX + 140, r3);
            add("c", QStringLiteral("C"), 'C', cursorX + 184, r3);
            add("v", QStringLiteral("V"), 'V', cursorX + 228, r3);
            add("b", QStringLiteral("B"), 'B', cursorX + 272, r3);
            add("n", QStringLiteral("N"), 'N', cursorX + 316, r3);
            add("m", QStringLiteral("M"), 'M', cursorX + 360, r3);
            add("comma", QStringLiteral(","), VK_OEM_COMMA, cursorX + 404, r3);
            add("period", QStringLiteral("."), VK_OEM_PERIOD, cursorX + 448, r3);
            add("slash", QStringLiteral("/"), VK_OEM_2, cursorX + 492, r3);
            add("rshift", QStringLiteral("Shift"), VK_RSHIFT, cursorX + 536, r3, 118);

            add("ctrl", QStringLiteral("Ctrl"), VK_LCONTROL, cursorX, r4, 58);
            add("lwin", QStringLiteral("Win"), VK_LWIN, cursorX + 64, r4, 52);
            add("alt", QStringLiteral("Alt"), VK_LMENU, cursorX + 122, r4, 52);
            add("space", QStringLiteral("Space"), VK_SPACE, cursorX + 180, r4, 224);
            add("ralt", QStringLiteral("Alt"), VK_RMENU, cursorX + 410, r4, 52);
            add("rwin", QStringLiteral("Win"), VK_RWIN, cursorX + 468, r4, 52);
            add("menu", QStringLiteral("Menu"), VK_APPS, cursorX + 526, r4, 62);
            add("rctrl", QStringLiteral("Ctrl"), VK_RCONTROL, cursorX + 594, r4, 60);
        }

        cursorX +=
            mainColumnWidth +
            sectionGap;
    }

    if (navColumnWidth > 0.0) {
        const qreal columnY =
            baseHeight_ - pad - navColumnHeight;

        qreal navY = columnY;
        if (navigationActive) {
            add("insert", QStringLiteral("Ins"), VK_INSERT, cursorX, navY);
            add("home", QStringLiteral("Home"), VK_HOME, cursorX + 44, navY);
            add("pgup", QStringLiteral("PgUp"), VK_PRIOR, cursorX + 88, navY);

            add("delete", QStringLiteral("Del"), VK_DELETE, cursorX, navY + 44);
            add("end", QStringLiteral("End"), VK_END, cursorX + 44, navY + 44);
            add("pgdn", QStringLiteral("PgDn"), VK_NEXT, cursorX + 88, navY + 44);

            navY +=
                navHeight +
                (arrowsActive ? stackGap : 0.0);
        }

        if (arrowsActive) {
            add("up", QStringLiteral("↑"), VK_UP, cursorX + 44, navY);
            add("left", QStringLiteral("←"), VK_LEFT, cursorX, navY + 44);
            add("down", QStringLiteral("↓"), VK_DOWN, cursorX + 44, navY + 44);
            add("right", QStringLiteral("→"), VK_RIGHT, cursorX + 88, navY + 44);
        }

        cursorX +=
            navColumnWidth +
            sectionGap;
    }

    if (numpadColumnWidth > 0.0) {
        const qreal y =
            baseHeight_ - pad - numpadHeight;

        add("numlock", QStringLiteral("Num"), VK_NUMLOCK, cursorX, y);
        add("numdivide", QStringLiteral("/"), VK_DIVIDE, cursorX + 44, y);
        add("nummultiply", QStringLiteral("*"), VK_MULTIPLY, cursorX + 88, y);
        add("numminus", QStringLiteral("-"), VK_SUBTRACT, cursorX + 132, y);

        add("num7", QStringLiteral("7"), VK_NUMPAD7, cursorX, y + 44);
        add("num8", QStringLiteral("8"), VK_NUMPAD8, cursorX + 44, y + 44);
        add("num9", QStringLiteral("9"), VK_NUMPAD9, cursorX + 88, y + 44);
        add("numplus", QStringLiteral("+"), VK_ADD, cursorX + 132, y + 44, key, 82);

        add("num4", QStringLiteral("4"), VK_NUMPAD4, cursorX, y + 88);
        add("num5", QStringLiteral("5"), VK_NUMPAD5, cursorX + 44, y + 88);
        add("num6", QStringLiteral("6"), VK_NUMPAD6, cursorX + 88, y + 88);

        add("num1", QStringLiteral("1"), VK_NUMPAD1, cursorX, y + 132);
        add("num2", QStringLiteral("2"), VK_NUMPAD2, cursorX + 44, y + 132);
        add("num3", QStringLiteral("3"), VK_NUMPAD3, cursorX + 88, y + 132);
        add("numenter", QStringLiteral("Ent"), VK_RETURN, cursorX + 132, y + 132, key, 82);

        add("num0", QStringLiteral("0"), VK_NUMPAD0, cursorX, y + 176, 82);
        add("numdecimal", QStringLiteral("."), VK_DECIMAL, cursorX + 88, y + 176);

        cursorX +=
            numpadColumnWidth +
            sectionGap;
    }

    if (mouseActive) {
        mouseBaseX_ = cursorX;
    } else {
        mouseBaseX_ = -1000.0;
    }
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

    refreshRenderedFrame();
    update();
}

QImage ClatashaInputHudWindow::latestFrame() const
{
    QMutexLocker locker(&frameMutex_);
    return renderedFrame_;
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
    if (mouseControls_ == 0)
        return;

    const QRectF area(
        mouseBaseX_,
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

void ClatashaInputHudWindow::drawContent(QPainter &p)
{
    p.setRenderHint(
        QPainter::Antialiasing,
        true);
    p.setRenderHint(
        QPainter::TextAntialiasing,
        true);

    const qreal scale =
        static_cast<qreal>(width()) /
        static_cast<qreal>(
            qMax(1, baseWidth_));
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
    p.fillPath(
        panel,
        kPanelFill);
    p.setPen(
        QPen(
            QColor(
                255,
                255,
                255,
                34),
            1.0));
    p.drawPath(panel);

    for (const KeyDef &key : keys_) {
        drawKey(
            p,
            key,
            keyDown(key.vk));
    }

    drawMouse(p);
}

void ClatashaInputHudWindow::refreshRenderedFrame()
{
    if (!enabled_)
        return;

    QImage frame(
        size(),
        QImage::Format_ARGB32_Premultiplied);
    frame.fill(Qt::transparent);

    {
        QPainter painter(&frame);
        drawContent(painter);
    }

    QMutexLocker locker(&frameMutex_);
    renderedFrame_ = frame;
}

void ClatashaInputHudWindow::paintEvent(QPaintEvent *)
{
    if (!enabled_ || outputMode_ == 1)
        return;

    QPainter painter(this);
    drawContent(painter);
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
