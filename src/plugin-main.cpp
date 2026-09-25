#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-hotkey.h>
#include <obs-interaction.h>
#include <util/dstr.h>

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDir>
#include <QFormLayout>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QFrame>
#include <QGroupBox>
#include <QGridLayout>
#include <QGuiApplication>
#include <QImage>
#include <QIcon>
#include <QKeyEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QObject>
#include <QPainter>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QVersionNumber>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QSize>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
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
static QTimer *g_hudWatchdog = nullptr;
static bool g_hudWantedVisible = true;
static bool g_frontendExiting = false;
static bool g_browserHudOverlaysWantedVisible = true;

#ifdef Q_OS_WIN
static HANDLE g_displayCaptureSafeEvent = nullptr;
static bool g_displayCaptureSafeActive = false;
static bool g_displayCaptureSafeModeEnabled = false;
#endif

namespace {

#ifndef CLATASHA_HUD_VERSION
#define CLATASHA_HUD_VERSION "0.0.0"
#endif

constexpr const char *kClatashaHudVersion = CLATASHA_HUD_VERSION;
constexpr qint64 kUpdateCheckIntervalSeconds = 24 * 60 * 60;
constexpr qint64 kUpdateRetryIntervalSeconds = 5 * 60;
const QUrl kLatestReleaseApiUrl(
    QStringLiteral("https://api.github.com/repos/Clatasha/Clatasha-HUD/releases/latest"));
const QUrl kLatestReleaseFallbackUrl(
    QStringLiteral("https://github.com/Clatasha/Clatasha-HUD/releases/latest"));

struct UpdateCheckState {
    bool requestInFlight = false;
    bool checkedSuccessfully = false;
    bool updateAvailable = false;
    qint64 lastAttemptEpoch = 0;
    qint64 lastSuccessfulCheckEpoch = 0;
    QString latestVersion;
    QUrl releaseUrl;
};

UpdateCheckState g_updateCheck;
#ifdef Q_OS_WIN
std::thread g_updateWorker;
QObject *g_updateCallbackContext = nullptr;
#endif
QPointer<QLabel> g_settingsVersionLabel;
QPointer<QLabel> g_settingsCurrentCheckIcon;
QPointer<QPushButton> g_settingsUpdateButton;

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

    // Browser/source canvas. This stays fixed while placement scales the
    // finished source, preventing resize handles from behaving like a crop.
    int sourceWidth = 600;
    int sourceHeight = 300;
    int opacityPercent = 100;

    int hudX = 80;
    int hudY = 80;
    int hudWidth = 600;
    int hudHeight = 300;
    bool hudLockRatio = true;

    int videoX = 660;
    int videoY = 40;
    int videoWidth = 600;
    int videoHeight = 300;
    bool videoLockRatio = true;
};

#ifdef Q_OS_WIN
struct GameBorderlessState {
    HWND window = nullptr;
    LONG_PTR style = 0;
    LONG_PTR exStyle = 0;
    RECT rect{};
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    bool active = false;
};

GameBorderlessState g_gameBorderless;
HWND g_lastExternalForegroundWindow = nullptr;
bool g_keepGameBorderlessApplied = true;
bool g_taskbarAutoHideChangedByClatasha = false;
bool g_displayCaptureForcedBorderless = false;

HWINEVENTHOOK g_topmostForegroundHook = nullptr;
HWINEVENTHOOK g_topmostShowHook = nullptr;
HWINEVENTHOOK g_topmostReorderHook = nullptr;
std::atomic_bool g_topmostReassertQueued{false};
#endif

QString localFilePathFromValue(const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return {};

    const QUrl url(trimmed);
    if (url.isLocalFile())
        return QDir::cleanPath(url.toLocalFile());

    const QFileInfo info(trimmed);
    if (info.exists() && info.isFile())
        return QDir::cleanPath(info.absoluteFilePath());

#ifdef Q_OS_WIN
    // Preserve a manually entered Windows absolute path even before QFileInfo
    // has resolved it, so it can be reported/loaded consistently.
    if (trimmed.size() >= 3 && trimmed.at(1) == QLatin1Char(':') &&
        (trimmed.at(2) == QLatin1Char('\\') || trimmed.at(2) == QLatin1Char('/')))
        return QDir::cleanPath(trimmed);
#endif

    return {};
}

bool isDirectImageUrl(const QString &value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.startsWith(QStringLiteral("data:image/"), Qt::CaseInsensitive))
        return true;

    QString path;
    const QString localPath = localFilePathFromValue(trimmed);
    if (!localPath.isEmpty())
        path = localPath.toLower();
    else
        path = QUrl(trimmed).path().toLower();

    static const std::array<const char *, 9> extensions = {
        ".png", ".apng", ".jpg", ".jpeg", ".gif",
        ".webp", ".svg", ".bmp", ".avif"
    };

    for (const char *extension : extensions) {
        if (path.endsWith(QLatin1String(extension)))
            return true;
    }

    return false;
}

QString imageMimeTypeForPath(const QString &path)
{
    const QString lower = path.toLower();

    if (lower.endsWith(QStringLiteral(".png")) ||
        lower.endsWith(QStringLiteral(".apng")))
        return QStringLiteral("image/png");
    if (lower.endsWith(QStringLiteral(".gif")))
        return QStringLiteral("image/gif");
    if (lower.endsWith(QStringLiteral(".webp")))
        return QStringLiteral("image/webp");
    if (lower.endsWith(QStringLiteral(".svg")))
        return QStringLiteral("image/svg+xml");
    if (lower.endsWith(QStringLiteral(".jpg")) ||
        lower.endsWith(QStringLiteral(".jpeg")))
        return QStringLiteral("image/jpeg");
    if (lower.endsWith(QStringLiteral(".bmp")))
        return QStringLiteral("image/bmp");
    if (lower.endsWith(QStringLiteral(".avif")))
        return QStringLiteral("image/avif");

    return QStringLiteral("application/octet-stream");
}

QString browserRenderableUrl(const QString &value)
{
    const QString trimmed = value.trimmed();
    if (!isDirectImageUrl(trimmed))
        return trimmed;

    QString imageSource = trimmed;
    const QString localPath = localFilePathFromValue(trimmed);

    if (!localPath.isEmpty()) {
        QFile file(localPath);
        if (!file.open(QIODevice::ReadOnly)) {
            blog(LOG_WARNING,
                 "[Clatasha HUD] Could not open local image overlay file");
            return {};
        }

        const QByteArray bytes = file.readAll();
        const QString mimeType = imageMimeTypeForPath(localPath);
        imageSource =
            QStringLiteral("data:%1;base64,%2")
                .arg(mimeType, QString::fromLatin1(bytes.toBase64()));
    }

    QString escaped = imageSource.toHtmlEscaped();
    escaped.replace(QStringLiteral("'"), QStringLiteral("&#39;"));

    const QString html = QStringLiteral(
        "<!doctype html>"
        "<html>"
        "<head>"
        "<meta charset='utf-8'>"
        "<style>"
        "html,body{"
        "width:100%%;height:100%%;margin:0;padding:0;"
        "overflow:hidden;background:rgba(0,0,0,0)!important;"
        "}"
        "body{display:flex;align-items:center;justify-content:center;}"
        "img{"
        "display:block;max-width:100%%;max-height:100%%;"
        "width:100%%;height:100%%;object-fit:contain;"
        "background:transparent;"
        "}"
        "</style>"
        "</head>"
        "<body><img src='%1' alt=''></body>"
        "</html>").arg(escaped);

    return QStringLiteral("data:text/html;charset=utf-8,") +
           QString::fromLatin1(QUrl::toPercentEncoding(html));
}

void applyBrowserInputSettings(obs_data_t *settings, const QString &value)
{
    const QString localPath = localFilePathFromValue(value);
    const bool localNonImage = !localPath.isEmpty() && !isDirectImageUrl(value);

    obs_data_set_bool(settings, "is_local_file", localNonImage);

    if (localNonImage) {
        obs_data_set_string(settings, "local_file", localPath.toUtf8().constData());
        obs_data_set_string(settings, "url", "");
    } else {
        const QString renderUrl = browserRenderableUrl(value);
        obs_data_set_string(settings, "local_file", "");
        obs_data_set_string(settings, "url", renderUrl.toUtf8().constData());
    }
}


bool modeHasHud(OverlayMode mode);

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
        setAutoFillBackground(false);

        obs_add_tick_callback(&BrowserHudOverlay::renderTick, this);
        tickRegistered_ = true;
    }

    ~BrowserHudOverlay() override
    {
        releaseObsResources();
    }

    void prepareForShutdown()
    {
        active_.store(false, std::memory_order_release);
        hide();
        frame_ = QImage();
        update();
        releaseObsResources();
    }

    bool applyConfig(const OverlayConfig &config)
    {
        if (config.url.trimmed().isEmpty())
            return false;

        const int newWidth = qBound(100, config.sourceWidth, 3840);
        const int newHeight = qBound(80, config.sourceHeight, 2160);

        obs_data_t *settings = obs_data_create();
        applyBrowserInputSettings(settings, config.url);
        obs_data_set_int(settings, "width", newWidth);
        obs_data_set_int(settings, "height", newHeight);
        obs_data_set_int(settings, "fps", 30);
        obs_data_set_bool(settings, "shutdown", false);
        obs_data_set_bool(settings, "restart_when_active", false);
        obs_data_set_string(
            settings,
            "css",
            "body { background-color: rgba(0, 0, 0, 0); margin: 0px auto; overflow: hidden; }");

        if (!source_) {
            const QByteArray sourceName =
                QStringLiteral("Clatasha HUD Private Browser %1").arg(index_ + 1).toUtf8();
            source_ =
                obs_source_create_private("browser_source", sourceName.constData(), settings);
            if (!source_) {
                obs_data_release(settings);
                blog(LOG_WARNING,
                     "[Clatasha HUD] Failed to create private browser_source for HUD overlay %d",
                     index_ + 1);
                return false;
            }

            obs_source_inc_showing(source_);
            sourceShowing_ = true;
        } else {
            obs_source_update(source_, settings);
            if (!sourceShowing_) {
                obs_source_inc_showing(source_);
                sourceShowing_ = true;
            }
        }

        obs_data_release(settings);

        renderWidth_.store(newWidth, std::memory_order_relaxed);
        renderHeight_.store(newHeight, std::memory_order_relaxed);
        active_.store(true, std::memory_order_release);

        setWindowOpacity(qBound(10, config.opacityPercent, 100) / 100.0);
        positionFromConfig(config);
        show();
        ensureTopmost();

#ifdef Q_OS_WIN
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        if (hwnd) {
            LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            exStyle |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);

            if (!SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
                blog(LOG_DEBUG,
                     "[Clatasha HUD] Capture exclusion unavailable for HUD browser %d: %lu",
                     index_ + 1, GetLastError());
            }
        }
#endif

        return true;
    }

    void deactivate()
    {
        active_.store(false, std::memory_order_release);
        hide();
        frame_ = QImage();
        update();
    }

    void ensureTopmost()
    {
        if (!active_.load(std::memory_order_acquire) || !isVisible())
            return;

#ifdef Q_OS_WIN
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        if (hwnd) {
            SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        }
#else
        raise();
#endif
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);

        if (!frame_.isNull()) {
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(rect(), frame_);
        }
    }

private:
    void releaseObsResources()
    {
        active_.store(false, std::memory_order_release);

        if (tickRegistered_) {
            obs_remove_tick_callback(&BrowserHudOverlay::renderTick, this);
            tickRegistered_ = false;
        }

        std::lock_guard<std::mutex> lock(renderMutex_);

        if (source_) {
            // Ask obs-browser to shut its CEF instance down while the browser
            // module and Qt event loop are still alive, then release our ref.
            obs_data_t *settings = obs_source_get_settings(source_);
            if (settings) {
                obs_data_set_bool(settings, "shutdown", true);
                obs_source_update(source_, settings);
                obs_data_release(settings);
            }

            if (sourceShowing_) {
                obs_source_dec_showing(source_);
                sourceShowing_ = false;
            }

            obs_source_release(source_);
            source_ = nullptr;
        }

        if (stageSurface_ || texRender_) {
            obs_enter_graphics();
            gs_stagesurface_destroy(stageSurface_);
            stageSurface_ = nullptr;
            gs_texrender_destroy(texRender_);
            texRender_ = nullptr;
            obs_leave_graphics();
        }
    }

    static void renderTick(void *param, float seconds)
    {
        auto *self = static_cast<BrowserHudOverlay *>(param);
        if (!self || !self->active_.load(std::memory_order_acquire))
            return;

        self->renderAccumulator_ += seconds;
        if (self->renderAccumulator_ < (1.0f / 30.0f))
            return;

        self->renderAccumulator_ = 0.0f;

        if (self->frameQueued_.load(std::memory_order_acquire))
            return;

        self->renderFrame();
    }

    void renderFrame()
    {
        std::lock_guard<std::mutex> lock(renderMutex_);

        const int width = renderWidth_.load(std::memory_order_relaxed);
        const int height = renderHeight_.load(std::memory_order_relaxed);

        if (!source_ || width <= 0 || height <= 0)
            return;

        QImage image;

        obs_enter_graphics();

        if (!texRender_)
            texRender_ = gs_texrender_create(GS_BGRA, GS_ZS_NONE);

        if (!stageSurface_ || stageWidth_ != width || stageHeight_ != height) {
            gs_stagesurface_destroy(stageSurface_);
            stageSurface_ = gs_stagesurface_create(
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height),
                GS_BGRA);
            stageWidth_ = width;
            stageHeight_ = height;
        }

        if (texRender_ && stageSurface_) {
            gs_texrender_reset(texRender_);

            if (gs_texrender_begin(
                    texRender_,
                    static_cast<uint32_t>(width),
                    static_cast<uint32_t>(height))) {
                vec4 clearColor;
                vec4_zero(&clearColor);
                gs_clear(GS_CLEAR_COLOR, &clearColor, 0.0f, 0);

                gs_viewport_push();
                gs_projection_push();

                gs_ortho(
                    0.0f,
                    static_cast<float>(width),
                    0.0f,
                    static_cast<float>(height),
                    -100.0f,
                    100.0f);
                gs_set_viewport(0, 0, width, height);

                obs_source_video_render(source_);

                gs_projection_pop();
                gs_viewport_pop();
                gs_texrender_end(texRender_);

                gs_stage_texture(stageSurface_, gs_texrender_get_texture(texRender_));

                uint8_t *data = nullptr;
                uint32_t linesize = 0;
                if (gs_stagesurface_map(stageSurface_, &data, &linesize)) {
                    image = QImage(
                        width,
                        height,
                        QImage::Format_ARGB32_Premultiplied);

                    const size_t rowBytes = static_cast<size_t>(width) * 4;
                    for (int y = 0; y < height; ++y) {
                        std::memcpy(
                            image.scanLine(y),
                            data + static_cast<size_t>(y) * linesize,
                            rowBytes);
                    }

                    gs_stagesurface_unmap(stageSurface_);
                }
            }
        }

        obs_leave_graphics();

        if (image.isNull())
            return;

        frameQueued_.store(true, std::memory_order_release);
        QMetaObject::invokeMethod(
            this,
            [this, image = std::move(image)]() mutable {
                frame_ = std::move(image);
                frameQueued_.store(false, std::memory_order_release);
                update();
            },
            Qt::QueuedConnection);
    }

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
        const int width = qMax(40, qRound(config.hudWidth * sx));
        const int height = qMax(40, qRound(config.hudHeight * sy));

        setGeometry(x, y, width, height);
    }

    int index_ = 0;
    obs_source_t *source_ = nullptr;
    bool sourceShowing_ = false;
    bool tickRegistered_ = false;
    std::mutex renderMutex_;

    gs_texrender_t *texRender_ = nullptr;
    gs_stagesurf_t *stageSurface_ = nullptr;
    int stageWidth_ = 0;
    int stageHeight_ = 0;

    std::atomic_bool active_{false};
    std::atomic_bool frameQueued_{false};
    std::atomic_int renderWidth_{600};
    std::atomic_int renderHeight_{300};
    float renderAccumulator_ = 0.0f;

    QImage frame_;
};

std::array<BrowserHudOverlay *, kOverlayCount> g_hudBrowserOverlays{};

void reassertClatashaTopmostWindows()
{
    if (g_frontendExiting)
        return;

#ifdef Q_OS_WIN
    if (g_hud && g_hudWantedVisible && g_hud->isVisible()) {
        const HWND hwnd = reinterpret_cast<HWND>(g_hud->winId());
        if (hwnd && IsWindow(hwnd)) {
            SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                    SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        }
    }

    if (g_browserHudOverlaysWantedVisible) {
        for (BrowserHudOverlay *overlay : g_hudBrowserOverlays) {
            if (overlay)
                overlay->ensureTopmost();
        }
    }
#else
    if (g_hud && g_hudWantedVisible && g_hud->isVisible())
        g_hud->raise();

    if (g_browserHudOverlaysWantedVisible) {
        for (BrowserHudOverlay *overlay : g_hudBrowserOverlays) {
            if (overlay)
                overlay->ensureTopmost();
        }
    }
#endif
}

#ifdef Q_OS_WIN
void queueTopmostReassert()
{
    if (g_frontendExiting || !g_hud)
        return;

    bool expected = false;
    if (!g_topmostReassertQueued.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel)) {
        return;
    }

    QPointer<ClatashaHudWindow> hudGuard(g_hud);
    QMetaObject::invokeMethod(
        g_hud,
        [hudGuard]() {
            g_topmostReassertQueued.store(false, std::memory_order_release);
            if (g_frontendExiting || !hudGuard)
                return;

            reassertClatashaTopmostWindows();
        },
        Qt::QueuedConnection);
}

void CALLBACK clatashaTopmostWinEvent(
    HWINEVENTHOOK,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD,
    DWORD)
{
    if (g_frontendExiting)
        return;

    // Foreground changes are always relevant. For object events, ignore
    // non-window child/control notifications to avoid unnecessary UI churn.
    if (event != EVENT_SYSTEM_FOREGROUND) {
        if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
            return;
    }

    if (hwnd) {
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (processId == GetCurrentProcessId())
            return;
    }

    queueTopmostReassert();
}

void startTopmostEventHooks()
{
    if (g_topmostForegroundHook || g_topmostShowHook ||
        g_topmostReorderHook) {
        return;
    }

    constexpr DWORD hookFlags =
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;

    g_topmostForegroundHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        nullptr,
        clatashaTopmostWinEvent,
        0,
        0,
        hookFlags);

    g_topmostShowHook = SetWinEventHook(
        EVENT_OBJECT_SHOW,
        EVENT_OBJECT_SHOW,
        nullptr,
        clatashaTopmostWinEvent,
        0,
        0,
        hookFlags);

    g_topmostReorderHook = SetWinEventHook(
        EVENT_OBJECT_REORDER,
        EVENT_OBJECT_REORDER,
        nullptr,
        clatashaTopmostWinEvent,
        0,
        0,
        hookFlags);

    if (g_topmostForegroundHook || g_topmostShowHook ||
        g_topmostReorderHook) {
        blog(LOG_INFO,
             "[Clatasha HUD] Event-driven topmost guard enabled");
    } else {
        blog(LOG_WARNING,
             "[Clatasha HUD] Event-driven topmost guard unavailable; watchdog fallback remains active");
    }
}

void stopTopmostEventHooks()
{
    if (g_topmostForegroundHook) {
        UnhookWinEvent(g_topmostForegroundHook);
        g_topmostForegroundHook = nullptr;
    }

    if (g_topmostShowHook) {
        UnhookWinEvent(g_topmostShowHook);
        g_topmostShowHook = nullptr;
    }

    if (g_topmostReorderHook) {
        UnhookWinEvent(g_topmostReorderHook);
        g_topmostReorderHook = nullptr;
    }

    g_topmostReassertQueued.store(false, std::memory_order_release);
}
#else
void startTopmostEventHooks()
{
}

void stopTopmostEventHooks()
{
}
#endif

bool showHudOverlaySlot(int index, const OverlayConfig &config, bool forceVisible = false)
{
    if (index < 0 || index >= kOverlayCount)
        return false;

    const bool shouldShow =
        (forceVisible || (config.enabled && modeHasHud(config.mode))) &&
        !config.url.trimmed().isEmpty();

    if (!shouldShow) {
        if (g_hudBrowserOverlays[index])
            g_hudBrowserOverlays[index]->deactivate();
        return true;
    }

    if (!g_hudBrowserOverlays[index])
        g_hudBrowserOverlays[index] = new BrowserHudOverlay(index);

    return g_hudBrowserOverlays[index]->applyConfig(config);
}

bool applyHudOverlays(const std::array<OverlayConfig, kOverlayCount> &configs)
{
    if (!g_browserHudOverlaysWantedVisible) {
        for (BrowserHudOverlay *overlay : g_hudBrowserOverlays) {
            if (overlay)
                overlay->deactivate();
        }
        return true;
    }

    bool browserAvailable = true;

    for (int i = 0; i < kOverlayCount; ++i) {
        if (!showHudOverlaySlot(i, configs[i], false))
            browserAvailable = false;
    }

    return browserAvailable;
}

void destroyHudOverlays()
{
    // Two-phase teardown: release OBS/CEF resources first, while all overlay
    // widgets are still alive, then destroy the Qt windows. This avoids
    // re-entering CEF browser shutdown from inside QWidget destruction.
    for (BrowserHudOverlay *overlay : g_hudBrowserOverlays) {
        if (overlay)
            overlay->prepareForShutdown();
    }

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

QString currentHudVersion()
{
    return QString::fromLatin1(kClatashaHudVersion);
}

QVersionNumber parsedVersion(QString value)
{
    value = value.trimmed();
    if (value.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        value.remove(0, 1);

    qsizetype suffixIndex = 0;
    return QVersionNumber::fromString(value, &suffixIndex);
}

bool isReleaseNewerThanInstalled(const QString &latest)
{
    const QVersionNumber latestVersion = parsedVersion(latest);
    const QVersionNumber installedVersion = parsedVersion(currentHudVersion());
    if (latestVersion.isNull() || installedVersion.isNull())
        return false;

    return QVersionNumber::compare(latestVersion, installedVersion) > 0;
}

void refreshSettingsUpdateUi()
{
    if (g_settingsVersionLabel)
        g_settingsVersionLabel->setText(
            QStringLiteral("v%1").arg(currentHudVersion()));

    const bool isCurrent =
        g_updateCheck.checkedSuccessfully && !g_updateCheck.updateAvailable;
    if (g_settingsCurrentCheckIcon) {
        g_settingsCurrentCheckIcon->setVisible(isCurrent);
        g_settingsCurrentCheckIcon->setToolTip(
            isCurrent
                ? QStringLiteral("Clatasha HUD is up to date.")
                : QString());
    }

    if (!g_settingsUpdateButton)
        return;

    g_settingsUpdateButton->setVisible(g_updateCheck.updateAvailable);
    if (!g_updateCheck.updateAvailable)
        return;

    const QString latest = g_updateCheck.latestVersion.trimmed();
    g_settingsUpdateButton->setText(QStringLiteral("●  Update available"));
    g_settingsUpdateButton->setToolTip(
        latest.isEmpty()
            ? QStringLiteral("A newer Clatasha HUD release is available.")
            : QStringLiteral("Clatasha HUD %1 is available. Click to open the release page.")
                  .arg(latest.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)
                           ? latest
                           : QStringLiteral("v%1").arg(latest)));
}

void loadCachedUpdateCheck()
{
    const QString path = settingsFilePath();
    if (path.isEmpty())
        return;

    QSettings settings(path, QSettings::IniFormat);

    // A failed request from an older OBS session must never suppress the next
    // startup check. Only a successful release lookup is persisted for 24 h.
    g_updateCheck.lastAttemptEpoch = 0;
    g_updateCheck.checkedSuccessfully =
        settings.value(QStringLiteral("update/checkedSuccessfully"), false).toBool();
    g_updateCheck.lastSuccessfulCheckEpoch =
        settings.value(QStringLiteral("update/lastSuccessfulCheckEpoch"), 0).toLongLong();

    // Migration from the first updater build, which stored its timestamp under
    // lastAttemptEpoch even when it had successfully completed.
    if (g_updateCheck.lastSuccessfulCheckEpoch <= 0 &&
        g_updateCheck.checkedSuccessfully) {
        g_updateCheck.lastSuccessfulCheckEpoch =
            settings.value(QStringLiteral("update/lastAttemptEpoch"), 0).toLongLong();
    }

    g_updateCheck.latestVersion =
        settings.value(QStringLiteral("update/latestVersion")).toString();
    g_updateCheck.releaseUrl = QUrl(
        settings.value(QStringLiteral("update/releaseUrl")).toString());
    g_updateCheck.updateAvailable =
        g_updateCheck.checkedSuccessfully &&
        isReleaseNewerThanInstalled(g_updateCheck.latestVersion);
}

void saveCachedUpdateCheck()
{
    const QString path = settingsFilePath();
    if (path.isEmpty())
        return;

    QSettings settings(path, QSettings::IniFormat);
    settings.remove(QStringLiteral("update/lastAttemptEpoch"));
    settings.setValue(
        QStringLiteral("update/lastSuccessfulCheckEpoch"),
        g_updateCheck.lastSuccessfulCheckEpoch);
    settings.setValue(
        QStringLiteral("update/latestVersion"),
        g_updateCheck.latestVersion);
    settings.setValue(
        QStringLiteral("update/releaseUrl"),
        g_updateCheck.releaseUrl.toString());
    settings.setValue(
        QStringLiteral("update/checkedSuccessfully"),
        g_updateCheck.checkedSuccessfully);
    settings.sync();
}

bool updateCheckCacheIsFresh()
{
    if (!g_updateCheck.checkedSuccessfully ||
        g_updateCheck.lastSuccessfulCheckEpoch <= 0) {
        return false;
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 age = now - g_updateCheck.lastSuccessfulCheckEpoch;
    return age >= 0 && age < kUpdateCheckIntervalSeconds;
}

bool updateRetryIsThrottled()
{
    if (g_updateCheck.lastAttemptEpoch <= 0)
        return false;

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 age = now - g_updateCheck.lastAttemptEpoch;
    return age >= 0 && age < kUpdateRetryIntervalSeconds;
}

#ifdef Q_OS_WIN
struct WinHttpUpdateResult {
    bool ok = false;
    DWORD errorCode = ERROR_SUCCESS;
    DWORD httpStatus = 0;
    QByteArray body;
};

WinHttpUpdateResult fetchLatestReleaseWithWinHttp()
{
    WinHttpUpdateResult result;

    const std::wstring userAgent =
        QStringLiteral("Clatasha-HUD/%1").arg(currentHudVersion()).toStdWString();

    HINTERNET session = WinHttpOpen(
        userAgent.c_str(),
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) {
        result.errorCode = GetLastError();
        return result;
    }

    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    auto cleanup = [&]() {
        if (request)
            WinHttpCloseHandle(request);
        if (connection)
            WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
    };

    // Keep the updater invisible to the OBS UI even if the network is down.
    WinHttpSetTimeouts(session, 3000, 3000, 5000, 5000);

    connection = WinHttpConnect(
        session,
        L"api.github.com",
        INTERNET_DEFAULT_HTTPS_PORT,
        0);
    if (!connection) {
        result.errorCode = GetLastError();
        cleanup();
        return result;
    }

    request = WinHttpOpenRequest(
        connection,
        L"GET",
        L"/repos/Clatasha/Clatasha-HUD/releases/latest",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!request) {
        result.errorCode = GetLastError();
        cleanup();
        return result;
    }

    const wchar_t *headers =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";

    if (!WinHttpSendRequest(
            request,
            headers,
            static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        result.errorCode = GetLastError();
        cleanup();
        return result;
    }

    DWORD statusSize = sizeof(result.httpStatus);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &result.httpStatus,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);

    if (result.httpStatus != 200) {
        cleanup();
        return result;
    }

    constexpr qsizetype kMaxReleaseResponseBytes = 1024 * 1024;
    std::array<char, 8192> buffer{};

    while (result.body.size() < kMaxReleaseResponseBytes) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            result.errorCode = GetLastError();
            cleanup();
            return result;
        }
        if (available == 0)
            break;

        const DWORD toRead =
            qMin<DWORD>(available, static_cast<DWORD>(buffer.size()));
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request, buffer.data(), toRead, &bytesRead)) {
            result.errorCode = GetLastError();
            cleanup();
            return result;
        }
        if (bytesRead == 0)
            break;

        result.body.append(buffer.data(), static_cast<qsizetype>(bytesRead));
    }

    cleanup();

    if (result.body.isEmpty())
        return result;

    result.ok = true;
    return result;
}

void shutdownUpdateChecker()
{
    if (g_updateWorker.joinable())
        g_updateWorker.join();

    delete g_updateCallbackContext;
    g_updateCallbackContext = nullptr;
    g_updateCheck.requestInFlight = false;
}
#else
void shutdownUpdateChecker()
{
    g_updateCheck.requestInFlight = false;
}
#endif

void checkForClatashaHudUpdate(bool force = false)
{
    if (g_frontendExiting || g_updateCheck.requestInFlight)
        return;

    if (!force && updateCheckCacheIsFresh()) {
        refreshSettingsUpdateUi();
        return;
    }

    if (!force && updateRetryIsThrottled()) {
        refreshSettingsUpdateUi();
        return;
    }

#ifndef Q_OS_WIN
    blog(LOG_INFO,
         "[Clatasha HUD] Update check skipped: native updater is Windows-only");
    refreshSettingsUpdateUi();
    return;
#else
    if (g_updateWorker.joinable())
        g_updateWorker.join();

    if (!g_updateCallbackContext)
        g_updateCallbackContext = new QObject();

    QObject *callbackContext = g_updateCallbackContext;
    g_updateCheck.requestInFlight = true;
    g_updateCheck.lastAttemptEpoch = QDateTime::currentSecsSinceEpoch();

    blog(LOG_INFO,
         "[Clatasha HUD] Checking GitHub Releases for updates using Windows WinHTTP");

    g_updateWorker = std::thread([callbackContext]() {
        WinHttpUpdateResult result = fetchLatestReleaseWithWinHttp();

        QMetaObject::invokeMethod(
            callbackContext,
            [result = std::move(result)]() mutable {
                g_updateCheck.requestInFlight = false;

                if (g_frontendExiting)
                    return;

                if (!result.ok) {
                    if (result.httpStatus != 0) {
                        blog(LOG_WARNING,
                             "[Clatasha HUD] Update check failed: GitHub HTTP %lu",
                             static_cast<unsigned long>(result.httpStatus));
                    } else {
                        blog(LOG_WARNING,
                             "[Clatasha HUD] Update check failed: WinHTTP error %lu",
                             static_cast<unsigned long>(result.errorCode));
                    }
                    refreshSettingsUpdateUi();
                    return;
                }

                QJsonParseError parseError{};
                const QJsonDocument document =
                    QJsonDocument::fromJson(result.body, &parseError);
                const QJsonObject object = document.object();
                const QString tagName =
                    object.value(QStringLiteral("tag_name")).toString();
                const QUrl releaseUrl(
                    object.value(QStringLiteral("html_url")).toString());

                if (parseError.error != QJsonParseError::NoError ||
                    !document.isObject() || tagName.trimmed().isEmpty()) {
                    blog(LOG_WARNING,
                         "[Clatasha HUD] Update check failed: GitHub release response was unreadable");
                    refreshSettingsUpdateUi();
                    return;
                }

                g_updateCheck.checkedSuccessfully = true;
                g_updateCheck.lastSuccessfulCheckEpoch =
                    QDateTime::currentSecsSinceEpoch();
                g_updateCheck.latestVersion = tagName.trimmed();
                g_updateCheck.releaseUrl =
                    releaseUrl.isValid()
                        ? releaseUrl
                        : kLatestReleaseFallbackUrl;
                g_updateCheck.updateAvailable =
                    isReleaseNewerThanInstalled(g_updateCheck.latestVersion);

                blog(LOG_INFO,
                     "[Clatasha HUD] Update check succeeded: installed v%s, latest %s%s",
                     kClatashaHudVersion,
                     g_updateCheck.latestVersion.toUtf8().constData(),
                     g_updateCheck.updateAvailable
                         ? " (update available)"
                         : " (current)");

                saveCachedUpdateCheck();
                refreshSettingsUpdateUi();
            },
            Qt::QueuedConnection);
    });
#endif
}

#ifdef Q_OS_WIN
bool isExternalForegroundCandidate(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd))
        return false;

    hwnd = GetAncestor(hwnd, GA_ROOT);
    if (!hwnd || !IsWindow(hwnd))
        return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid != 0 && pid != GetCurrentProcessId();
}

void trackExternalForegroundWindow()
{
    HWND hwnd = GetForegroundWindow();
    if (!isExternalForegroundCandidate(hwnd))
        return;

    g_lastExternalForegroundWindow = GetAncestor(hwnd, GA_ROOT);
}

QString externalWindowDescription(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return QStringLiteral("No game/window detected");

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    QString processName;
    HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &size))
            processName = QFileInfo(QString::fromWCharArray(path)).fileName();
        CloseHandle(process);
    }

    wchar_t title[512] = {};
    GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
    const QString windowTitle = QString::fromWCharArray(title).trimmed();

    if (processName.isEmpty())
        processName = QStringLiteral("PID %1").arg(pid);

    if (windowTitle.isEmpty())
        return processName;

    return QStringLiteral("%1 — %2").arg(processName, windowTitle);
}

bool isSafeAutomaticGameTarget(HWND hwnd)
{
    if (!isExternalForegroundCandidate(hwnd))
        return false;

    hwnd = GetAncestor(hwnd, GA_ROOT);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0)
        return false;

    HANDLE process =
        OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            pid);
    if (!process)
        return false;

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring processName;
    if (QueryFullProcessImageNameW(
            process,
            0,
            path,
            &size)) {
        processName.assign(path, size);
        const size_t separator =
            processName.find_last_of(L"\\/");
        if (separator != std::wstring::npos)
            processName.erase(0, separator + 1);
    }
    CloseHandle(process);

    static const wchar_t *excluded[] = {
        L"explorer.exe",
        L"SearchHost.exe",
        L"StartMenuExperienceHost.exe",
        L"ShellExperienceHost.exe",
        L"TextInputHost.exe",
        L"LockApp.exe",
        L"dwm.exe",
        L"obs64.exe",
    };

    for (const wchar_t *candidate : excluded) {
        if (_wcsicmp(
                processName.c_str(),
                candidate) == 0) {
            return false;
        }
    }

    return true;
}

void loadGameWindowSettings()
{
    const QString path = settingsFilePath();
    if (path.isEmpty())
        return;

    QSettings settings(path, QSettings::IniFormat);
    g_keepGameBorderlessApplied =
        settings.value(
                    QStringLiteral("gameWindow/keepBorderlessApplied"),
                    true)
            .toBool();
    g_displayCaptureSafeModeEnabled =
        settings.value(
                    QStringLiteral("gameWindow/displayCaptureSafeMode"),
                    false)
            .toBool();
}

void saveGameWindowSettings()
{
    const QString path = settingsFilePath();
    if (path.isEmpty())
        return;

    QSettings settings(path, QSettings::IniFormat);
    settings.setValue(
        QStringLiteral("gameWindow/keepBorderlessApplied"),
        g_keepGameBorderlessApplied);
    settings.setValue(
        QStringLiteral("gameWindow/displayCaptureSafeMode"),
        g_displayCaptureSafeModeEnabled);
    settings.sync();
}

bool taskbarAutoHideEnabled()
{
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    const UINT state =
        static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &data));
    return (state & ABS_AUTOHIDE) != 0;
}

bool setTaskbarAutoHideEnabled(bool enabled)
{
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar)
        return false;

    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = taskbar;

    const UINT current =
        static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &data));
    UINT desired = current;
    if (enabled)
        desired |= ABS_AUTOHIDE;
    else
        desired &= ~ABS_AUTOHIDE;

    if (desired == current)
        return true;

    data.lParam = static_cast<LPARAM>(desired);
    SHAppBarMessage(ABM_SETSTATE, &data);
    return taskbarAutoHideEnabled() == enabled;
}

void enableTemporaryTaskbarAutoHide()
{
    if (taskbarAutoHideEnabled()) {
        g_taskbarAutoHideChangedByClatasha = false;
        return;
    }

    if (setTaskbarAutoHideEnabled(true)) {
        g_taskbarAutoHideChangedByClatasha = true;
        blog(LOG_INFO,
             "[Clatasha HUD] Temporarily enabled Windows taskbar auto-hide");
    } else {
        g_taskbarAutoHideChangedByClatasha = false;
        blog(LOG_WARNING,
             "[Clatasha HUD] Could not enable Windows taskbar auto-hide");
    }
}

void restoreTemporaryTaskbarAutoHide()
{
    if (!g_taskbarAutoHideChangedByClatasha)
        return;

    if (!setTaskbarAutoHideEnabled(false)) {
        blog(LOG_WARNING,
             "[Clatasha HUD] Could not restore Windows taskbar auto-hide setting");
        return;
    }

    g_taskbarAutoHideChangedByClatasha = false;
    blog(LOG_INFO,
         "[Clatasha HUD] Restored Windows taskbar auto-hide setting");
}

bool setBorderlessWindowGeometry(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return false;

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor)
        return false;

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return false;

    const RECT &area = monitorInfo.rcMonitor;
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;

    // Do not make the client region an exact physical-monitor match. Modern
    // Windows can promote an exact borderless fullscreen window into
    // DirectFlip / Independent Flip, allowing the game to bypass normal DWM
    // composition and cover desktop HUD windows much like exclusive fullscreen.
    //
    // A one-pixel composition guard along the bottom is visually negligible but
    // keeps this as a genuinely windowed/composited presentation mode.
    constexpr int kCompositionGuardPx = 1;
    const int compositedHeight = qMax(1, height - kCompositionGuardPx);

    // The taskbar is handled explicitly while forced-borderless mode is
    // active. Keep the game itself out of the topmost band so Clatasha's HUD
    // remains the only overlay that needs topmost recovery.
    return SetWindowPos(
               hwnd,
               HWND_NOTOPMOST,
               area.left,
               area.top,
               width,
               compositedHeight,
               SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                   SWP_SHOWWINDOW) != FALSE;
}

bool applyBorderlessStyle(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return false;

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
               WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME);
    style |= WS_POPUP;

    exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                 WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR oldStyle = SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    if (oldStyle == 0 && GetLastError() != ERROR_SUCCESS)
        return false;

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR oldExStyle =
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
    if (oldExStyle == 0 && GetLastError() != ERROR_SUCCESS)
        return false;

    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);

    return setBorderlessWindowGeometry(hwnd);
}

bool restoreGameBorderlessWindow()
{
    if (!g_gameBorderless.active) {
        restoreTemporaryTaskbarAutoHide();
        return true;
    }

    restoreTemporaryTaskbarAutoHide();

    HWND hwnd = g_gameBorderless.window;
    const GameBorderlessState original = g_gameBorderless;
    g_gameBorderless = {};

    if (!hwnd || !IsWindow(hwnd))
        return true;

    SetWindowLongPtrW(hwnd, GWL_STYLE, original.style);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, original.exStyle);

    const int width = original.rect.right - original.rect.left;
    const int height = original.rect.bottom - original.rect.top;
    const HWND zOrder =
        (original.exStyle & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_NOTOPMOST;

    SetWindowPos(
        hwnd,
        zOrder,
        original.rect.left,
        original.rect.top,
        width,
        height,
        SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
            SWP_SHOWWINDOW);

    WINDOWPLACEMENT placement = original.placement;
    placement.length = sizeof(WINDOWPLACEMENT);
    SetWindowPlacement(hwnd, &placement);

    blog(LOG_INFO, "[Clatasha HUD] Restored original game window");
    return true;
}

bool forceGameBorderless(HWND hwnd)
{
    if (!isExternalForegroundCandidate(hwnd))
        return false;

    hwnd = GetAncestor(hwnd, GA_ROOT);

    if (g_gameBorderless.active && g_gameBorderless.window == hwnd)
        return applyBorderlessStyle(hwnd);

    if (g_gameBorderless.active)
        restoreGameBorderlessWindow();

    GameBorderlessState state;
    state.window = hwnd;
    state.style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    state.exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    state.placement.length = sizeof(WINDOWPLACEMENT);

    if (!GetWindowRect(hwnd, &state.rect) ||
        !GetWindowPlacement(hwnd, &state.placement)) {
        return false;
    }

    if (!applyBorderlessStyle(hwnd)) {
        // Best effort rollback if Windows rejected only part of the change.
        SetWindowLongPtrW(hwnd, GWL_STYLE, state.style);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, state.exStyle);
        SetWindowPos(
            hwnd,
            (state.exStyle & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_NOTOPMOST,
            state.rect.left,
            state.rect.top,
            state.rect.right - state.rect.left,
            state.rect.bottom - state.rect.top,
            SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                SWP_SHOWWINDOW);
        return false;
    }

    state.active = true;
    g_gameBorderless = state;
    g_lastExternalForegroundWindow = hwnd;
    enableTemporaryTaskbarAutoHide();

    blog(LOG_INFO,
         "[Clatasha HUD] Forced borderless fullscreen on %s",
         externalWindowDescription(hwnd).toUtf8().constData());
    return true;
}

bool forceLastExternalGameBorderless()
{
    trackExternalForegroundWindow();
    return forceGameBorderless(g_lastExternalForegroundWindow);
}

void maintainGameBorderless()
{
    if (!g_keepGameBorderlessApplied || !g_gameBorderless.active)
        return;

    if (!g_gameBorderless.window || !IsWindow(g_gameBorderless.window)) {
        restoreTemporaryTaskbarAutoHide();
        g_gameBorderless = {};
        return;
    }

    applyBorderlessStyle(g_gameBorderless.window);
}

void toggleGameBorderless()
{
    if (g_gameBorderless.active) {
        restoreGameBorderlessWindow();
        return;
    }

    if (!forceLastExternalGameBorderless()) {
        blog(LOG_WARNING,
             "[Clatasha HUD] No external foreground game/window available for borderless mode");
    }
}
#else
void loadGameWindowSettings() {}
void saveGameWindowSettings() {}
void trackExternalForegroundWindow() {}
void maintainGameBorderless() {}
void toggleGameBorderless() {}
#endif

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
        config.opacityPercent =
            qBound(10,
                   settings.value(prefix + QStringLiteral("opacityPercent"), 100).toInt(),
                   100);

        // Migration: older builds used one width/height pair for both the
        // browser viewport and displayed size.
        const int legacyWidth =
            qBound(100, settings.value(prefix + QStringLiteral("width"), 600).toInt(), 3840);
        const int legacyHeight =
            qBound(80, settings.value(prefix + QStringLiteral("height"), 300).toInt(), 2160);

        config.sourceWidth =
            qBound(100,
                   settings.value(prefix + QStringLiteral("sourceWidth"), legacyWidth).toInt(),
                   3840);
        config.sourceHeight =
            qBound(80,
                   settings.value(prefix + QStringLiteral("sourceHeight"), legacyHeight).toInt(),
                   2160);

        config.hudX = qBound(0, settings.value(prefix + QStringLiteral("hudX"), 80).toInt(),
                             kCanvasWidth - 20);
        config.hudY = qBound(0, settings.value(prefix + QStringLiteral("hudY"), 80).toInt(),
                             kCanvasHeight - 20);
        config.hudWidth =
            qBound(100,
                   settings.value(prefix + QStringLiteral("hudWidth"), legacyWidth).toInt(),
                   kCanvasWidth);
        config.hudHeight =
            qBound(80,
                   settings.value(prefix + QStringLiteral("hudHeight"), legacyHeight).toInt(),
                   kCanvasHeight);
        config.hudLockRatio =
            settings.value(prefix + QStringLiteral("hudLockRatio"), true).toBool();

        config.videoX = qBound(0, settings.value(prefix + QStringLiteral("videoX"), 660).toInt(),
                               kCanvasWidth - 20);
        config.videoY = qBound(0, settings.value(prefix + QStringLiteral("videoY"), 40).toInt(),
                               kCanvasHeight - 20);
        config.videoWidth =
            qBound(100,
                   settings.value(prefix + QStringLiteral("videoWidth"), legacyWidth).toInt(),
                   kCanvasWidth);
        config.videoHeight =
            qBound(80,
                   settings.value(prefix + QStringLiteral("videoHeight"), legacyHeight).toInt(),
                   kCanvasHeight);
        config.videoLockRatio =
            settings.value(prefix + QStringLiteral("videoLockRatio"), true).toBool();
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
        settings.setValue(prefix + QStringLiteral("opacityPercent"), config.opacityPercent);

        settings.setValue(prefix + QStringLiteral("sourceWidth"), config.sourceWidth);
        settings.setValue(prefix + QStringLiteral("sourceHeight"), config.sourceHeight);
        // Keep the old keys as source-resolution aliases for downgrade/migration safety.
        settings.setValue(prefix + QStringLiteral("width"), config.sourceWidth);
        settings.setValue(prefix + QStringLiteral("height"), config.sourceHeight);

        settings.setValue(prefix + QStringLiteral("hudX"), config.hudX);
        settings.setValue(prefix + QStringLiteral("hudY"), config.hudY);
        settings.setValue(prefix + QStringLiteral("hudWidth"), config.hudWidth);
        settings.setValue(prefix + QStringLiteral("hudHeight"), config.hudHeight);
        settings.setValue(prefix + QStringLiteral("hudLockRatio"), config.hudLockRatio);

        settings.setValue(prefix + QStringLiteral("videoX"), config.videoX);
        settings.setValue(prefix + QStringLiteral("videoY"), config.videoY);
        settings.setValue(prefix + QStringLiteral("videoWidth"), config.videoWidth);
        settings.setValue(prefix + QStringLiteral("videoHeight"), config.videoHeight);
        settings.setValue(prefix + QStringLiteral("videoLockRatio"), config.videoLockRatio);
    }

    settings.sync();
}

QString overlaySizeText(const OverlayConfig &config)
{
    return QStringLiteral("HUD %1 × %2   ·   VIDEO %3 × %4")
        .arg(config.hudWidth)
        .arg(config.hudHeight)
        .arg(config.videoWidth)
        .arg(config.videoHeight);
}

bool modeHasVideo(OverlayMode mode)
{
    return mode == OverlayMode::Video || mode == OverlayMode::Both;
}

bool modeHasHud(OverlayMode mode)
{
    return mode == OverlayMode::Hud || mode == OverlayMode::Both;
}

bool applyVideoOpacityFilter(obs_source_t *source, int opacityPercent)
{
    if (!source)
        return false;

    constexpr const char *kOpacityFilterName = "Clatasha Overlay Opacity";
    const double opacity = qBound(10, opacityPercent, 100) / 100.0;

    obs_data_t *filterSettings = obs_data_create();
    obs_data_set_double(filterSettings, "opacity", opacity);

    obs_source_t *filter =
        obs_source_get_filter_by_name(source, kOpacityFilterName);

    if (filter) {
        obs_source_update(filter, filterSettings);
        obs_source_release(filter);
        obs_data_release(filterSettings);
        return true;
    }

    filter = obs_source_create(
        "color_filter",
        kOpacityFilterName,
        filterSettings,
        nullptr);
    obs_data_release(filterSettings);

    if (!filter) {
        blog(LOG_WARNING,
             "[Clatasha HUD] Could not create opacity filter for browser overlay");
        return false;
    }

    obs_source_filter_add(source, filter);
    obs_source_release(filter);
    return true;
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
        applyBrowserInputSettings(settings, config.url);
        obs_data_set_int(settings, "width", config.sourceWidth);
        obs_data_set_int(settings, "height", config.sourceHeight);
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

        if (!applyVideoOpacityFilter(source, config.opacityPercent))
            browserAvailable = false;

        if (scene && !item)
            item = obs_scene_add(scene, source);

        if (item) {
            struct vec2 pos;
            pos.x = static_cast<float>(config.videoX);
            pos.y = static_cast<float>(config.videoY);
            obs_sceneitem_set_pos(item, &pos);
            obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);

            struct vec2 scale;
            scale.x = static_cast<float>(config.videoWidth) /
                      static_cast<float>(qMax(1, config.sourceWidth));
            scale.y = static_cast<float>(config.videoHeight) /
                      static_cast<float>(qMax(1, config.sourceHeight));
            obs_sceneitem_set_scale(item, &scale);
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

    void setLockRatio(bool enabled, double ratio)
    {
        lockRatio_ = enabled;
        if (ratio > 0.01)
            aspectRatio_ = ratio;
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

        if (activeHandle_ == Handle::Move) {
            next.translate(dx, dy);
        } else if (lockRatio_) {
            next = lockedResize(dx, dy);
        } else {
            switch (activeHandle_) {
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

            enforceMinimumSize(next);
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

    void normalizeLockedSize(int &width, int &height, bool widthPrimary) const
    {
        const double ratio = qMax(0.01, aspectRatio_);

        if (widthPrimary) {
            width = qMax(100, width);
            height = qMax(80, qRound(width / ratio));
            if (height == 80)
                width = qMax(100, qRound(height * ratio));
        } else {
            height = qMax(80, height);
            width = qMax(100, qRound(height * ratio));
            if (width == 100)
                height = qMax(80, qRound(width / ratio));
        }

        const double scale =
            qMin(1.0,
                 qMin(kCanvasWidth / static_cast<double>(qMax(1, width)),
                      kCanvasHeight / static_cast<double>(qMax(1, height))));
        if (scale < 1.0) {
            width = qMax(100, qRound(width * scale));
            height = qMax(80, qRound(width / ratio));
        }
    }

    QRect lockedResize(int dx, int dy) const
    {
        const QRect s = startRect_;
        int width = s.width();
        int height = s.height();
        bool widthPrimary = true;

        const double relX = std::abs(dx) / static_cast<double>(qMax(1, s.width()));
        const double relY = std::abs(dy) / static_cast<double>(qMax(1, s.height()));

        switch (activeHandle_) {
        case Handle::Left:
            width = s.width() - dx;
            widthPrimary = true;
            break;
        case Handle::Right:
            width = s.width() + dx;
            widthPrimary = true;
            break;
        case Handle::Top:
            height = s.height() - dy;
            widthPrimary = false;
            break;
        case Handle::Bottom:
            height = s.height() + dy;
            widthPrimary = false;
            break;
        case Handle::TopLeft:
            width = s.width() - dx;
            height = s.height() - dy;
            widthPrimary = relX >= relY;
            break;
        case Handle::TopRight:
            width = s.width() + dx;
            height = s.height() - dy;
            widthPrimary = relX >= relY;
            break;
        case Handle::BottomLeft:
            width = s.width() - dx;
            height = s.height() + dy;
            widthPrimary = relX >= relY;
            break;
        case Handle::BottomRight:
            width = s.width() + dx;
            height = s.height() + dy;
            widthPrimary = relX >= relY;
            break;
        default:
            break;
        }

        normalizeLockedSize(width, height, widthPrimary);

        switch (activeHandle_) {
        case Handle::Left:
            return QRect(s.right() - width + 1, s.center().y() - height / 2, width, height);
        case Handle::Right:
            return QRect(s.left(), s.center().y() - height / 2, width, height);
        case Handle::Top:
            return QRect(s.center().x() - width / 2, s.bottom() - height + 1, width, height);
        case Handle::Bottom:
            return QRect(s.center().x() - width / 2, s.top(), width, height);
        case Handle::TopLeft:
            return QRect(s.right() - width + 1, s.bottom() - height + 1, width, height);
        case Handle::TopRight:
            return QRect(s.left(), s.bottom() - height + 1, width, height);
        case Handle::BottomLeft:
            return QRect(s.right() - width + 1, s.top(), width, height);
        case Handle::BottomRight:
            return QRect(s.left(), s.top(), width, height);
        default:
            return s;
        }
    }

    void enforceMinimumSize(QRect &rect) const
    {
        if (rect.width() < 100) {
            if (activeHandle_ == Handle::Left || activeHandle_ == Handle::TopLeft ||
                activeHandle_ == Handle::BottomLeft)
                rect.setLeft(rect.right() - 99);
            else
                rect.setRight(rect.left() + 99);
        }

        if (rect.height() < 80) {
            if (activeHandle_ == Handle::Top || activeHandle_ == Handle::TopLeft ||
                activeHandle_ == Handle::TopRight)
                rect.setTop(rect.bottom() - 79);
            else
                rect.setBottom(rect.top() + 79);
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
    bool lockRatio_ = true;
    double aspectRatio_ = 2.0;
    std::function<void(const QRect &)> changedCallback_;
};

void showOverlayPreview(QWidget *parent, int overlayIndex, OverlayConfig &config)
{
    const OverlayConfig originalConfig = config;

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Clatasha Overlay Placement"));
    dialog.setModal(true);
    dialog.resize(790, 570);

    auto *previewNote = new QLabel(
        QStringLiteral(
            "Placement now scales the finished overlay instead of changing the browser viewport. "
            "Lock Ratio keeps the current proportions while you drag a resize handle."),
        &dialog);
    previewNote->setWordWrap(true);
    previewNote->setStyleSheet(QStringLiteral("color:#8cc5ff; padding:2px 4px 6px 4px;"));

    auto *sourceInfo = new QLabel(
        QStringLiteral("Source canvas: %1 × %2")
            .arg(config.sourceWidth)
            .arg(config.sourceHeight),
        &dialog);
    sourceInfo->setStyleSheet(QStringLiteral("color:#8996a3; padding:0 4px 4px 4px;"));

    auto *modeTabs = new QTabWidget(&dialog);
    auto *hudPage = new QWidget(modeTabs);
    auto *videoPage = new QWidget(modeTabs);

    auto showLiveHudPreview = [&](const QRect &rect) {
        OverlayConfig previewConfig = config;
        previewConfig.enabled = true;
        previewConfig.mode = OverlayMode::Hud;
        previewConfig.hudX = rect.x();
        previewConfig.hudY = rect.y();
        previewConfig.hudWidth = rect.width();
        previewConfig.hudHeight = rect.height();
        showHudOverlaySlot(overlayIndex, previewConfig, true);
    };

    struct PageControls {
        QSpinBox *x = nullptr;
        QSpinBox *y = nullptr;
        QSpinBox *w = nullptr;
        QSpinBox *h = nullptr;
        QCheckBox *lock = nullptr;
    };

    auto buildPage = [&](QWidget *page, bool video) {
        PageControls controlsData;
        auto *canvas = new OverlayPreviewCanvas(page);
        canvas->setLabel(config.name);

        const QRect initial(video ? config.videoX : config.hudX,
                            video ? config.videoY : config.hudY,
                            video ? config.videoWidth : config.hudWidth,
                            video ? config.videoHeight : config.hudHeight);
        canvas->setGeometryData(initial.x(), initial.y(), initial.width(), initial.height());

        controlsData.x = new QSpinBox(page);
        controlsData.y = new QSpinBox(page);
        controlsData.w = new QSpinBox(page);
        controlsData.h = new QSpinBox(page);
        controlsData.lock = new QCheckBox(QStringLiteral("Lock Ratio"), page);

        controlsData.x->setRange(0, kCanvasWidth - 20);
        controlsData.y->setRange(0, kCanvasHeight - 20);
        controlsData.w->setRange(100, kCanvasWidth);
        controlsData.h->setRange(80, kCanvasHeight);

        controlsData.x->setValue(initial.x());
        controlsData.y->setValue(initial.y());
        controlsData.w->setValue(initial.width());
        controlsData.h->setValue(initial.height());
        controlsData.lock->setChecked(video ? config.videoLockRatio : config.hudLockRatio);

        auto aspect = std::make_shared<double>(
            initial.height() > 0
                ? initial.width() / static_cast<double>(initial.height())
                : 2.0);
        canvas->setLockRatio(controlsData.lock->isChecked(), *aspect);

        auto syncCanvas = [=, &showLiveHudPreview]() {
            canvas->setGeometryData(
                controlsData.x->value(),
                controlsData.y->value(),
                controlsData.w->value(),
                controlsData.h->value());
            if (!video)
                showLiveHudPreview(canvas->geometryData());
        };

        QObject::connect(controlsData.x, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
                         [=, &showLiveHudPreview](int) { syncCanvas(); });
        QObject::connect(controlsData.y, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
                         [=, &showLiveHudPreview](int) { syncCanvas(); });

        QObject::connect(controlsData.w, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
                         [=, &showLiveHudPreview](int value) {
                             if (controlsData.lock->isChecked()) {
                                 const int height =
                                     qBound(80, qRound(value / qMax(0.01, *aspect)), kCanvasHeight);
                                 QSignalBlocker blocker(controlsData.h);
                                 controlsData.h->setValue(height);
                             }
                             syncCanvas();
                         });

        QObject::connect(controlsData.h, QOverload<int>::of(&QSpinBox::valueChanged), &dialog,
                         [=, &showLiveHudPreview](int value) {
                             if (controlsData.lock->isChecked()) {
                                 const int width =
                                     qBound(100, qRound(value * qMax(0.01, *aspect)), kCanvasWidth);
                                 QSignalBlocker blocker(controlsData.w);
                                 controlsData.w->setValue(width);
                             }
                             syncCanvas();
                         });

        QObject::connect(controlsData.lock, &QCheckBox::toggled, &dialog,
                         [=](bool checked) {
                             if (checked && controlsData.h->value() > 0)
                                 *aspect = controlsData.w->value() /
                                           static_cast<double>(controlsData.h->value());
                             canvas->setLockRatio(checked, *aspect);
                         });

        canvas->setChangedCallback([=, &showLiveHudPreview](const QRect &rect) {
            QSignalBlocker bx(controlsData.x);
            QSignalBlocker by(controlsData.y);
            QSignalBlocker bw(controlsData.w);
            QSignalBlocker bh(controlsData.h);
            controlsData.x->setValue(rect.x());
            controlsData.y->setValue(rect.y());
            controlsData.w->setValue(rect.width());
            controlsData.h->setValue(rect.height());

            if (!video)
                showLiveHudPreview(rect);
        });

        auto *centerButton = new QPushButton(QStringLiteral("Center"), page);
        auto *resetButton = new QPushButton(QStringLiteral("Reset 1:1"), page);

        QObject::connect(centerButton, &QPushButton::clicked, &dialog, [=]() {
            const int x = (kCanvasWidth - controlsData.w->value()) / 2;
            const int y = (kCanvasHeight - controlsData.h->value()) / 2;
            controlsData.x->setValue(qMax(0, x));
            controlsData.y->setValue(qMax(0, y));
        });

        QObject::connect(resetButton, &QPushButton::clicked, &dialog, [=]() {
            const int width = qBound(100, config.sourceWidth, kCanvasWidth);
            const int height = qBound(80, config.sourceHeight, kCanvasHeight);
            {
                QSignalBlocker bw(controlsData.w);
                QSignalBlocker bh(controlsData.h);
                controlsData.w->setValue(width);
                controlsData.h->setValue(height);
            }
            *aspect = width / static_cast<double>(qMax(1, height));
            canvas->setLockRatio(controlsData.lock->isChecked(), *aspect);
            syncCanvas();
        });

        auto *geometryRow = new QHBoxLayout();
        geometryRow->addWidget(new QLabel(QStringLiteral("X"), page));
        geometryRow->addWidget(controlsData.x);
        geometryRow->addWidget(new QLabel(QStringLiteral("Y"), page));
        geometryRow->addWidget(controlsData.y);
        geometryRow->addSpacing(8);
        geometryRow->addWidget(new QLabel(QStringLiteral("W"), page));
        geometryRow->addWidget(controlsData.w);
        geometryRow->addWidget(new QLabel(QStringLiteral("H"), page));
        geometryRow->addWidget(controlsData.h);
        geometryRow->addStretch();
        geometryRow->addWidget(centerButton);
        geometryRow->addWidget(resetButton);

        auto *ratioRow = new QHBoxLayout();
        ratioRow->addWidget(controlsData.lock);
        ratioRow->addWidget(new QLabel(
            QStringLiteral("Scales the overlay without changing its browser/source canvas."),
            page));
        ratioRow->addStretch();

        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(12, 12, 12, 12);
        layout->addWidget(canvas, 1);
        layout->addLayout(ratioRow);
        layout->addLayout(geometryRow);

        return controlsData;
    };

    const PageControls hudControls = buildPage(hudPage, false);
    const PageControls videoControls = buildPage(videoPage, true);

    modeTabs->addTab(hudPage, QStringLiteral("HUD Placement"));
    modeTabs->addTab(videoPage, QStringLiteral("VIDEO Placement"));

    if (config.mode == OverlayMode::Video)
        modeTabs->setCurrentWidget(videoPage);

    if (!config.url.trimmed().isEmpty()) {
        showLiveHudPreview(
            QRect(config.hudX, config.hudY, config.hudWidth, config.hudHeight));
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);

    auto *mainLayout = new QVBoxLayout(&dialog);
    mainLayout->addWidget(previewNote);
    mainLayout->addWidget(sourceInfo);
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
        "QPushButton:hover { background:#2a323b; }"
        "QCheckBox { spacing:7px; }"));

    if (dialog.exec() == QDialog::Accepted) {
        config.hudX = hudControls.x->value();
        config.hudY = hudControls.y->value();
        config.hudWidth = hudControls.w->value();
        config.hudHeight = hudControls.h->value();
        config.hudLockRatio = hudControls.lock->isChecked();

        config.videoX = videoControls.x->value();
        config.videoY = videoControls.y->value();
        config.videoWidth = videoControls.w->value();
        config.videoHeight = videoControls.h->value();
        config.videoLockRatio = videoControls.lock->isChecked();

        showHudOverlaySlot(overlayIndex, config, false);
    } else {
        config = originalConfig;
        showHudOverlaySlot(overlayIndex, originalConfig, false);
    }
}

struct OverlayRow {
    QCheckBox *enabled = nullptr;
    QLineEdit *name = nullptr;
    QLineEdit *url = nullptr;
    QPushButton *browse = nullptr;
    QComboBox *mode = nullptr;
    QSlider *opacity = nullptr;
    QLabel *opacityValue = nullptr;
    QLabel *sizeLabel = nullptr;
    QPushButton *preview = nullptr;
};

bool configureGameCaptureForHudExclusion(
    void *,
    obs_source_t *source)
{
    if (!source)
        return true;

    const char *id =
        obs_source_get_unversioned_id(source);
    if (!id ||
        std::strcmp(id, "game_capture") != 0) {
        return true;
    }

    obs_data_t *settings =
        obs_source_get_settings(source);
    if (!settings)
        return true;

    const bool captureOverlays =
        obs_data_get_bool(
            settings,
            "capture_overlays");

    if (captureOverlays) {
        obs_data_set_bool(
            settings,
            "capture_overlays",
            false);
        obs_source_update(
            source,
            settings);

        blog(
            LOG_INFO,
            "[Clatasha HUD] Game Capture '%s': disabled third-party overlay capture for local-only HUD",
            obs_source_get_name(source));
    }

    obs_data_release(settings);
    return true;
}

void enforceGameCaptureHudExclusion()
{
    obs_enum_sources(
        configureGameCaptureForHudExclusion,
        nullptr);
}

#ifdef Q_OS_WIN
bool findActiveDisplayCapture(
    void *param,
    obs_source_t *source)
{
    if (!param || !source)
        return true;

    const char *id =
        obs_source_get_unversioned_id(source);
    if (!id ||
        std::strcmp(id, "monitor_capture") != 0) {
        return true;
    }

    if (obs_source_showing(source)) {
        *static_cast<bool *>(param) = true;
        return false;
    }

    return true;
}

bool activeDisplayCapturePresent()
{
    bool active = false;
    obs_enum_sources(
        findActiveDisplayCapture,
        &active);
    return active;
}

void updateDisplayCaptureSafeMode()
{
    if (!g_displayCaptureSafeModeEnabled) {
        if (g_displayCaptureSafeEvent)
            ResetEvent(g_displayCaptureSafeEvent);

        if (g_displayCaptureForcedBorderless) {
            restoreGameBorderlessWindow();
            g_displayCaptureForcedBorderless = false;
        }

        if (g_displayCaptureSafeActive) {
            g_displayCaptureSafeActive = false;
            blog(
                LOG_INFO,
                "[Clatasha HUD] Display Capture Safe Mode: disabled by user");
        }
        return;
    }

    const bool active =
        activeDisplayCapturePresent();

    if (g_displayCaptureSafeEvent) {
        if (active)
            SetEvent(g_displayCaptureSafeEvent);
        else
            ResetEvent(g_displayCaptureSafeEvent);
    }

    // Display Capture cannot hide HUD pixels that are injected into the game
    // framebuffer. In safe mode we use the separate capture-excluded HUD
    // window instead, so true fullscreen needs to be converted to Clatasha's
    // composited borderless mode for that window to remain visible locally.
    if (active) {
        if (!g_gameBorderless.active &&
            g_lastExternalForegroundWindow &&
            IsWindow(g_lastExternalForegroundWindow) &&
            isSafeAutomaticGameTarget(
                g_lastExternalForegroundWindow)) {
            if (forceGameBorderless(
                    g_lastExternalForegroundWindow)) {
                g_displayCaptureForcedBorderless = true;
                blog(
                    LOG_INFO,
                    "[Clatasha HUD] Display Capture Safe Mode switched game to borderless fullscreen");
            }
        }
    } else if (g_displayCaptureForcedBorderless) {
        restoreGameBorderlessWindow();
        g_displayCaptureForcedBorderless = false;
        blog(
            LOG_INFO,
            "[Clatasha HUD] Display Capture Safe Mode restored original game window mode");
    }

    if (active ==
        g_displayCaptureSafeActive) {
        return;
    }

    g_displayCaptureSafeActive = active;

    blog(
        LOG_INFO,
        "[Clatasha HUD] Display Capture Safe Mode: %s%s",
        active ? "active" : "inactive",
        active
            ? " (local HUD visible, injected drawing suppressed)"
            : "");
}
#else
void updateDisplayCaptureSafeMode()
{
}
#endif

} // namespace

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Native local-only HUD for OBS Studio";
}

static void ensure_hud()
{
    if (g_hud)
        return;

    g_hud = new ClatashaHudWindow();

    g_hudWatchdog = new QTimer(g_hud);
    g_hudWatchdog->setInterval(250);
    QObject::connect(g_hudWatchdog, &QTimer::timeout, []() {
        trackExternalForegroundWindow();
        maintainGameBorderless();

        static int captureExclusionTicks = 0;
        if (++captureExclusionTicks >= 8) {
            captureExclusionTicks = 0;
            enforceGameCaptureHudExclusion();
        }

        updateDisplayCaptureSafeMode();

        if (!g_hud)
            return;

        bool restored = false;
        if (g_hudWantedVisible && !g_hud->isVisible()) {
            g_hud->show();
            g_hud->positionHud();
            restored = true;
        }

#ifdef Q_OS_WIN
        if (g_hudWantedVisible) {
            const HWND hwnd = reinterpret_cast<HWND>(g_hud->winId());
            if (hwnd && IsIconic(hwnd)) {
                ShowWindow(hwnd, SW_RESTORE);
                restored = true;
            }
        }
#endif

        reassertClatashaTopmostWindows();

        if (restored)
            blog(LOG_INFO, "[Clatasha HUD] Restored main HUD visibility");
    });
    g_hudWatchdog->start();
    startTopmostEventHooks();
    reassertClatashaTopmostWindows();

    QObject::connect(g_hud, &QObject::destroyed, []() {
        g_hudWatchdog = nullptr;
    });
}

static void toggle_hud()
{
    ensure_hud();

    g_hudWantedVisible = !g_hudWantedVisible;

    if (!g_hudWantedVisible) {
        g_hud->hide();
        return;
    }

    g_hud->show();
    g_hud->positionHud();
    reassertClatashaTopmostWindows();
}

enum class HudHotkeyAction : int {
    ToggleHud = 0,
    ToggleBrowserHudOverlays,
    ToggleReplayBuffer,
    SaveReplay,
    ToggleRecording,
    PauseRecording,
    ToggleStreaming,
    ToggleGameBorderless,
    Count,
};

struct HudHotkeyDefinition {
    const char *name = nullptr;
    const char *description = nullptr;
    const char *label = nullptr;
    const char *group = nullptr;
    obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
};

constexpr int kHudHotkeyCount = static_cast<int>(HudHotkeyAction::Count);

static std::array<HudHotkeyDefinition, kHudHotkeyCount> g_hudHotkeys{{
    {"ClatashaHUD.ToggleHUD", "Clatasha HUD: Toggle HUD", "Toggle Clatasha HUD", "HUD"},
    {"ClatashaHUD.ToggleBrowserHudOverlays", "Clatasha HUD: Show/Hide Browser HUD Overlays", "Show/Hide Browser HUD Overlays", "HUD"},
    {"ClatashaHUD.ToggleReplayBuffer", "Clatasha HUD: Toggle Replay Buffer", "Toggle Replay Buffer", "Replay Buffer"},
    {"ClatashaHUD.SaveReplay", "Clatasha HUD: Save Replay", "Save Replay", "Replay Buffer"},
    {"ClatashaHUD.ToggleRecording", "Clatasha HUD: Toggle Recording", "Toggle Recording", "Recording"},
    {"ClatashaHUD.PauseRecording", "Clatasha HUD: Pause/Resume Recording", "Pause/Resume Recording", "Recording"},
    {"ClatashaHUD.ToggleStreaming", "Clatasha HUD: Toggle Streaming", "Toggle Streaming", "Streaming"},
    {"ClatashaHUD.ToggleGameBorderless", "Clatasha HUD: Toggle Game Borderless Fullscreen", "Toggle Game Borderless Fullscreen", "Game Window"},
}};

static QString hotkeyCombinationText(obs_key_combination_t combination)
{
    if (obs_key_combination_is_empty(combination))
        return QStringLiteral("Not set");

    dstr text = {};
    obs_key_combination_to_str(combination, &text);
    const QString result = QString::fromUtf8(text.array ? text.array : "");
    dstr_free(&text);
    return result.isEmpty() ? QStringLiteral("Not set") : result;
}

struct HotkeyBindingLookup {
    obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
    obs_key_combination_t combination{0, OBS_KEY_NONE};
    bool found = false;
};

static bool findHotkeyBinding(void *data, size_t, obs_hotkey_binding_t *binding)
{
    auto *lookup = static_cast<HotkeyBindingLookup *>(data);
    if (!lookup || obs_hotkey_binding_get_hotkey_id(binding) != lookup->id)
        return true;

    lookup->combination = obs_hotkey_binding_get_key_combination(binding);
    lookup->found = true;
    return false;
}

static obs_key_combination_t currentHotkeyBinding(obs_hotkey_id id)
{
    HotkeyBindingLookup lookup;
    lookup.id = id;
    obs_enum_hotkey_bindings(findHotkeyBinding, &lookup);
    return lookup.combination;
}

class HotkeyCaptureEdit final : public QLineEdit {
public:
    explicit HotkeyCaptureEdit(QWidget *parent = nullptr) : QLineEdit(parent)
    {
        setReadOnly(true);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
        setMinimumWidth(170);
        setPlaceholderText(QStringLiteral("Not set"));
    }

    void setCombination(obs_key_combination_t combination)
    {
        combination_ = combination;
        setText(hotkeyCombinationText(combination_));
    }

    obs_key_combination_t combination() const { return combination_; }

    void clearCombination()
    {
        combination_ = {0, OBS_KEY_NONE};
        setText(hotkeyCombinationText(combination_));
        if (changedCallback)
            changedCallback(combination_);
    }

    std::function<void(obs_key_combination_t)> changedCallback;

protected:
    void focusInEvent(QFocusEvent *event) override
    {
        QLineEdit::focusInEvent(event);
        setText(QStringLiteral("Press shortcut…"));
        selectAll();
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        setText(hotkeyCombinationText(combination_));
        QLineEdit::focusOutEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        QLineEdit::mousePressEvent(event);
        setFocus(Qt::MouseFocusReason);
        selectAll();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            clearFocus();
            event->accept();
            return;
        }

        if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
            clearCombination();
            clearFocus();
            event->accept();
            return;
        }

        if (event->key() == Qt::Key_Shift || event->key() == Qt::Key_Control ||
            event->key() == Qt::Key_Alt || event->key() == Qt::Key_Meta) {
            event->accept();
            return;
        }

        const int nativeKey = static_cast<int>(event->nativeVirtualKey());
        const obs_key_t key = nativeKey ? obs_key_from_virtual_key(nativeKey) : OBS_KEY_NONE;
        if (key == OBS_KEY_NONE) {
            setText(QStringLiteral("Unsupported key"));
            event->accept();
            return;
        }

        uint32_t modifiers = 0;
        const Qt::KeyboardModifiers qtModifiers = event->modifiers();
        if (qtModifiers.testFlag(Qt::ShiftModifier))
            modifiers |= INTERACT_SHIFT_KEY;
        if (qtModifiers.testFlag(Qt::ControlModifier))
            modifiers |= INTERACT_CONTROL_KEY;
        if (qtModifiers.testFlag(Qt::AltModifier))
            modifiers |= INTERACT_ALT_KEY;
        if (qtModifiers.testFlag(Qt::MetaModifier))
            modifiers |= INTERACT_COMMAND_KEY;

        combination_ = {modifiers, key};
        setText(hotkeyCombinationText(combination_));
        if (changedCallback)
            changedCallback(combination_);

        clearFocus();
        event->accept();
    }

private:
    obs_key_combination_t combination_{0, OBS_KEY_NONE};
};

static void toggle_browser_hud_overlays()
{
    g_browserHudOverlaysWantedVisible = !g_browserHudOverlaysWantedVisible;

    if (!g_browserHudOverlaysWantedVisible) {
        for (BrowserHudOverlay *overlay : g_hudBrowserOverlays) {
            if (overlay)
                overlay->deactivate();
        }
        blog(LOG_INFO, "[Clatasha HUD] Browser HUD overlays hidden");
        return;
    }

    applyHudOverlays(loadOverlayConfigs());
    blog(LOG_INFO, "[Clatasha HUD] Browser HUD overlays shown");
}

static void runHotkeyActionOnUi(HudHotkeyAction action)
{
    if (g_frontendExiting)
        return;

    QObject *target = QCoreApplication::instance();
    if (!target)
        return;

    QMetaObject::invokeMethod(
        target,
        [action]() {
            if (g_frontendExiting)
                return;

            switch (action) {
            case HudHotkeyAction::ToggleHud:
                toggle_hud();
                break;

            case HudHotkeyAction::ToggleBrowserHudOverlays:
                toggle_browser_hud_overlays();
                break;

            case HudHotkeyAction::ToggleReplayBuffer:
                if (obs_frontend_replay_buffer_active())
                    obs_frontend_replay_buffer_stop();
                else
                    obs_frontend_replay_buffer_start();
                break;

            case HudHotkeyAction::SaveReplay:
                if (obs_frontend_replay_buffer_active())
                    obs_frontend_replay_buffer_save();
                break;

            case HudHotkeyAction::ToggleRecording:
                if (obs_frontend_recording_active())
                    obs_frontend_recording_stop();
                else
                    obs_frontend_recording_start();
                break;

            case HudHotkeyAction::PauseRecording:
                if (obs_frontend_recording_active())
                    obs_frontend_recording_pause(!obs_frontend_recording_paused());
                break;

            case HudHotkeyAction::ToggleStreaming:
                if (obs_frontend_streaming_active())
                    obs_frontend_streaming_stop();
                else
                    obs_frontend_streaming_start();
                break;

            case HudHotkeyAction::ToggleGameBorderless:
                toggleGameBorderless();
                break;

            case HudHotkeyAction::Count:
                break;
            }
        },
        Qt::QueuedConnection);
}

static void hudHotkeyCallback(void *data,
                              obs_hotkey_id,
                              obs_hotkey_t *,
                              bool pressed)
{
    if (!pressed)
        return;

    const auto action =
        static_cast<HudHotkeyAction>(reinterpret_cast<intptr_t>(data));
    runHotkeyActionOnUi(action);
}

static void saveHudHotkeys()
{
    const QString path = settingsFilePath();
    if (path.isEmpty())
        return;

    QSettings settings(path, QSettings::IniFormat);
    for (int i = 0; i < kHudHotkeyCount; ++i) {
        const obs_key_combination_t combination =
            currentHotkeyBinding(g_hudHotkeys[i].id);
        const QString prefix = QStringLiteral("hotkeys/%1/").arg(i);
        settings.setValue(prefix + QStringLiteral("key"),
                          static_cast<int>(combination.key));
        settings.setValue(prefix + QStringLiteral("modifiers"),
                          static_cast<qulonglong>(combination.modifiers));
    }
    settings.sync();
}

static void loadHudHotkeys()
{
    const QString path = settingsFilePath();
    if (path.isEmpty())
        return;

    QSettings settings(path, QSettings::IniFormat);
    for (int i = 0; i < kHudHotkeyCount; ++i) {
        const QString prefix = QStringLiteral("hotkeys/%1/").arg(i);
        if (!settings.contains(prefix + QStringLiteral("key")))
            continue;

        obs_key_combination_t combination{};
        combination.key = static_cast<obs_key_t>(
            settings.value(prefix + QStringLiteral("key"),
                           static_cast<int>(OBS_KEY_NONE)).toInt());
        combination.modifiers = static_cast<uint32_t>(
            settings.value(prefix + QStringLiteral("modifiers"), 0).toULongLong());

        if (obs_key_combination_is_empty(combination))
            obs_hotkey_load_bindings(g_hudHotkeys[i].id, nullptr, 0);
        else
            obs_hotkey_load_bindings(g_hudHotkeys[i].id, &combination, 1);
    }
}

static void registerHudHotkeys()
{
    for (int i = 0; i < kHudHotkeyCount; ++i) {
        g_hudHotkeys[i].id = obs_hotkey_register_frontend(
            g_hudHotkeys[i].name,
            g_hudHotkeys[i].description,
            hudHotkeyCallback,
            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }

    loadHudHotkeys();
}

static void unregisterHudHotkeys()
{
    saveHudHotkeys();

    for (HudHotkeyDefinition &hotkey : g_hudHotkeys) {
        if (hotkey.id != OBS_INVALID_HOTKEY_ID) {
            obs_hotkey_unregister(hotkey.id);
            hotkey.id = OBS_INVALID_HOTKEY_ID;
        }
    }
}

static void show_settings()
{
    ensure_hud();

    const int originalOpacity = g_hud->opacityPercent();
    const QString originalLocation = g_hud->location();
    const auto originalOverlayConfigs = loadOverlayConfigs();
    auto overlayConfigs = originalOverlayConfigs;

    QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Clatasha HUD Settings"));
    dialog.setWindowIcon(QIcon(QStringLiteral(":/clatasha/icons/settings.svg")));
    dialog.setModal(true);
    dialog.setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    dialog.setWindowFlag(Qt::WindowMinimizeButtonHint, true);
    dialog.setSizeGripEnabled(true);
    dialog.setMinimumSize(720, 520);

    QScreen *settingsScreen = parent ? parent->screen() : QGuiApplication::primaryScreen();
    const QRect availableGeometry =
        settingsScreen ? settingsScreen->availableGeometry() : QRect(0, 0, 930, 720);
    const int initialWidth = qMax(
        dialog.minimumWidth(),
        qMin(930, qMax(720, availableGeometry.width() - 40)));
    const int initialHeight = qMax(
        dialog.minimumHeight(),
        qMin(720, qMax(520, availableGeometry.height() - 40)));
    dialog.resize(initialWidth, initialHeight);

    auto *pages = new QStackedWidget(&dialog);

    // HUD tab
    auto *hudTab = new QWidget(pages);
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

    auto *hudTitle = new QLabel(QStringLiteral("HUD"), hudTab);
    QFont hudTitleFont = hudTitle->font();
    hudTitleFont.setPointSize(15);
    hudTitleFont.setBold(true);
    hudTitle->setFont(hudTitleFont);

    auto *hudSubtitle = new QLabel(
        QStringLiteral("Control the compact private status HUD shown over your desktop."),
        hudTab);
    hudSubtitle->setWordWrap(true);
    hudSubtitle->setProperty("muted", true);

    auto *hudLayout = new QVBoxLayout(hudTab);
    hudLayout->setContentsMargins(18, 18, 18, 18);
    hudLayout->setSpacing(12);
    hudLayout->addWidget(hudTitle);
    hudLayout->addWidget(hudSubtitle);
    hudLayout->addWidget(hudCard);
    hudLayout->addWidget(hudInfo);
    hudLayout->addStretch();

    const int hudPageIndex = pages->addWidget(hudTab);

    // Browser overlays tab
    auto *browserTab = new QWidget(pages);
    auto *browserLayout = new QVBoxLayout(browserTab);
    browserLayout->setContentsMargins(16, 16, 16, 16);
    browserLayout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("Browser Overlays"), browserTab);
    QFont titleFont = title->font();
    titleFont.setPointSize(15);
    titleFont.setBold(true);
    title->setFont(titleFont);

    auto *subtitle = new QLabel(
        QStringLiteral("Add up to five Streamlabs/browser URLs or choose local image/HTML files. Transparent local and remote images render without a browser background."),
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
        rows[i].url->setPlaceholderText(QStringLiteral("URL or local file"));
        rows[i].browse = new QPushButton(QStringLiteral("Browse Local…"), card);

        auto *sourceRow = new QHBoxLayout();
        sourceRow->setSpacing(7);
        sourceRow->addWidget(rows[i].url, 1);
        sourceRow->addWidget(rows[i].browse);

        auto *opacityRow = new QHBoxLayout();
        opacityRow->setSpacing(8);
        auto *opacityLabel = new QLabel(QStringLiteral("Opacity"), card);
        rows[i].opacity = new QSlider(Qt::Horizontal, card);
        rows[i].opacity->setRange(10, 100);
        rows[i].opacity->setValue(overlayConfigs[i].opacityPercent);
        rows[i].opacityValue =
            new QLabel(QStringLiteral("%1%").arg(overlayConfigs[i].opacityPercent), card);
        rows[i].opacityValue->setMinimumWidth(38);
        rows[i].opacityValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        opacityRow->addWidget(opacityLabel);
        opacityRow->addWidget(rows[i].opacity, 1);
        opacityRow->addWidget(rows[i].opacityValue);

        QObject::connect(
            rows[i].opacity,
            &QSlider::valueChanged,
            &dialog,
            [&, i](int value) {
                rows[i].opacityValue->setText(QStringLiteral("%1%").arg(value));
            });

        auto *bottomRow = new QHBoxLayout();
        rows[i].sizeLabel = new QLabel(overlaySizeText(overlayConfigs[i]), card);
        rows[i].sizeLabel->setProperty("muted", true);
        rows[i].preview = new QPushButton(QStringLiteral("Preview / Edit"), card);

        bottomRow->addWidget(rows[i].sizeLabel);
        bottomRow->addStretch();
        bottomRow->addWidget(rows[i].preview);

        cardLayout->addLayout(topRow);
        cardLayout->addLayout(sourceRow);
        cardLayout->addLayout(opacityRow);
        cardLayout->addLayout(bottomRow);
        cards->addWidget(card);

        QObject::connect(rows[i].browse, &QPushButton::clicked, &dialog, [&, i]() {
            QString initialDirectory;
            const QString currentLocalPath = localFilePathFromValue(rows[i].url->text());
            if (!currentLocalPath.isEmpty())
                initialDirectory = QFileInfo(currentLocalPath).absolutePath();

            const QString path = QFileDialog::getOpenFileName(
                &dialog,
                QStringLiteral("Choose Local Overlay File"),
                initialDirectory,
                QStringLiteral(
                    "Overlay files (*.png *.apng *.gif *.webp *.svg *.jpg *.jpeg *.bmp *.avif *.html *.htm);;"
                    "Images (*.png *.apng *.gif *.webp *.svg *.jpg *.jpeg *.bmp *.avif);;"
                    "HTML files (*.html *.htm);;"
                    "All files (*.*)"));

            if (!path.isEmpty()) {
                rows[i].url->setText(QDir::toNativeSeparators(path));
                if (rows[i].name->text().trimmed().isEmpty() ||
                    rows[i].name->text().startsWith(QStringLiteral("Overlay "))) {
                    rows[i].name->setText(QFileInfo(path).completeBaseName());
                }
            }
        });

        QObject::connect(rows[i].preview, &QPushButton::clicked, &dialog, [&, i]() {
            overlayConfigs[i].enabled = rows[i].enabled->isChecked();
            overlayConfigs[i].name = rows[i].name->text().trimmed();
            overlayConfigs[i].url = rows[i].url->text().trimmed();
            overlayConfigs[i].mode = static_cast<OverlayMode>(rows[i].mode->currentData().toInt());
            overlayConfigs[i].opacityPercent = rows[i].opacity->value();

            showOverlayPreview(&dialog, i, overlayConfigs[i]);

            rows[i].sizeLabel->setText(overlaySizeText(overlayConfigs[i]));
        });
    }

    cards->addStretch();
    scrollBody->setLayout(cards);
    scroll->setWidget(scrollBody);
    browserLayout->addWidget(scroll, 1);

    auto *videoNote = new QLabel(
        QStringLiteral("HUD overlays preserve transparency and stay invisible until content is rendered. Direct image URLs and local image files are wrapped on a transparent canvas automatically. Local HTML files use OBS Browser Source local-file mode. The Opacity slider applies to whichever output mode is selected: HUD, VIDEO, or both. VIDEO overlays are added to the current OBS scene when you press Apply or OK."),
        browserTab);
    videoNote->setWordWrap(true);
    videoNote->setProperty("accentNote", true);
    browserLayout->addWidget(videoNote);

    const int browserPageIndex = pages->addWidget(browserTab);

    // Future-ready settings pages. Existing behavior stays on HUD and Browser Overlays;
    // the other pages give Clatasha HUD a consistent product shell as it grows.
    auto makeInfoPage = [&](const QString &pageTitle,
                            const QString &pageSubtitle,
                            const QString &bodyText) {
        auto *page = new QWidget(pages);
        auto *pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(18, 18, 18, 18);
        pageLayout->setSpacing(12);

        auto *pageHeading = new QLabel(pageTitle, page);
        QFont headingFont = pageHeading->font();
        headingFont.setPointSize(15);
        headingFont.setBold(true);
        pageHeading->setFont(headingFont);

        auto *pageSub = new QLabel(pageSubtitle, page);
        pageSub->setWordWrap(true);
        pageSub->setProperty("muted", true);

        auto *card = new QGroupBox(page);
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(18, 18, 18, 18);
        auto *body = new QLabel(bodyText, card);
        body->setWordWrap(true);
        body->setTextFormat(Qt::RichText);
        body->setOpenExternalLinks(true);
        cardLayout->addWidget(body);

        pageLayout->addWidget(pageHeading);
        pageLayout->addWidget(pageSub);
        pageLayout->addWidget(card);
        pageLayout->addStretch();
        return page;
    };

    auto *generalPage = makeInfoPage(
        QStringLiteral("General"),
        QStringLiteral("Clatasha HUD status and core behavior."),
        QStringLiteral(
            "<b>Clatasha HUD</b> runs inside OBS Studio and provides a private status HUD "
            "plus browser overlays for HUD, VIDEO, or both.<br><br>"
            "The main HUD starts automatically with OBS and now includes a visibility "
            "watchdog that restores it if Windows unexpectedly hides it."));

    auto *appearancePage = makeInfoPage(
        QStringLiteral("Appearance"),
        QStringLiteral("Visual controls for Clatasha HUD."),
        QStringLiteral(
            "The current compact HUD uses Clatasha's dark interface. "
            "HUD opacity and screen-corner placement are available on the <b>HUD</b> page. "
            "This page is ready for future theme and visual options."));

    auto *hotkeysPage = new QWidget(pages);
    auto *hotkeysLayout = new QVBoxLayout(hotkeysPage);
    hotkeysLayout->setContentsMargins(18, 18, 18, 18);
    hotkeysLayout->setSpacing(12);

    auto *hotkeysTitle = new QLabel(QStringLiteral("Hotkeys"), hotkeysPage);
    QFont hotkeysTitleFont = hotkeysTitle->font();
    hotkeysTitleFont.setPointSize(15);
    hotkeysTitleFont.setBold(true);
    hotkeysTitle->setFont(hotkeysTitleFont);

    auto *hotkeysSubtitle = new QLabel(
        QStringLiteral(
            "Shortcuts for the OBS actions Clatasha HUD monitors and controls. "
            "These are real OBS frontend hotkeys and also appear in OBS Settings → Hotkeys."),
        hotkeysPage);
    hotkeysSubtitle->setWordWrap(true);
    hotkeysSubtitle->setProperty("muted", true);

    hotkeysLayout->addWidget(hotkeysTitle);
    hotkeysLayout->addWidget(hotkeysSubtitle);

    auto *hotkeysScroll = new QScrollArea(hotkeysPage);
    hotkeysScroll->setWidgetResizable(true);
    hotkeysScroll->setFrameShape(QFrame::NoFrame);
    hotkeysScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *hotkeysScrollBody = new QWidget(hotkeysScroll);
    auto *hotkeysCardsLayout = new QVBoxLayout(hotkeysScrollBody);
    hotkeysCardsLayout->setContentsMargins(0, 4, 6, 4);
    hotkeysCardsLayout->setSpacing(10);
    hotkeysScroll->setWidget(hotkeysScrollBody);
    hotkeysLayout->addWidget(hotkeysScroll, 1);

    std::array<HotkeyCaptureEdit *, kHudHotkeyCount> hotkeyEdits{};
    std::array<bool, kHudHotkeyCount> hotkeyDirty{};

    QString lastGroup;
    QGroupBox *currentHotkeyGroup = nullptr;
    QGridLayout *currentHotkeyGrid = nullptr;
    int currentHotkeyRow = 0;

    for (int i = 0; i < kHudHotkeyCount; ++i) {
        const QString groupName = QString::fromUtf8(g_hudHotkeys[i].group);
        if (groupName != lastGroup) {
            currentHotkeyGroup = new QGroupBox(groupName, hotkeysScrollBody);
            currentHotkeyGrid = new QGridLayout(currentHotkeyGroup);
            currentHotkeyGrid->setContentsMargins(14, 20, 14, 14);
            currentHotkeyGrid->setHorizontalSpacing(8);
            currentHotkeyGrid->setVerticalSpacing(8);
            currentHotkeyGrid->setColumnStretch(0, 1);
            currentHotkeyRow = 0;
            hotkeysCardsLayout->addWidget(currentHotkeyGroup);
            lastGroup = groupName;
        }

        auto *label = new QLabel(
            QString::fromUtf8(g_hudHotkeys[i].label),
            currentHotkeyGroup);

        auto *capture = new HotkeyCaptureEdit(currentHotkeyGroup);
        capture->setCombination(currentHotkeyBinding(g_hudHotkeys[i].id));
        hotkeyEdits[i] = capture;
        capture->changedCallback = [&, i](obs_key_combination_t) {
            hotkeyDirty[i] = true;
        };

        auto *changeButton =
            new QPushButton(QStringLiteral("Change"), currentHotkeyGroup);
        auto *clearButton =
            new QPushButton(QStringLiteral("Clear"), currentHotkeyGroup);

        QObject::connect(changeButton, &QPushButton::clicked, &dialog, [capture]() {
            capture->setFocus(Qt::ShortcutFocusReason);
            capture->selectAll();
        });
        QObject::connect(clearButton, &QPushButton::clicked, &dialog, [capture]() {
            capture->clearCombination();
        });

        currentHotkeyGrid->addWidget(label, currentHotkeyRow, 0);
        currentHotkeyGrid->addWidget(capture, currentHotkeyRow, 1);
        currentHotkeyGrid->addWidget(changeButton, currentHotkeyRow, 2);
        currentHotkeyGrid->addWidget(clearButton, currentHotkeyRow, 3);
        ++currentHotkeyRow;
    }

    auto *hotkeyNote = new QLabel(
        QStringLiteral(
            "No shortcuts are assigned by default. Click Change and press a shortcut. "
            "Delete or Backspace clears the selected binding."),
        hotkeysScrollBody);
    hotkeyNote->setWordWrap(true);
    hotkeyNote->setProperty("accentNote", true);
    hotkeysCardsLayout->addWidget(hotkeyNote);
    hotkeysCardsLayout->addStretch();

    auto *advancedPage = new QWidget(pages);
    auto *advancedLayout = new QVBoxLayout(advancedPage);
    advancedLayout->setContentsMargins(18, 18, 18, 18);
    advancedLayout->setSpacing(12);

    auto *advancedTitle =
        new QLabel(QStringLiteral("Advanced"), advancedPage);
    QFont advancedTitleFont = advancedTitle->font();
    advancedTitleFont.setPointSize(15);
    advancedTitleFont.setBold(true);
    advancedTitle->setFont(advancedTitleFont);

    auto *advancedSubtitle = new QLabel(
        QStringLiteral(
            "Game-window compatibility and lower-level Clatasha HUD controls."),
        advancedPage);
    advancedSubtitle->setWordWrap(true);
    advancedSubtitle->setProperty("muted", true);

    auto *gameWindowCard =
        new QGroupBox(QStringLiteral("Game Window"), advancedPage);
    auto *gameWindowLayout = new QVBoxLayout(gameWindowCard);
    gameWindowLayout->setContentsMargins(14, 20, 14, 14);
    gameWindowLayout->setSpacing(10);

    auto *gameTargetLabel =
        new QLabel(QStringLiteral("Target: checking…"), gameWindowCard);
    gameTargetLabel->setWordWrap(true);

    auto *gameWindowStatus =
        new QLabel(QStringLiteral("Status: Original window mode"), gameWindowCard);
    gameWindowStatus->setProperty("muted", true);

    auto *keepBorderless = new QCheckBox(
        QStringLiteral("Keep Borderless Applied"),
        gameWindowCard);
#ifdef Q_OS_WIN
    keepBorderless->setChecked(g_keepGameBorderlessApplied);
#else
    keepBorderless->setChecked(false);
    keepBorderless->setEnabled(false);
#endif

    auto *displayCaptureSafeMode = new QCheckBox(
        QStringLiteral("Display Capture Safe Mode"),
        gameWindowCard);
#ifdef Q_OS_WIN
    displayCaptureSafeMode->setChecked(
        g_displayCaptureSafeModeEnabled);
#else
    displayCaptureSafeMode->setChecked(false);
    displayCaptureSafeMode->setEnabled(false);
#endif

    auto *displayCaptureSafeNote = new QLabel(
        QStringLiteral(
            "Off by default. When enabled, Clatasha can keep the HUD local-only "
            "for OBS Display Capture. If needed, it may switch the selected game "
            "to Clatasha Borderless Fullscreen and temporarily enable Windows "
            "taskbar Auto-hide. Nothing is changed unless you enable this option."),
        gameWindowCard);
    displayCaptureSafeNote->setWordWrap(true);
    displayCaptureSafeNote->setProperty("muted", true);

    auto *gameButtons = new QHBoxLayout();
    auto *forceBorderlessButton = new QPushButton(
        QStringLiteral("Force Borderless Fullscreen"),
        gameWindowCard);
    auto *restoreWindowButton = new QPushButton(
        QStringLiteral("Restore Original Window"),
        gameWindowCard);
    gameButtons->addWidget(forceBorderlessButton);
    gameButtons->addWidget(restoreWindowButton);
    gameButtons->addStretch();

    auto *gameWindowNote = new QLabel(
        QStringLiteral(
            "Clatasha remembers the last non-OBS foreground window. If the target "
            "is wrong, Alt-Tab to the game once, then return here. Borderless "
            "Fullscreen removes the title bar and window buttons and fills "
            "the game's current monitor while leaving a one-pixel composition "
            "guard. This avoids Windows promoting the game into an exact "
            "fullscreen DirectFlip path. While this mode is active, Clatasha "
            "temporarily enables Windows taskbar Auto-hide when needed, so the "
            "taskbar stays out of the way but still appears when you move the "
            "pointer to the screen edge. Clatasha restores the setting when "
            "Borderless is turned off. Some elevated or protected games may block "
            "window-style changes."),
        gameWindowCard);
    gameWindowNote->setWordWrap(true);
    gameWindowNote->setProperty("accentNote", true);

    gameWindowLayout->addWidget(gameTargetLabel);
    gameWindowLayout->addWidget(gameWindowStatus);
    gameWindowLayout->addWidget(keepBorderless);
    gameWindowLayout->addWidget(displayCaptureSafeMode);
    gameWindowLayout->addWidget(displayCaptureSafeNote);
    gameWindowLayout->addLayout(gameButtons);
    gameWindowLayout->addWidget(gameWindowNote);

    advancedLayout->addWidget(advancedTitle);
    advancedLayout->addWidget(advancedSubtitle);
    advancedLayout->addWidget(gameWindowCard);
    advancedLayout->addStretch();

#ifdef Q_OS_WIN
    auto refreshGameWindowUi = [=]() {
        HWND target = g_gameBorderless.active
                          ? g_gameBorderless.window
                          : g_lastExternalForegroundWindow;
        gameTargetLabel->setText(
            QStringLiteral("Target: %1")
                .arg(externalWindowDescription(target)));
        gameWindowStatus->setText(
            g_gameBorderless.active
                ? QStringLiteral("Status: Borderless Fullscreen active")
                : QStringLiteral("Status: Original window mode"));
        restoreWindowButton->setEnabled(g_gameBorderless.active);
    };

    auto *gameUiTimer = new QTimer(advancedPage);
    gameUiTimer->setInterval(500);
    QObject::connect(
        gameUiTimer,
        &QTimer::timeout,
        advancedPage,
        refreshGameWindowUi);
    gameUiTimer->start();
    refreshGameWindowUi();

    QObject::connect(
        forceBorderlessButton,
        &QPushButton::clicked,
        &dialog,
        [&, refreshGameWindowUi]() {
            if (!forceLastExternalGameBorderless()) {
                QMessageBox::warning(
                    &dialog,
                    QStringLiteral("Game Window"),
                    QStringLiteral(
                        "Clatasha could not modify the target window. Alt-Tab "
                        "to the game once and try again. Elevated or protected "
                        "games can block window-style changes."));
            }
            refreshGameWindowUi();
        });

    QObject::connect(
        restoreWindowButton,
        &QPushButton::clicked,
        &dialog,
        [refreshGameWindowUi]() {
            restoreGameBorderlessWindow();
            refreshGameWindowUi();
        });
#else
    forceBorderlessButton->setEnabled(false);
    restoreWindowButton->setEnabled(false);
    gameTargetLabel->setText(QStringLiteral("Target: Windows only"));
    gameWindowStatus->setText(
        QStringLiteral("Status: Game Window controls require Windows"));
#endif

    auto *aboutPage = makeInfoPage(
        QStringLiteral("About"),
        QStringLiteral("Clatasha HUD"),
        QStringLiteral(
            "<b>Clatasha HUD v%1</b><br>"
            "Stream Smarter. Create More.<br><br>"
            "<a href='https://github.com/Clatasha/Clatasha-HUD'>GitHub project</a><br><br>"
            "Built as part of the Clatasha creator-tool ecosystem.")
            .arg(currentHudVersion()));

    const int generalPageIndex = pages->addWidget(generalPage);
    const int appearancePageIndex = pages->addWidget(appearancePage);
    const int hotkeysPageIndex = pages->addWidget(hotkeysPage);
    const int advancedPageIndex = pages->addWidget(advancedPage);
    const int aboutPageIndex = pages->addWidget(aboutPage);

    // Branded header: logo sits left of the two-line title/slogan block and is
    // vertically centered across both lines.
    auto *header = new QFrame(&dialog);
    header->setObjectName(QStringLiteral("settingsHeader"));
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 9, 16, 9);
    headerLayout->setSpacing(10);

    auto *brandLogo = new QLabel(header);
    brandLogo->setFixedSize(42, 42);
    brandLogo->setAlignment(Qt::AlignCenter);
    if (!g_hud->logoPixmap().isNull()) {
        brandLogo->setPixmap(
            g_hud->logoPixmap().scaled(
                40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    auto *brandText = new QWidget(header);
    auto *brandTextLayout = new QVBoxLayout(brandText);
    brandTextLayout->setContentsMargins(0, 0, 0, 0);
    brandTextLayout->setSpacing(0);

    auto *brandTitle = new QLabel(QStringLiteral("Clatasha HUD Settings"), brandText);
    QFont brandTitleFont = brandTitle->font();
    brandTitleFont.setPointSize(12);
    brandTitleFont.setBold(true);
    brandTitle->setFont(brandTitleFont);

    auto *brandSlogan =
        new QLabel(QStringLiteral("Stream Smarter. Create More."), brandText);
    brandSlogan->setObjectName(QStringLiteral("brandSlogan"));

    brandTextLayout->addStretch();
    brandTextLayout->addWidget(brandTitle);
    brandTextLayout->addWidget(brandSlogan);
    brandTextLayout->addStretch();

    auto *versionLabel =
        new QLabel(QStringLiteral("v%1").arg(currentHudVersion()), header);
    versionLabel->setObjectName(QStringLiteral("versionLabel"));
    g_settingsVersionLabel = versionLabel;

    auto *currentCheckIcon = new QLabel(header);
    currentCheckIcon->setObjectName(QStringLiteral("currentVersionCheck"));
    currentCheckIcon->setFixedSize(16, 16);
    currentCheckIcon->setAlignment(Qt::AlignCenter);
    currentCheckIcon->setPixmap(
        QIcon(QStringLiteral(":/clatasha/icons/checkmark.svg"))
            .pixmap(QSize(14, 14)));
    currentCheckIcon->hide();
    g_settingsCurrentCheckIcon = currentCheckIcon;

    auto *updateButton =
        new QPushButton(QStringLiteral("●  Update available"), header);
    updateButton->setObjectName(QStringLiteral("updateButton"));
    updateButton->setCursor(Qt::PointingHandCursor);
    updateButton->hide();
    g_settingsUpdateButton = updateButton;

    auto *donateButton = new QPushButton(QStringLiteral("Donate"), header);
    donateButton->setObjectName(QStringLiteral("donateButton"));
    donateButton->setIcon(
        QIcon(QStringLiteral(":/clatasha/icons/donate-heart.svg")));
    donateButton->setIconSize(QSize(16, 16));
    donateButton->setCursor(Qt::PointingHandCursor);

    headerLayout->addWidget(brandLogo);
    headerLayout->addWidget(brandText);
    headerLayout->addStretch();
    headerLayout->addWidget(versionLabel);
    headerLayout->addSpacing(3);
    headerLayout->addWidget(currentCheckIcon);
    headerLayout->addSpacing(6);
    headerLayout->addWidget(updateButton);
    headerLayout->addSpacing(6);
    headerLayout->addWidget(donateButton);

    QObject::connect(updateButton, &QPushButton::clicked, &dialog, []() {
        QDesktopServices::openUrl(
            g_updateCheck.releaseUrl.isValid()
                ? g_updateCheck.releaseUrl
                : kLatestReleaseFallbackUrl);
    });

    QObject::connect(&dialog, &QObject::destroyed, []() {
        g_settingsVersionLabel.clear();
        g_settingsCurrentCheckIcon.clear();
        g_settingsUpdateButton.clear();
    });

    refreshSettingsUpdateUi();
    checkForClatashaHudUpdate();

    QObject::connect(donateButton, &QPushButton::clicked, &dialog, [&]() {
        QDialog donateDialog(&dialog);
        donateDialog.setWindowTitle(QStringLiteral("Support Clatasha HUD"));
        donateDialog.setModal(true);
        donateDialog.setFixedWidth(470);

        auto *donateLayout = new QVBoxLayout(&donateDialog);
        donateLayout->setContentsMargins(18, 18, 18, 18);
        donateLayout->setSpacing(12);

        auto *donateTitle = new QLabel(QStringLiteral("Support Clatasha HUD"), &donateDialog);
        QFont donateTitleFont = donateTitle->font();
        donateTitleFont.setPointSize(14);
        donateTitleFont.setBold(true);
        donateTitle->setFont(donateTitleFont);

        auto *donateText = new QLabel(
            QStringLiteral(
                "If Clatasha HUD helps your stream, you can support continued development."),
            &donateDialog);
        donateText->setWordWrap(true);
        donateText->setProperty("muted", true);

        auto *toast = new QLabel(&donateDialog);
        toast->setObjectName(QStringLiteral("donationToast"));
        toast->setAlignment(Qt::AlignCenter);
        toast->hide();

        auto *toastTimer = new QTimer(&donateDialog);
        toastTimer->setSingleShot(true);
        QObject::connect(toastTimer, &QTimer::timeout, toast, &QLabel::hide);

        auto showDonationToast = [&](const QString &message) {
            toast->setText(message);
            toast->show();
            toastTimer->start(1800);
        };

        auto makeSupportRow = [&](const QString &titleText,
                                  const QString &detailText,
                                  const QString &buttonText,
                                  std::function<void()> action) {
            auto *row = new QFrame(&donateDialog);
            row->setObjectName(QStringLiteral("supportRow"));
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(12, 10, 12, 10);

            auto *textWrap = new QWidget(row);
            auto *textLayout = new QVBoxLayout(textWrap);
            textLayout->setContentsMargins(0, 0, 0, 0);
            textLayout->setSpacing(1);

            auto *title = new QLabel(titleText, textWrap);
            QFont titleFont = title->font();
            titleFont.setBold(true);
            title->setFont(titleFont);

            auto *detail = new QLabel(detailText, textWrap);
            detail->setProperty("muted", true);
            detail->setTextInteractionFlags(Qt::TextSelectableByMouse);

            auto *actionButton = new QPushButton(buttonText, row);
            actionButton->setCursor(Qt::PointingHandCursor);

            textLayout->addWidget(title);
            textLayout->addWidget(detail);
            rowLayout->addWidget(textWrap, 1);
            rowLayout->addWidget(actionButton);

            QObject::connect(actionButton, &QPushButton::clicked, &donateDialog,
                             [action = std::move(action)]() { action(); });
            return row;
        };

        auto *kofiRow = makeSupportRow(
            QStringLiteral("Ko-fi"),
            QStringLiteral("ko-fi.com/derspawn"),
            QStringLiteral("Open Ko-fi"),
            [&]() {
                QDesktopServices::openUrl(QUrl(QStringLiteral("https://ko-fi.com/derspawn")));
                showDonationToast(QStringLiteral("Opening Ko-fi…"));
            });

        auto *paypalRow = makeSupportRow(
            QStringLiteral("PayPal"),
            QStringLiteral("paypal.me/BFHQ"),
            QStringLiteral("Open PayPal"),
            [&]() {
                QDesktopServices::openUrl(
                    QUrl(QStringLiteral("https://www.paypal.com/paypalme/BFHQ")));
                showDonationToast(QStringLiteral("Opening PayPal…"));
            });

        const QString bitcoinAddress =
            QStringLiteral("14pqVhaQyGWYzz8XcYLNaCyGKHN2G1gZcA");
        auto *bitcoinRow = makeSupportRow(
            QStringLiteral("Bitcoin"),
            bitcoinAddress,
            QStringLiteral("Copy Address"),
            [&]() {
                if (QGuiApplication::clipboard())
                    QGuiApplication::clipboard()->setText(bitcoinAddress);
                showDonationToast(QStringLiteral("✓ Bitcoin address copied"));
            });

        auto *closeButton = new QPushButton(QStringLiteral("Close"), &donateDialog);
        QObject::connect(closeButton, &QPushButton::clicked, &donateDialog, &QDialog::accept);

        donateLayout->addWidget(donateTitle);
        donateLayout->addWidget(donateText);
        donateLayout->addWidget(kofiRow);
        donateLayout->addWidget(paypalRow);
        donateLayout->addWidget(bitcoinRow);
        donateLayout->addWidget(toast);
        donateLayout->addWidget(closeButton, 0, Qt::AlignRight);

        donateDialog.setStyleSheet(QStringLiteral(
            "QDialog { background:#0c1015; color:#edf3f8; }"
            "QFrame#supportRow { background:#151b22; border:1px solid #2b3641; border-radius:8px; }"
            "QLabel[muted='true'] { color:#8d9aa6; }"
            "QPushButton { background:#202832; color:#edf3f8; border:1px solid #344250;"
            " border-radius:6px; padding:7px 12px; }"
            "QPushButton:hover { background:#293441; border-color:#4a5d70; }"
            "QLabel#donationToast { background:#153820; color:#bdf5c9;"
            " border:1px solid #2d7140; border-radius:6px; padding:7px 10px; }"));

        donateDialog.exec();
    });

    // Left navigation.
    auto *sidebar = new QFrame(&dialog);
    sidebar->setObjectName(QStringLiteral("settingsSidebar"));
    sidebar->setFixedWidth(168);
    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(8, 12, 8, 12);
    sidebarLayout->setSpacing(5);

    auto *navGroup = new QButtonGroup(&dialog);
    navGroup->setExclusive(true);

    auto addNavButton = [&](const QString &label,
                            int pageIndex,
                            const QString &iconPath,
                            bool bottom) {
        auto *button = new QPushButton(label, sidebar);
        button->setCheckable(true);
        button->setProperty("navButton", true);
        button->setCursor(Qt::PointingHandCursor);
        if (!iconPath.isEmpty()) {
            button->setIcon(QIcon(iconPath));
            button->setIconSize(QSize(18, 18));
        }
        navGroup->addButton(button);
        if (bottom)
            sidebarLayout->addStretch();
        sidebarLayout->addWidget(button);

        QObject::connect(button, &QPushButton::clicked, pages,
                         [pages, pageIndex]() { pages->setCurrentIndex(pageIndex); });
        return button;
    };

    auto *generalNav = addNavButton(
        QStringLiteral("General"),
        generalPageIndex,
        QStringLiteral(":/clatasha/icons/general.svg"),
        false);
    auto *browserNav = addNavButton(
        QStringLiteral("Browser Overlays"),
        browserPageIndex,
        QStringLiteral(":/clatasha/icons/browser-overlays.svg"),
        false);
    auto *hudNav = addNavButton(
        QStringLiteral("HUD"),
        hudPageIndex,
        QStringLiteral(":/clatasha/icons/hud.svg"),
        false);
    auto *appearanceNav = addNavButton(
        QStringLiteral("Appearance"),
        appearancePageIndex,
        QStringLiteral(":/clatasha/icons/appearance.svg"),
        false);
    auto *hotkeysNav = addNavButton(
        QStringLiteral("Hotkeys"),
        hotkeysPageIndex,
        QStringLiteral(":/clatasha/icons/hotkeys.svg"),
        false);
    auto *advancedNav = addNavButton(
        QStringLiteral("Advanced"),
        advancedPageIndex,
        QStringLiteral(":/clatasha/icons/advanced.svg"),
        false);
    auto *aboutNav = addNavButton(
        QStringLiteral("About"),
        aboutPageIndex,
        QStringLiteral(":/clatasha/icons/about.svg"),
        true);

    // Browser Overlays is the most-used configuration page and mirrors the concept.
    browserNav->setChecked(true);
    pages->setCurrentIndex(browserPageIndex);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply, &dialog);

    auto *applyToast = new QFrame(&dialog);
    applyToast->setObjectName(QStringLiteral("applyToast"));
    applyToast->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    auto *applyToastLayout = new QHBoxLayout(applyToast);
    applyToastLayout->setContentsMargins(12, 8, 14, 8);
    applyToastLayout->setSpacing(7);

    auto *applyToastIcon = new QLabel(applyToast);
    applyToastIcon->setFixedSize(16, 16);
    applyToastIcon->setAlignment(Qt::AlignCenter);
    applyToastIcon->setPixmap(
        QIcon(QStringLiteral(":/clatasha/icons/checkmark.svg"))
            .pixmap(QSize(14, 14)));

    auto *applyToastText =
        new QLabel(QStringLiteral("Changes applied"), applyToast);
    applyToastText->setObjectName(QStringLiteral("applyToastText"));

    applyToastLayout->addWidget(applyToastIcon);
    applyToastLayout->addWidget(applyToastText);
    applyToast->setStyleSheet(QStringLiteral(
        "QFrame#applyToast {"
        " background:#153820;"
        " border:1px solid #2d7140;"
        " border-radius:7px;"
        "}"
        "QLabel#applyToastText {"
        " color:#bdf5c9;"
        " font-weight:600;"
        " background:transparent;"
        "}"));
    applyToast->hide();

    auto *toastTimer = new QTimer(&dialog);
    toastTimer->setSingleShot(true);
    QObject::connect(toastTimer, &QTimer::timeout, applyToast, &QWidget::hide);

    auto showApplyToast = [&]() {
        applyToast->adjustSize();
        applyToast->move(
            qMax(12, (dialog.width() - applyToast->width()) / 2),
            qMax(12, dialog.height() - applyToast->height() - 62));
        applyToast->raise();
        applyToast->show();
        toastTimer->start(1800);
    };

    auto syncRowsToConfigs = [&]() {
        for (int i = 0; i < kOverlayCount; ++i) {
            overlayConfigs[i].enabled = rows[i].enabled->isChecked();
            overlayConfigs[i].name = rows[i].name->text().trimmed();
            if (overlayConfigs[i].name.isEmpty())
                overlayConfigs[i].name = QStringLiteral("Overlay %1").arg(i + 1);
            overlayConfigs[i].url = rows[i].url->text().trimmed();
            overlayConfigs[i].mode = static_cast<OverlayMode>(rows[i].mode->currentData().toInt());
            overlayConfigs[i].opacityPercent = rows[i].opacity->value();
        }
    };

    auto applySettings = [&]() -> bool {
        syncRowsToConfigs();

        // Prevent two Clatasha actions from accidentally sharing the same
        // shortcut. OBS permits this, but it would trigger both actions.
        for (int i = 0; i < kHudHotkeyCount; ++i) {
            const obs_key_combination_t a = hotkeyEdits[i]->combination();
            if (obs_key_combination_is_empty(a))
                continue;

            for (int j = i + 1; j < kHudHotkeyCount; ++j) {
                const obs_key_combination_t b = hotkeyEdits[j]->combination();
                if (a.key == b.key && a.modifiers == b.modifiers) {
                    QMessageBox::warning(
                        &dialog,
                        QStringLiteral("Duplicate Clatasha HUD Hotkey"),
                        QStringLiteral("%1 and %2 are using the same shortcut: %3")
                            .arg(QString::fromUtf8(g_hudHotkeys[i].label))
                            .arg(QString::fromUtf8(g_hudHotkeys[j].label))
                            .arg(hotkeyCombinationText(a)));
                    return false;
                }
            }
        }

        for (int i = 0; i < kHudHotkeyCount; ++i) {
            if (!hotkeyDirty[i])
                continue;

            obs_key_combination_t combination = hotkeyEdits[i]->combination();
            if (obs_key_combination_is_empty(combination))
                obs_hotkey_load_bindings(g_hudHotkeys[i].id, nullptr, 0);
            else
                obs_hotkey_load_bindings(g_hudHotkeys[i].id, &combination, 1);

            hotkeyDirty[i] = false;
        }

        saveHudHotkeys();
        g_hud->saveSettings();
        saveOverlayConfigs(overlayConfigs);
#ifdef Q_OS_WIN
        g_keepGameBorderlessApplied =
            keepBorderless->isChecked();

        const bool safeModeWasEnabled =
            g_displayCaptureSafeModeEnabled;
        g_displayCaptureSafeModeEnabled =
            displayCaptureSafeMode->isChecked();

        if (safeModeWasEnabled &&
            !g_displayCaptureSafeModeEnabled) {
            if (g_displayCaptureSafeEvent)
                ResetEvent(g_displayCaptureSafeEvent);
            if (g_displayCaptureForcedBorderless) {
                restoreGameBorderlessWindow();
                g_displayCaptureForcedBorderless = false;
            }
            g_displayCaptureSafeActive = false;
        }

        saveGameWindowSettings();
        updateDisplayCaptureSafeMode();
#endif

        const bool videoOk = applyVideoOverlays(overlayConfigs);
        const bool hudOk = applyHudOverlays(overlayConfigs);

        if (!videoOk || !hudOk) {
            QMessageBox::warning(
                &dialog,
                QStringLiteral("Browser Overlay Unavailable"),
                QStringLiteral("One or more browser overlays could not be created. Make sure the OBS Browser plugin is installed and enabled."));
            return false;
        }

        return true;
    };

    QObject::connect(opacitySlider, &QSlider::valueChanged, [&](int value) {
        opacityValue->setText(QStringLiteral("%1%").arg(value));
        g_hud->setOpacityPercent(value);
    });

    QObject::connect(locationBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [&](int) {
        g_hud->setLocation(locationBox->currentData().toString());
    });

    QObject::connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog,
                     [&]() {
                         if (applySettings())
                             showApplyToast();
                     });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        if (applySettings())
            dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto *content = new QWidget(&dialog);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(10);
    contentLayout->addWidget(pages, 1);
    contentLayout->addWidget(buttons);

    auto *body = new QWidget(&dialog);
    auto *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(sidebar);
    bodyLayout->addWidget(content, 1);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(body, 1);

    dialog.setStyleSheet(QStringLiteral(
        "QDialog { background:#0b0f14; color:#e9eef3; }"
        "QWidget { color:#e9eef3; font-family:'Segoe UI'; font-size:10pt; }"

        "QFrame#settingsHeader { background:#0d141c; border-bottom:1px solid #26313d; }"
        "QLabel#brandSlogan { color:#5f9ed7; font-size:8.5pt; }"
        "QLabel#versionLabel { color:#81909d; font-size:8.5pt; }"
        "QPushButton#updateButton { background:#3a2d12; color:#ffd98a;"
        " border:1px solid #7b5d1c; border-radius:7px; padding:5px 9px; font-size:8.5pt; font-weight:600; }"
        "QPushButton#updateButton:hover { background:#4a3916; border-color:#a17a24; }"
        "QPushButton#donateButton { background:#16283a; color:#9dd1ff;"
        " border:1px solid #2b5b82; border-radius:7px; padding:7px 14px; font-weight:600; }"
        "QPushButton#donateButton:hover { background:#1b3550; border-color:#3e79a8; }"

        "QFrame#settingsSidebar { background:#101821; border-right:1px solid #26313d; }"
        "QPushButton[navButton='true'] { text-align:left; background:transparent;"
        " color:#aab6c2; border:1px solid transparent; border-radius:6px;"
        " padding:9px 10px; font-weight:500; }"
        "QPushButton[navButton='true']:hover { background:#182431; color:#dce8f3; }"
        "QPushButton[navButton='true']:checked { background:#146dcc; color:white;"
        " border-color:#2389ff; }"

        "QStackedWidget { background:#0d1218; }"
        "QGroupBox { background:#151a20; border:1px solid #29313a; border-radius:8px;"
        " margin-top:10px; font-weight:600; }"
        "QGroupBox::title { subcontrol-origin:margin; left:12px; padding:0 5px; color:#dfe7ee; }"
        "QLineEdit,QComboBox,QSpinBox { background:#0f1318; border:1px solid #303943;"
        " border-radius:5px; padding:6px 8px; selection-background-color:#2389ff; }"
        "QLineEdit:focus,QComboBox:focus,QSpinBox:focus { border:1px solid #2389ff; }"
        "QPushButton { background:#20262d; border:1px solid #343e48; border-radius:5px;"
        " padding:7px 13px; }"
        "QPushButton:hover { background:#29313a; border-color:#46525f; }"
        "QPushButton:pressed { background:#1b2026; }"
        "QCheckBox { spacing:7px; }"
        "QSlider::groove:horizontal { height:4px; background:#2a3139; border-radius:2px; }"
        "QSlider::handle:horizontal { width:14px; margin:-5px 0; background:#2389ff;"
        " border-radius:7px; }"
        "QScrollArea { background:transparent; border:0; }"
        "QScrollBar:vertical { background:#0f1318; width:9px; }"
        "QScrollBar::handle:vertical { background:#343e48; border-radius:4px; min-height:30px; }"
        "QLabel[muted='true'] { color:#8f9aa6; }"
        "QLabel[accentNote='true'] { color:#8cc5ff; background:#111d29;"
        " border:1px solid #24435e; border-radius:6px; padding:8px; }"));


    if (dialog.exec() != QDialog::Accepted) {
        g_hud->setOpacityPercent(originalOpacity);
        g_hud->setLocation(originalLocation);
        applyHudOverlays(originalOverlayConfigs);
    }
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
    switch (event) {
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
        g_hudWantedVisible = true;
        ensure_hud();
        enforceGameCaptureHudExclusion();
        updateDisplayCaptureSafeMode();
        g_hud->show();
        g_hud->positionHud();
        QTimer::singleShot(5000, []() {
            if (!g_frontendExiting)
                checkForClatashaHudUpdate();
        });
        {
            const auto configs = loadOverlayConfigs();
            applyVideoOverlays(configs);
            applyHudOverlays(configs);
            QTimer::singleShot(1200, []() { applyHudOverlays(loadOverlayConfigs()); });
        }
        break;

    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
        enforceGameCaptureHudExclusion();
        updateDisplayCaptureSafeMode();
        applyVideoOverlays(loadOverlayConfigs());
        break;

    case OBS_FRONTEND_EVENT_EXIT:
        restoreGameBorderlessWindow();
        g_frontendExiting = true;
        stopTopmostEventHooks();
        shutdownUpdateChecker();
        g_hudWantedVisible = false;
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

    g_frontendExiting = false;
#ifdef Q_OS_WIN
    {
        wchar_t eventName[96] = {};
        swprintf_s(
            eventName,
            L"Local\\ClatashaHUD_DisplayCaptureSafe_%lu",
            GetCurrentProcessId());
        g_displayCaptureSafeEvent =
            CreateEventW(
                nullptr,
                TRUE,
                FALSE,
                eventName);
        if (!g_displayCaptureSafeEvent) {
            blog(
                LOG_WARNING,
                "[Clatasha HUD] Could not create Display Capture Safe Mode event: %lu",
                GetLastError());
        }
    }
#endif
    loadGameWindowSettings();
    loadCachedUpdateCheck();
    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    registerHudHotkeys();

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
    g_displayCaptureForcedBorderless = false;
    restoreGameBorderlessWindow();
#ifdef Q_OS_WIN
    if (g_displayCaptureSafeEvent) {
        ResetEvent(g_displayCaptureSafeEvent);
        CloseHandle(g_displayCaptureSafeEvent);
        g_displayCaptureSafeEvent = nullptr;
    }
    g_displayCaptureSafeActive = false;
#endif
    g_hudWantedVisible = false;
    g_frontendExiting = true;

    stopTopmostEventHooks();
    shutdownUpdateChecker();

    unregisterHudHotkeys();
    if (!g_frontendExiting)
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
