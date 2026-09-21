#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QObject>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <functional>
#include <string>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#endif

#include "hud-window.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("clatasha-hud", "en-US")

static ClatashaHudWindow *g_hud = nullptr;
static QAction *g_toolsAction = nullptr;
static QAction *g_settingsAction = nullptr;

namespace {

constexpr int kOverlayCount = 5;
constexpr int kCanvasWidth = 1920;
constexpr int kCanvasHeight = 1080;

enum class OverlayMode {
    Hud = 0,
    Video = 1,
    Both = 2,
};

struct OverlayConfig {
    bool enabled = false;
    QString name;
    QString url;
    OverlayMode mode = OverlayMode::Hud;
    int width = 600;
    int height = 300;
    int hudX = 80;
    int hudY = 80;
    int videoX = 660;
    int videoY = 40;
};


bool modeHasHud(OverlayMode mode);

struct QCefCookieManager;

class QCefWidget : public QWidget {
public:
    explicit QCefWidget(QWidget *parent = nullptr) : QWidget(parent) {}
    virtual void setURL(const std::string &url) = 0;
    virtual void setStartupScript(const std::string &script) = 0;
    virtual void allowAllPopups(bool allow) = 0;
    virtual void closeBrowser() = 0;
    virtual void reloadPage() = 0;
    virtual bool zoomPage(int direction) = 0;
    virtual void executeJavaScript(const std::string &script) = 0;
};

struct QCef {
    virtual ~QCef() = default;
    virtual bool init_browser(void) = 0;
    virtual bool initialized(void) = 0;
    virtual bool wait_for_browser_init(void) = 0;
    virtual QCefWidget *create_widget(QWidget *parent, const std::string &url,
                                      QCefCookieManager *cookieManager = nullptr) = 0;
};

QCef *g_browserEngine = nullptr;

QCef *ensureBrowserEngine()
{
    if (g_browserEngine)
        return g_browserEngine;

    obs_module_t *browserModule = obs_get_module("obs-browser");
    if (!browserModule) {
        blog(LOG_WARNING, "[Clatasha HUD] obs-browser module is unavailable for HUD overlays");
        return nullptr;
    }

    void *library = obs_get_module_lib(browserModule);
    if (!library) {
        blog(LOG_WARNING, "[Clatasha HUD] obs-browser library handle is unavailable");
        return nullptr;
    }

    using CreateQCefFn = QCef *(*)();
    auto createQCef =
        reinterpret_cast<CreateQCefFn>(os_dlsym(library, "obs_browser_create_qcef"));
    if (!createQCef) {
        blog(LOG_WARNING, "[Clatasha HUD] obs-browser QCEF interface is unavailable");
        return nullptr;
    }

    g_browserEngine = createQCef();
    if (!g_browserEngine)
        return nullptr;

    if (!g_browserEngine->initialized()) {
        g_browserEngine->init_browser();
        if (!g_browserEngine->wait_for_browser_init()) {
            blog(LOG_WARNING, "[Clatasha HUD] Browser engine failed to initialize");
            return nullptr;
        }
    }

    blog(LOG_INFO, "[Clatasha HUD] Browser HUD engine initialized");
    return g_browserEngine;
}

class BrowserHudOverlay final : public QWidget {
public:
    explicit BrowserHudOverlay(int index)
        : QWidget(nullptr), index_(index)
    {
        setObjectName(QStringLiteral("ClatashaBrowserHUD%1").arg(index + 1));
        setWindowTitle(QStringLiteral("Clatasha HUD Browser %1").arg(index + 1));
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                       Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NativeWindow, true);
        setWindowFlag(Qt::WindowTransparentForInput, true);
        setStyleSheet(QStringLiteral("background: transparent;"));
    }

    ~BrowserHudOverlay() override
    {
        if (browser_) {
            browser_->closeBrowser();
            browser_ = nullptr;
        }
    }

    bool applyConfig(const OverlayConfig &config)
    {
        QCef *engine = ensureBrowserEngine();
        if (!engine)
            return false;

        const std::string url = config.url.toStdString();

        if (!browser_) {
            browser_ = engine->create_widget(this, url, nullptr);
            if (!browser_) {
                blog(LOG_WARNING, "[Clatasha HUD] Failed to create HUD browser widget %d", index_ + 1);
                return false;
            }

            browser_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            browser_->setAttribute(Qt::WA_TranslucentBackground, true);
            browser_->setStyleSheet(QStringLiteral("background: transparent;"));
            browser_->setStartupScript(
                "document.documentElement.style.background='transparent';"
                "if(document.body)document.body.style.background='transparent';");
            browser_->show();
        } else if (config.url != url_) {
            browser_->setURL(url);
        }

        url_ = config.url;
        positionFromConfig(config);

        browser_->setGeometry(rect());
        show();
        raise();

#ifdef Q_OS_WIN
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        if (hwnd && !SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
            blog(LOG_DEBUG,
                 "[Clatasha HUD] Capture exclusion unavailable for HUD browser %d: %lu",
                 index_ + 1, GetLastError());
        }
#endif

        return true;
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (browser_)
            browser_->setGeometry(rect());
    }

private:
    void positionFromConfig(const OverlayConfig &config)
    {
        QScreen *screen = QGuiApplication::primaryScreen();
        if (!screen)
            return;

        const QRect screenRect = screen->geometry();
        const double sx = screenRect.width() / static_cast<double>(kCanvasWidth);
        const double sy = screenRect.height() / static_cast<double>(kCanvasHeight);

        const int x = screenRect.left() + qRound(config.hudX * sx);
        const int y = screenRect.top() + qRound(config.hudY * sy);
        const int width = qMax(40, qRound(config.width * sx));
        const int height = qMax(40, qRound(config.height * sy));

        setGeometry(x, y, width, height);
    }

    int index_ = 0;
    QCefWidget *browser_ = nullptr;
    QString url_;
};

std::array<BrowserHudOverlay *, kOverlayCount> g_hudBrowserOverlays{};

bool applyHudOverlays(const std::array<OverlayConfig, kOverlayCount> &configs)
{
    bool browserAvailable = true;

    for (int i = 0; i < kOverlayCount; ++i) {
        const OverlayConfig &config = configs[i];
        const bool shouldShow = config.enabled && modeHasHud(config.mode) &&
                                !config.url.trimmed().isEmpty();

        if (!shouldShow) {
            delete g_hudBrowserOverlays[i];
            g_hudBrowserOverlays[i] = nullptr;
            continue;
        }

        if (!g_hudBrowserOverlays[i])
            g_hudBrowserOverlays[i] = new BrowserHudOverlay(i);

        if (!g_hudBrowserOverlays[i]->applyConfig(config))
            browserAvailable = false;
    }

    return browserAvailable;
}

void destroyHudOverlays()
{
    for (BrowserHudOverlay *&overlay : g_hudBrowserOverlays) {
        delete overlay;
        overlay = nullptr;
    }
}

QString settingsFilePath()
{
    char *path = obs_module_config_path("settings.ini");
    if (!path)
        return {};

    const QString result = QString::fromUtf8(path);
    bfree(path);
    return result;
}

QString overlaySourceName(int index)
{
    return QStringLiteral("Clatasha Browser %1").arg(index + 1);
}

std::array<OverlayConfig, kOverlayCount> loadOverlayConfigs()
{
    std::array<OverlayConfig, kOverlayCount> configs{};
    const QString path = settingsFilePath();
    if (path.isEmpty())
        return configs;

    QSettings settings(path, QSettings::IniFormat);

    for (int i = 0; i < kOverlayCount; ++i) {
        const QString prefix = QStringLiteral("browser/%1/").arg(i + 1);
        OverlayConfig &config = configs[i];

        config.enabled = settings.value(prefix + QStringLiteral("enabled"), false).toBool();
        config.name = settings.value(prefix + QStringLiteral("name"),
                                     QStringLiteral("Overlay %1").arg(i + 1)).toString();
        config.url = settings.value(prefix + QStringLiteral("url")).toString();
        config.mode = static_cast<OverlayMode>(
            qBound(0, settings.value(prefix + QStringLiteral("mode"), 0).toInt(), 2));
        config.width = qBound(100, settings.value(prefix + QStringLiteral("width"), 600).toInt(), 3840);
        config.height = qBound(80, settings.value(prefix + QStringLiteral("height"), 300).toInt(), 2160);
        config.hudX = qBound(0, settings.value(prefix + QStringLiteral("hudX"), 80).toInt(),
                             kCanvasWidth - 20);
        config.hudY = qBound(0, settings.value(prefix + QStringLiteral("hudY"), 80).toInt(),
                             kCanvasHeight - 20);
        config.videoX = qBound(0, settings.value(prefix + QStringLiteral("videoX"), 660).toInt(),
                               kCanvasWidth - 20);
        config.videoY = qBound(0, settings.value(prefix + QStringLiteral("videoY"), 40).toInt(),
                               kCanvasHeight - 20);
    }

    return configs;
}

void saveOverlayConfigs(const std::array<OverlayConfig, kOverlayCount> &configs)
{
    const QString path = settingsFilePath();
    if (path.isEmpty())
        return;

    QSettings settings(path, QSettings::IniFormat);

    for (int i = 0; i < kOverlayCount; ++i) {
        const QString prefix = QStringLiteral("browser/%1/").arg(i + 1);
        const OverlayConfig &config = configs[i];

        settings.setValue(prefix + QStringLiteral("enabled"), config.enabled);
        settings.setValue(prefix + QStringLiteral("name"), config.name);
        settings.setValue(prefix + QStringLiteral("url"), config.url);
        settings.setValue(prefix + QStringLiteral("mode"), static_cast<int>(config.mode));
        settings.setValue(prefix + QStringLiteral("width"), config.width);
        settings.setValue(prefix + QStringLiteral("height"), config.height);
        settings.setValue(prefix + QStringLiteral("hudX"), config.hudX);
        settings.setValue(prefix + QStringLiteral("hudY"), config.hudY);
        settings.setValue(prefix + QStringLiteral("videoX"), config.videoX);
        settings.setValue(prefix + QStringLiteral("videoY"), config.videoY);
    }

    settings.sync();
}

bool modeHasVideo(OverlayMode mode)
{
    return mode == OverlayMode::Video || mode == OverlayMode::Both;
}

bool modeHasHud(OverlayMode mode)
{
    return mode == OverlayMode::Hud || mode == OverlayMode::Both;
}

bool applyVideoOverlays(const std::array<OverlayConfig, kOverlayCount> &configs)
{
    obs_source_t *sceneSource = obs_frontend_get_current_scene();
    if (!sceneSource)
        return false;

    obs_scene_t *scene = obs_scene_from_source(sceneSource);
    bool browserAvailable = true;

    for (int i = 0; i < kOverlayCount; ++i) {
        const OverlayConfig &config = configs[i];
        const QByteArray sourceName = overlaySourceName(i).toUtf8();
        const bool shouldShow = config.enabled && modeHasVideo(config.mode) &&
                                !config.url.trimmed().isEmpty();

        obs_sceneitem_t *item = scene ? obs_scene_find_source(scene, sourceName.constData()) : nullptr;

        if (!shouldShow) {
            if (item)
                obs_sceneitem_remove(item);
            continue;
        }

        obs_data_t *settings = obs_data_create();
        obs_data_set_string(settings, "url", config.url.toUtf8().constData());
        obs_data_set_int(settings, "width", config.width);
        obs_data_set_int(settings, "height", config.height);
        obs_data_set_int(settings, "fps", 30);
        obs_data_set_bool(settings, "shutdown", false);
        obs_data_set_bool(settings, "restart_when_active", false);

        obs_source_t *source = obs_get_source_by_name(sourceName.constData());
        if (!source) {
            source = obs_source_create("browser_source", sourceName.constData(), settings, nullptr);
            if (!source) {
                browserAvailable = false;
                obs_data_release(settings);
                continue;
            }
        } else {
            obs_source_update(source, settings);
        }

        obs_data_release(settings);

        if (scene && !item)
            item = obs_scene_add(scene, source);

        if (item) {
            struct vec2 pos;
            pos.x = static_cast<float>(config.videoX);
            pos.y = static_cast<float>(config.videoY);
            obs_sceneitem_set_pos(item, &pos);
            obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
        }

        obs_source_release(source);
    }

    obs_source_release(sceneSource);
    return browserAvailable;
}

class OverlayPreviewCanvas final : public QWidget {
public:
    explicit OverlayPreviewCanvas(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(640, 360);
        setMouseTracking(true);
    }

    void setGeometryData(int x, int y, int width, int height)
    {
        logicalRect_ = QRect(x, y, width, height);
        clampRect();
        update();
    }

    QRect geometryData() const { return logicalRect_; }

    void setLabel(const QString &label)
    {
        label_ = label;
        update();
    }

    void setChangedCallback(std::function<void(const QRect &)> callback)
    {
        changedCallback_ = std::move(callback);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        painter.fillRect(rect(), QColor(8, 11, 15));

        const QRect viewport = canvasRect();
        painter.fillRect(viewport, QColor(20, 25, 31));

        painter.setPen(QPen(QColor(38, 45, 53), 1));
        constexpr int divisions = 12;
        for (int i = 1; i < divisions; ++i) {
            const int x = viewport.left() + i * viewport.width() / divisions;
            painter.drawLine(x, viewport.top(), x, viewport.bottom());
        }
        for (int i = 1; i < 7; ++i) {
            const int y = viewport.top() + i * viewport.height() / 7;
            painter.drawLine(viewport.left(), y, viewport.right(), y);
        }

        const QRect visual = toVisual(logicalRect_);
        painter.fillRect(visual, QColor(45, 143, 255, 38));
        painter.setPen(QPen(QColor(67, 159, 255), 2));
        painter.drawRect(visual);

        painter.setPen(QColor(225, 232, 238));
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold));
        painter.drawText(visual.adjusted(10, 10, -10, -10),
                         Qt::AlignCenter | Qt::TextWordWrap,
                         label_.isEmpty() ? QStringLiteral("Browser Overlay") : label_);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(230, 235, 240));
        for (const QPoint &point : handlePoints(visual))
            painter.drawRoundedRect(QRect(point.x() - 4, point.y() - 4, 8, 8), 2, 2);

        painter.setPen(QColor(112, 124, 136));
        painter.setFont(QFont(QStringLiteral("Segoe UI"), 8));
        painter.drawText(viewport.adjusted(6, 4, -6, -4),
                         Qt::AlignLeft | Qt::AlignTop,
                         QStringLiteral("1920 × 1080"));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const QRect visual = toVisual(logicalRect_);
        activeHandle_ = hitHandle(event->position().toPoint(), visual);
        dragging_ = activeHandle_ != Handle::None || visual.contains(event->position().toPoint());
        if (!dragging_)
            return;

        if (activeHandle_ == Handle::None)
            activeHandle_ = Handle::Move;

        dragStart_ = event->position().toPoint();
        startRect_ = logicalRect_;
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!dragging_) {
            updateCursor(event->position().toPoint());
            return;
        }

        const QRect viewport = canvasRect();
        if (viewport.width() <= 0 || viewport.height() <= 0)
            return;

        const QPoint pixelDelta = event->position().toPoint() - dragStart_;
        const int dx = qRound(pixelDelta.x() * static_cast<double>(kCanvasWidth) / viewport.width());
        const int dy = qRound(pixelDelta.y() * static_cast<double>(kCanvasHeight) / viewport.height());

        QRect next = startRect_;

        switch (activeHandle_) {
        case Handle::Move:
            next.translate(dx, dy);
            break;
        case Handle::Left:
            next.setLeft(startRect_.left() + dx);
            break;
        case Handle::Right:
            next.setRight(startRect_.right() + dx);
            break;
        case Handle::Top:
            next.setTop(startRect_.top() + dy);
            break;
        case Handle::Bottom:
            next.setBottom(startRect_.bottom() + dy);
            break;
        case Handle::TopLeft:
            next.setTop(startRect_.top() + dy);
            next.setLeft(startRect_.left() + dx);
            break;
        case Handle::TopRight:
            next.setTop(startRect_.top() + dy);
            next.setRight(startRect_.right() + dx);
            break;
        case Handle::BottomLeft:
            next.setBottom(startRect_.bottom() + dy);
            next.setLeft(startRect_.left() + dx);
            break;
        case Handle::BottomRight:
            next.setBottom(startRect_.bottom() + dy);
            next.setRight(startRect_.right() + dx);
            break;
        default:
            break;
        }

        if (next.width() < 100) {
            if (activeHandle_ == Handle::Left || activeHandle_ == Handle::TopLeft ||
                activeHandle_ == Handle::BottomLeft)
                next.setLeft(next.right() - 99);
            else
                next.setRight(next.left() + 99);
        }

        if (next.height() < 80) {
            if (activeHandle_ == Handle::Top || activeHandle_ == Handle::TopLeft ||
                activeHandle_ == Handle::TopRight)
                next.setTop(next.bottom() - 79);
            else
                next.setBottom(next.top() + 79);
        }

        logicalRect_ = next;
        clampRect();
        update();

        if (changedCallback_)
            changedCallback_(logicalRect_);

        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        dragging_ = false;
        activeHandle_ = Handle::None;
        updateCursor(event->position().toPoint());
        event->accept();
    }

private:
    enum class Handle {
        None,
        Move,
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };

    QRect canvasRect() const
    {
        const int margin = 18;
        QRect available = rect().adjusted(margin, margin, -margin, -margin);
        const double targetAspect = 16.0 / 9.0;
        int width = available.width();
        int height = qRound(width / targetAspect);

        if (height > available.height()) {
            height = available.height();
            width = qRound(height * targetAspect);
        }

        return QRect((this->width() - width) / 2, (this->height() - height) / 2, width, height);
    }

    QRect toVisual(const QRect &logical) const
    {
        const QRect viewport = canvasRect();
        const double sx = viewport.width() / static_cast<double>(kCanvasWidth);
        const double sy = viewport.height() / static_cast<double>(kCanvasHeight);

        return QRect(viewport.left() + qRound(logical.x() * sx),
                     viewport.top() + qRound(logical.y() * sy),
                     qMax(1, qRound(logical.width() * sx)),
                     qMax(1, qRound(logical.height() * sy)));
    }

    std::array<QPoint, 8> handlePoints(const QRect &rect) const
    {
        return {
            rect.topLeft(),
            QPoint(rect.center().x(), rect.top()),
            rect.topRight(),
            QPoint(rect.right(), rect.center().y()),
            rect.bottomRight(),
            QPoint(rect.center().x(), rect.bottom()),
            rect.bottomLeft(),
            QPoint(rect.left(), rect.center().y()),
        };
    }

    Handle hitHandle(const QPoint &point, const QRect &visual) const
    {
        const auto points = handlePoints(visual);
        constexpr int radius = 8;

        for (int i = 0; i < static_cast<int>(points.size()); ++i) {
            const QRect hit(points[i].x() - radius, points[i].y() - radius, radius * 2, radius * 2);
            if (!hit.contains(point))
                continue;

            static constexpr Handle mapping[] = {
                Handle::TopLeft, Handle::Top, Handle::TopRight, Handle::Right,
                Handle::BottomRight, Handle::Bottom, Handle::BottomLeft, Handle::Left,
            };
            return mapping[i];
        }

        return Handle::None;
    }

    void updateCursor(const QPoint &point)
    {
        const QRect visual = toVisual(logicalRect_);
        const Handle handle = hitHandle(point, visual);

        switch (handle) {
        case Handle::TopLeft:
        case Handle::BottomRight:
            setCursor(Qt::SizeFDiagCursor);
            break;
        case Handle::TopRight:
        case Handle::BottomLeft:
            setCursor(Qt::SizeBDiagCursor);
            break;
        case Handle::Top:
        case Handle::Bottom:
            setCursor(Qt::SizeVerCursor);
            break;
        case Handle::Left:
        case Handle::Right:
            setCursor(Qt::SizeHorCursor);
            break;
        default:
            setCursor(visual.contains(point) ? Qt::SizeAllCursor : Qt::ArrowCursor);
            break;
        }
    }

    void clampRect()
    {
        if (logicalRect_.width() > kCanvasWidth)
            logicalRect_.setWidth(kCanvasWidth);
        if (logicalRect_.height() > kCanvasHeight)
            logicalRect_.setHeight(kCanvasHeight);

        if (logicalRect_.left() < 0)
            logicalRect_.moveLeft(0);
        if (logicalRect_.top() < 0)
            logicalRect_.moveTop(0);
        if (logicalRect_.right() >= kCanvasWidth)
            logicalRect_.moveRight(kCanvasWidth - 1);
        if (logicalRect_.bottom() >= kCanvasHeight)
            logicalRect_.moveBottom(kCanvasHeight - 1);
    }

    QRect logicalRect_{80, 80, 600, 300};
    QRect startRect_;
    QPoint dragStart_;
    QString label_;
    Handle activeHandle_ = Handle::None;
    bool dragging_ = false;
    std::function<void(const QRect &)> changedCallback_;
};

void showOverlayPreview(QWidget *parent, OverlayConfig &config)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Clatasha Overlay Preview"));
    dialog.setModal(true);
    dialog.resize(760, 540);

    auto *modeTabs = new QTabWidget(&dialog);
    auto *hudPage = new QWidget(modeTabs);
    auto *videoPage = new QWidget(modeTabs);

    auto buildPage = [&](QWidget *page, bool video) {
        auto *canvas = new OverlayPreviewCanvas(page);
        canvas->setLabel(config.name);

        const QRect initial(video ? config.videoX : config.hudX,
                            video ? config.videoY : config.hudY,
                            config.width, config.height);
        canvas->setGeometryData(initial.x(), initial.y(), initial.width(), initial.height());

        auto *xSpin = new QSpinBox(page);
        auto *ySpin = new QSpinBox(page);
        auto *wSpin = new QSpinBox(page);
        auto *hSpin = new QSpinBox(page);

        xSpin->setRange(0, kCanvasWidth - 20);
        ySpin->setRange(0, kCanvasHeight - 20);
        wSpin->setRange(100, 3840);
        hSpin->setRange(80, 2160);

        xSpin->setValue(initial.x());
        ySpin->setValue(initial.y());
        wSpin->setValue(initial.width());
        hSpin->setValue(initial.height());

        auto updateFromSpins = [=]() {
            canvas->setGeometryData(xSpin->value(), ySpin->value(), wSpin->value(), hSpin->value());
        };

        QObject::connect(xSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
                         [=](int) { updateFromSpins(); });
        QObject::connect(ySpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
                         [=](int) { updateFromSpins(); });
        QObject::connect(wSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
                         [=](int) { updateFromSpins(); });
        QObject::connect(hSpin, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
                         [=](int) { updateFromSpins(); });

        canvas->setChangedCallback([=](const QRect &rect) {
            QSignalBlocker bx(xSpin);
            QSignalBlocker by(ySpin);
            QSignalBlocker bw(wSpin);
            QSignalBlocker bh(hSpin);
            xSpin->setValue(rect.x());
            ySpin->setValue(rect.y());
            wSpin->setValue(rect.width());
            hSpin->setValue(rect.height());
        });

        auto *centerButton = new QPushButton(QStringLiteral("Center"), page);
        auto *resetButton = new QPushButton(QStringLiteral("Reset Size"), page);

        QObject::connect(centerButton, &QPushButton::clicked, &dialog, [=]() {
            const int x = (kCanvasWidth - wSpin->value()) / 2;
            const int y = (kCanvasHeight - hSpin->value()) / 2;
            xSpin->setValue(qMax(0, x));
            ySpin->setValue(qMax(0, y));
        });

        QObject::connect(resetButton, &QPushButton::clicked, &dialog, [=]() {
            wSpin->setValue(600);
            hSpin->setValue(300);
        });

        auto *controls = new QHBoxLayout();
        controls->addWidget(new QLabel(QStringLiteral("X"), page));
        controls->addWidget(xSpin);
        controls->addWidget(new QLabel(QStringLiteral("Y"), page));
        controls->addWidget(ySpin);
        controls->addSpacing(8);
        controls->addWidget(new QLabel(QStringLiteral("W"), page));
        controls->addWidget(wSpin);
        controls->addWidget(new QLabel(QStringLiteral("H"), page));
        controls->addWidget(hSpin);
        controls->addStretch();
        controls->addWidget(centerButton);
        controls->addWidget(resetButton);

        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->addWidget(canvas, 1);
        layout->addLayout(controls);

        return std::array<QSpinBox *, 4>{xSpin, ySpin, wSpin, hSpin};
    };

    const auto hudSpins = buildPage(hudPage, false);
    const auto videoSpins = buildPage(videoPage, true);

    modeTabs->addTab(hudPage, QStringLiteral("HUD Placement"));
    modeTabs->addTab(videoPage, QStringLiteral("VIDEO Placement"));

    if (config.mode == OverlayMode::Video)
        modeTabs->setCurrentWidget(videoPage);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);

    auto *mainLayout = new QVBoxLayout(&dialog);
    mainLayout->addWidget(modeTabs, 1);
    mainLayout->addWidget(buttons);

    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background:#0d1014; color:#e8edf2; }"
        "QTabWidget::pane { border:1px solid #2a3038; background:#11151a; border-radius:6px; }"
        "QTabBar::tab { background:#171c22; color:#aeb7c0; padding:8px 16px; margin-right:2px; }"
        "QTabBar::tab:selected { background:#2389ff; color:white; }"
        "QSpinBox { background:#171c22; color:#e8edf2; border:1px solid #303843; border-radius:4px; padding:4px; }"
        "QPushButton { background:#20262d; color:#e8edf2; border:1px solid #343d47; border-radius:5px; padding:6px 12px; }"
        "QPushButton:hover { background:#2a323b; }"));

    if (dialog.exec() == QDialog::Accepted) {
        config.hudX = hudSpins[0]->value();
        config.hudY = hudSpins[1]->value();
        config.videoX = videoSpins[0]->value();
        config.videoY = videoSpins[1]->value();
        config.width = qBound(100, videoSpins[2]->value(), 3840);
        config.height = qBound(80, videoSpins[3]->value(), 2160);

        // Keep one shared browser size for HUD and VIDEO in the first iteration.
        if (modeTabs->currentWidget() == hudPage) {
            config.width = qBound(100, hudSpins[2]->value(), 3840);
            config.height = qBound(80, hudSpins[3]->value(), 2160);
        }
    }
}

struct OverlayRow {
    QCheckBox *enabled = nullptr;
    QLineEdit *name = nullptr;
    QLineEdit *url = nullptr;
    QComboBox *mode = nullptr;
    QLabel *sizeLabel = nullptr;
    QPushButton *preview = nullptr;
};

} // namespace

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Native local-only HUD for OBS Studio";
}

static void ensure_hud()
{
    if (!g_hud)
        g_hud = new ClatashaHudWindow();
}

static void toggle_hud()
{
    ensure_hud();

    if (g_hud->isVisible()) {
        g_hud->hide();
    } else {
        g_hud->show();
        g_hud->positionHud();
        g_hud->raise();
    }
}

static void show_settings()
{
    ensure_hud();

    const int originalOpacity = g_hud->opacityPercent();
    const QString originalLocation = g_hud->location();
    auto overlayConfigs = loadOverlayConfigs();

    QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Clatasha HUD Settings"));
    dialog.setModal(true);
    dialog.resize(790, 720);
    dialog.setMinimumSize(720, 620);

    auto *tabs = new QTabWidget(&dialog);

    // HUD tab
    auto *hudTab = new QWidget(tabs);
    auto *opacitySlider = new QSlider(Qt::Horizontal, hudTab);
    opacitySlider->setRange(10, 100);
    opacitySlider->setValue(originalOpacity);

    auto *opacityValue = new QLabel(QStringLiteral("%1%").arg(originalOpacity), hudTab);
    auto *opacityRow = new QWidget(hudTab);
    auto *opacityLayout = new QHBoxLayout(opacityRow);
    opacityLayout->setContentsMargins(0, 0, 0, 0);
    opacityLayout->addWidget(opacitySlider, 1);
    opacityLayout->addWidget(opacityValue);

    auto *locationBox = new QComboBox(hudTab);
    locationBox->addItem(QStringLiteral("Top Right"), QStringLiteral("top-right"));
    locationBox->addItem(QStringLiteral("Top Left"), QStringLiteral("top-left"));
    locationBox->addItem(QStringLiteral("Bottom Right"), QStringLiteral("bottom-right"));
    locationBox->addItem(QStringLiteral("Bottom Left"), QStringLiteral("bottom-left"));

    const int currentLocation = locationBox->findData(originalLocation);
    locationBox->setCurrentIndex(currentLocation >= 0 ? currentLocation : 0);

    auto *hudCard = new QGroupBox(QStringLiteral("HUD"), hudTab);
    auto *hudForm = new QFormLayout(hudCard);
    hudForm->setContentsMargins(18, 22, 18, 18);
    hudForm->setSpacing(12);
    hudForm->addRow(QStringLiteral("Opacity"), opacityRow);
    hudForm->addRow(QStringLiteral("HUD location"), locationBox);

    auto *hudInfo = new QLabel(
        QStringLiteral("The desktop-audio meter now uses a small monitor icon and Mic/Aux uses a microphone icon."),
        hudTab);
    hudInfo->setWordWrap(true);
    hudInfo->setProperty("muted", true);

    auto *hudLayout = new QVBoxLayout(hudTab);
    hudLayout->setContentsMargins(16, 16, 16, 16);
    hudLayout->addWidget(hudCard);
    hudLayout->addWidget(hudInfo);
    hudLayout->addStretch();

    tabs->addTab(hudTab, QStringLiteral("HUD"));

    // Browser overlays tab
    auto *browserTab = new QWidget(tabs);
    auto *browserLayout = new QVBoxLayout(browserTab);
    browserLayout->setContentsMargins(16, 16, 16, 16);
    browserLayout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("Browser Overlays"), browserTab);
    QFont titleFont = title->font();
    titleFont.setPointSize(15);
    titleFont.setBold(true);
    title->setFont(titleFont);

    auto *subtitle = new QLabel(
        QStringLiteral("Add up to five Streamlabs or browser-widget URLs. HUD shows a private desktop overlay, VIDEO adds it to the current OBS scene, and HUD / VIDEO does both."),
        browserTab);
    subtitle->setWordWrap(true);
    subtitle->setProperty("muted", true);

    browserLayout->addWidget(title);
    browserLayout->addWidget(subtitle);

    auto *scroll = new QScrollArea(browserTab);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *scrollBody = new QWidget(scroll);
    auto *cards = new QVBoxLayout(scrollBody);
    cards->setContentsMargins(0, 4, 4, 4);
    cards->setSpacing(10);

    std::array<OverlayRow, kOverlayCount> rows{};

    for (int i = 0; i < kOverlayCount; ++i) {
        auto *card = new QGroupBox(QStringLiteral("Overlay %1").arg(i + 1), scrollBody);
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 20, 14, 14);
        cardLayout->setSpacing(9);

        auto *topRow = new QHBoxLayout();
        rows[i].enabled = new QCheckBox(QStringLiteral("Active"), card);
        rows[i].name = new QLineEdit(overlayConfigs[i].name, card);
        rows[i].name->setPlaceholderText(QStringLiteral("Name"));
        rows[i].mode = new QComboBox(card);
        rows[i].mode->addItem(QStringLiteral("HUD"), static_cast<int>(OverlayMode::Hud));
        rows[i].mode->addItem(QStringLiteral("VIDEO"), static_cast<int>(OverlayMode::Video));
        rows[i].mode->addItem(QStringLiteral("HUD / VIDEO"), static_cast<int>(OverlayMode::Both));
        rows[i].mode->setCurrentIndex(rows[i].mode->findData(static_cast<int>(overlayConfigs[i].mode)));

        rows[i].enabled->setChecked(overlayConfigs[i].enabled);

        topRow->addWidget(rows[i].enabled);
        topRow->addWidget(rows[i].name, 1);
        topRow->addWidget(rows[i].mode);

        rows[i].url = new QLineEdit(overlayConfigs[i].url, card);
        rows[i].url->setPlaceholderText(QStringLiteral("https://streamlabs.com/..."));

        auto *bottomRow = new QHBoxLayout();
        rows[i].sizeLabel = new QLabel(
            QStringLiteral("%1 × %2").arg(overlayConfigs[i].width).arg(overlayConfigs[i].height), card);
        rows[i].sizeLabel->setProperty("muted", true);
        rows[i].preview = new QPushButton(QStringLiteral("Preview / Edit"), card);

        bottomRow->addWidget(rows[i].sizeLabel);
        bottomRow->addStretch();
        bottomRow->addWidget(rows[i].preview);

        cardLayout->addLayout(topRow);
        cardLayout->addWidget(rows[i].url);
        cardLayout->addLayout(bottomRow);
        cards->addWidget(card);

        QObject::connect(rows[i].preview, &QPushButton::clicked, &dialog, [&, i]() {
            overlayConfigs[i].enabled = rows[i].enabled->isChecked();
            overlayConfigs[i].name = rows[i].name->text().trimmed();
            overlayConfigs[i].url = rows[i].url->text().trimmed();
            overlayConfigs[i].mode = static_cast<OverlayMode>(rows[i].mode->currentData().toInt());

            showOverlayPreview(&dialog, overlayConfigs[i]);

            rows[i].sizeLabel->setText(
                QStringLiteral("%1 × %2").arg(overlayConfigs[i].width).arg(overlayConfigs[i].height));
        });
    }

    cards->addStretch();
    scrollBody->setLayout(cards);
    scroll->setWidget(scrollBody);
    browserLayout->addWidget(scroll, 1);

    auto *videoNote = new QLabel(
        QStringLiteral("HUD overlays appear on your desktop and are excluded from Windows capture when supported. VIDEO overlays are added to the current OBS scene when you press Apply or OK."),
        browserTab);
    videoNote->setWordWrap(true);
    videoNote->setProperty("accentNote", true);
    browserLayout->addWidget(videoNote);

    tabs->addTab(browserTab, QStringLiteral("Browser Overlays"));

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, &dialog);

    auto syncRowsToConfigs = [&]() {
        for (int i = 0; i < kOverlayCount; ++i) {
            overlayConfigs[i].enabled = rows[i].enabled->isChecked();
            overlayConfigs[i].name = rows[i].name->text().trimmed();
            if (overlayConfigs[i].name.isEmpty())
                overlayConfigs[i].name = QStringLiteral("Overlay %1").arg(i + 1);
            overlayConfigs[i].url = rows[i].url->text().trimmed();
            overlayConfigs[i].mode = static_cast<OverlayMode>(rows[i].mode->currentData().toInt());
        }
    };

    auto applySettings = [&]() {
        syncRowsToConfigs();
        g_hud->saveSettings();
        saveOverlayConfigs(overlayConfigs);

        const bool videoOk = applyVideoOverlays(overlayConfigs);
        const bool hudOk = applyHudOverlays(overlayConfigs);

        if (!videoOk || !hudOk) {
            QMessageBox::warning(
                &dialog,
                QStringLiteral("Browser Overlay Unavailable"),
                QStringLiteral("One or more browser overlays could not be created. Make sure the OBS Browser plugin is installed and enabled."));
        }
    };

    QObject::connect(opacitySlider, &QSlider::valueChanged, [&](int value) {
        opacityValue->setText(QStringLiteral("%1%").arg(value));
        g_hud->setOpacityPercent(value);
    });

    QObject::connect(locationBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [&](int) {
        g_hud->setLocation(locationBox->currentData().toString());
    });

    QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog,
                     [&]() { applySettings(); });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        applySettings();
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->addWidget(tabs, 1);
    layout->addWidget(buttons);

    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background:#0c0f13; color:#e9eef3; }"
        "QWidget { color:#e9eef3; font-family:'Segoe UI'; font-size:10pt; }"
        "QTabWidget::pane { border:1px solid #252c34; background:#101419; border-radius:7px; }"
        "QTabBar::tab { background:#151a20; color:#9da8b3; padding:9px 18px; margin-right:2px; border-top-left-radius:5px; border-top-right-radius:5px; }"
        "QTabBar::tab:selected { background:#2389ff; color:white; }"
        "QGroupBox { background:#151a20; border:1px solid #29313a; border-radius:8px; margin-top:10px; font-weight:600; }"
        "QGroupBox::title { subcontrol-origin:margin; left:12px; padding:0 5px; color:#dfe7ee; }"
        "QLineEdit,QComboBox,QSpinBox { background:#0f1318; border:1px solid #303943; border-radius:5px; padding:6px 8px; selection-background-color:#2389ff; }"
        "QLineEdit:focus,QComboBox:focus,QSpinBox:focus { border:1px solid #2389ff; }"
        "QPushButton { background:#20262d; border:1px solid #343e48; border-radius:5px; padding:7px 13px; }"
        "QPushButton:hover { background:#29313a; border-color:#46525f; }"
        "QPushButton:pressed { background:#1b2026; }"
        "QCheckBox { spacing:7px; }"
        "QSlider::groove:horizontal { height:4px; background:#2a3139; border-radius:2px; }"
        "QSlider::handle:horizontal { width:14px; margin:-5px 0; background:#2389ff; border-radius:7px; }"
        "QScrollArea { background:transparent; }"
        "QScrollBar:vertical { background:#0f1318; width:9px; }"
        "QScrollBar::handle:vertical { background:#343e48; border-radius:4px; min-height:30px; }"
        "QLabel[muted='true'] { color:#8f9aa6; }"
        "QLabel[accentNote='true'] { color:#8cc5ff; background:#111d29; border:1px solid #24435e; border-radius:6px; padding:8px; }"));

    if (dialog.exec() != QDialog::Accepted) {
        g_hud->setOpacityPercent(originalOpacity);
        g_hud->setLocation(originalLocation);
    }
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
    switch (event) {
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
        ensure_hud();
        g_hud->show();
        g_hud->positionHud();
        {
            const auto configs = loadOverlayConfigs();
            applyVideoOverlays(configs);
            applyHudOverlays(configs);
            QTimer::singleShot(1200, []() { applyHudOverlays(loadOverlayConfigs()); });
        }
        break;

    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
        applyVideoOverlays(loadOverlayConfigs());
        break;

    case OBS_FRONTEND_EVENT_EXIT:
        destroyHudOverlays();
        if (g_hud) {
            g_hud->saveSettings();
            g_hud->close();
            delete g_hud;
            g_hud = nullptr;
        }
        break;

    default:
        break;
    }
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "[Clatasha HUD] Loading plugin");

    obs_frontend_add_event_callback(on_frontend_event, nullptr);

    g_toolsAction = static_cast<QAction *>(
        obs_frontend_add_tools_menu_qaction(obs_module_text("ClatashaHUD.Menu")));
    g_settingsAction = static_cast<QAction *>(
        obs_frontend_add_tools_menu_qaction(obs_module_text("ClatashaHUD.Settings")));

    if (g_toolsAction)
        QObject::connect(g_toolsAction, &QAction::triggered, []() { toggle_hud(); });
    if (g_settingsAction)
        QObject::connect(g_settingsAction, &QAction::triggered, []() { show_settings(); });

    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    destroyHudOverlays();

    if (g_toolsAction) {
        QObject::disconnect(g_toolsAction, nullptr, nullptr, nullptr);
        g_toolsAction = nullptr;
    }

    if (g_settingsAction) {
        QObject::disconnect(g_settingsAction, nullptr, nullptr, nullptr);
        g_settingsAction = nullptr;
    }

    if (g_hud) {
        g_hud->saveSettings();
        g_hud->close();
        delete g_hud;
        g_hud = nullptr;
    }

    blog(LOG_INFO, "[Clatasha HUD] Plugin unloaded");
}
