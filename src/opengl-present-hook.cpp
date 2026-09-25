#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wincrypt.h>
#include <shlwapi.h>
#include <gdiplus.h>
#include <detours.h>
#include <GL/gl.h>

#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kSharedMagic = 0x4C474F43; // "COGL"
constexpr std::uint32_t kSharedVersion = 6;

enum HookBits : LONG {
    HookSwapBuffers = 1 << 0,
    HookWglSwapBuffers = 1 << 1,
    HookWglSwapLayerBuffers = 1 << 2,
};

enum DrawStage : LONG {
    DrawStageIdle = 0,
    DrawStageEligible = 10,
    DrawStageRendererEntry = 11,
    DrawStageFallbackEntry = 15,
    DrawStageRendererReady = 20,
    DrawStageLiveFpsUploadEntry = 21,
    DrawStageLiveFpsUploadDone = 22,
    DrawStageLiveObsFpsUploadEntry = 23,
    DrawStageLiveObsFpsUploadDone = 24,
    DrawStageLiveTimerUploadEntry = 25,
    DrawStageLiveTimerUploadDone = 26,
    DrawStageLiveAudioUploadEntry = 27,
    DrawStageLiveAudioUploadDone = 28,
    DrawStageLiveStatusUploadEntry = 29,
    DrawStageViewportReady = 30,
    DrawStageLiveStatusUploadDone = 31,
    DrawStageStateCaptured = 40,
    DrawStageOverlayStateApplied = 50,
    DrawStageVerticesUploaded = 60,
    DrawStageDrawReturned = 70,
    DrawStageStateRestored = 80,
    DrawStageComplete = 90,
};

struct alignas(8) OpenGlPresentShared {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t pid;
    volatile LONG liveFpsAck; // injected DLL echoes liveFpsInput here
    volatile LONG64 presentCount;
    volatile LONG64 lastPresentTick;
    volatile LONG hookState; // 0 = starting, 1 = active, 2 = failed
    volatile LONG hookedMask;
    volatile LONG drawMarker;
    volatile LONG reservedControl; // draw-stage diagnostic; layout unchanged
    volatile LONG64 drawCount;
    volatile LONG64 lastDrawTick;
    volatile LONG renderMode; // 0 = idle, 1 = modern shader, 2 = fallback marker
    volatile LONG liveFpsInput;
    volatile LONG liveSessionSeconds; // OBS writes elapsed recording/stream seconds
    volatile LONG liveSessionSecondsAck; // last timer value uploaded by renderer
    volatile LONG liveAudioLevels; // desktop low16, mic high16, each 0..1000
    volatile LONG liveAudioLevelsAck;
    volatile LONG liveHudStatus; // disk, session flags, opacity
    volatile LONG liveHudStatusAck;
};

using SwapBuffersFn = BOOL (WINAPI *)(HDC);
using WglSwapBuffersFn = BOOL (WINAPI *)(HDC);
using WglSwapLayerBuffersFn = BOOL (WINAPI *)(HDC, UINT);

SwapBuffersFn g_realSwapBuffers = nullptr;
WglSwapBuffersFn g_realWglSwapBuffers = nullptr;
WglSwapLayerBuffersFn g_realWglSwapLayerBuffers = nullptr;

HANDLE g_sharedMapping = nullptr;
OpenGlPresentShared *g_shared = nullptr;
thread_local LONG g_swapDepth = 0;

void SetHookState(LONG state, LONG mask)
{
    if (!g_shared)
        return;

    InterlockedExchange(&g_shared->hookedMask, mask);
    InterlockedExchange(&g_shared->hookState, state);
}

void RecordPresent()
{
    if (!g_shared)
        return;

    // Transport-only live data proof. Read the helper-provided integer FPS
    // and echo it back. This value is deliberately not used by rendering.
    const LONG liveFps =
        InterlockedCompareExchange(
            &g_shared->liveFpsInput,
            0,
            0);
    InterlockedExchange(
        &g_shared->liveFpsAck,
        liveFps);

    InterlockedIncrement64(&g_shared->presentCount);
    InterlockedExchange64(
        &g_shared->lastPresentTick,
        static_cast<LONG64>(GetTickCount64()));
}

void SetDrawStage(LONG stage)
{
    if (g_shared)
        InterlockedExchange(&g_shared->reservedControl, stage);
}

bool IsFullscreenWindow(HWND hwnd)
{
    if (!hwnd || hwnd != GetForegroundWindow() ||
        !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }

    RECT client{};
    if (!GetClientRect(hwnd, &client))
        return false;

    POINT topLeft{client.left, client.top};
    POINT bottomRight{client.right, client.bottom};
    if (!ClientToScreen(hwnd, &topLeft) ||
        !ClientToScreen(hwnd, &bottomRight)) {
        return false;
    }

    HMONITOR monitor =
        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor)
        return false;

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return false;

    constexpr LONG tolerance = 3;
    const RECT &screen = info.rcMonitor;

    return topLeft.x <= screen.left + tolerance &&
           topLeft.y <= screen.top + tolerance &&
           bottomRight.x >= screen.right - tolerance &&
           bottomRight.y >= screen.bottom - tolerance;
}

constexpr int kHudWidth = 145;
constexpr int kHudHeight = 50;
constexpr int kHudMargin = 12;
constexpr int kFpsPatchX = 22;
constexpr int kFpsPatchY = 0;
constexpr int kFpsPatchWidth = 53;
constexpr int kFpsPatchHeight = 34;
constexpr int kFpsUploadHeight = 27;
constexpr int kObsFpsPatchX = 74;
constexpr int kObsFpsPatchY = 3;
constexpr int kObsFpsPatchWidth = 29;
constexpr int kObsFpsPatchHeight = 23;
constexpr int kTimerPatchX = 21;
constexpr int kTimerPatchY = 27;
constexpr int kTimerPatchWidth = 73;
constexpr int kTimerPatchHeight = 18;
constexpr int kTimerGlyphWidth = 10;
constexpr int kTimerGlyphHeight = 18;
constexpr int kTimerGlyphAdvance = 9;
constexpr int kTimerGlyphCount = 11;
constexpr int kAudioPatchX = 4;
constexpr int kAudioPatchY = 5;
constexpr int kAudioPatchWidth = 17;
constexpr int kAudioPatchHeight = 29;
constexpr int kAudioSegments = 6;
constexpr int kAudioSegmentHeight = 4;
constexpr int kAudioSegmentGap = 1;
constexpr int kDiskPatchX = 94;
constexpr int kDiskPatchY = 37;
constexpr int kDiskPatchWidth = 38;
constexpr int kDiskPatchHeight = 11;
constexpr int kDiskGlyphWidth = 6;
constexpr int kDiskGlyphHeight = 11;
constexpr int kDiskGlyphAdvance = 5;
constexpr int kDiskGlyphCount = 14;
constexpr int kStatePatchX = 101;
constexpr int kStatePatchY = 16;
constexpr int kStatePatchWidth = 42;
constexpr int kStatePatchHeight = 21;
constexpr std::uint32_t kHudStatusDiskMask = 0x000FFFFFu;
constexpr std::uint32_t kHudStatusRecordingBit = 1u << 20;
constexpr std::uint32_t kHudStatusStreamingBit = 1u << 21;
constexpr std::uint32_t kHudStatusReplayBit = 1u << 22;
constexpr std::uint32_t kHudStatusOpacityShift = 23;
constexpr int kMaxCachedFps = 999;
constexpr LONG kMaxTimerSeconds = 359999; // 99:59:59

GLuint g_hudTexture = 0;
HGLRC g_hudTextureContext = nullptr;
std::vector<std::uint8_t> g_hudPixels;
std::vector<std::vector<std::uint8_t>> g_liveFpsPatches;
std::vector<std::vector<std::uint8_t>> g_liveObsFpsPatches;
std::vector<std::vector<std::uint8_t>> g_timerGlyphs;
std::vector<std::uint8_t> g_timerBasePatch;
std::vector<std::uint8_t> g_timerScratch;
std::vector<std::uint8_t> g_audioBasePatch;
std::vector<std::uint8_t> g_audioScratch;
std::vector<std::vector<std::uint8_t>> g_audioLevelPatches;
std::vector<std::uint8_t> g_liveFpsScratch;
std::vector<std::vector<std::uint8_t>> g_diskGlyphs;
std::vector<std::uint8_t> g_diskBasePatch;
std::vector<std::uint8_t> g_diskScratch;
std::vector<std::uint8_t> g_stateBasePatch;
std::vector<std::uint8_t> g_stateScratch;
LONG g_lastRenderedLiveFps = -1;
LONG g_lastRenderedLiveObsFps = -1;
LONG g_lastRenderedSessionSeconds = -1;
LONG g_lastRenderedAudioLevels = -1;
LONG g_lastRenderedHudStatus = -1;
LONG g_lastRenderedDiskTenths = -1;
bool g_lastRenderedSessionActive = false;
ULONGLONG g_lastStatusAnimationTick = 0;

bool ShouldDrawOverlay(HDC dc)
{
    if (!g_shared ||
        InterlockedCompareExchange(&g_shared->drawMarker, 0, 0) == 0 ||
        !dc ||
        !wglGetCurrentContext()) {
        return false;
    }

    if (wglGetCurrentDC() != dc)
        return false;

    return IsFullscreenWindow(WindowFromDC(dc));
}

void DrawMeter(HDC dc, int x, int y)
{
    constexpr int segments = 6;
    constexpr int segmentHeight = 4;
    constexpr int gap = 1;

    HBRUSH active = CreateSolidBrush(RGB(31, 218, 102));
    HBRUSH inactive = CreateSolidBrush(RGB(49, 55, 59));

    for (int i = 0; i < segments; ++i) {
        const int sy = y + 29 - segmentHeight -
                       i * (segmentHeight + gap);
        RECT r{x, sy, x + 3, sy + segmentHeight};
        FillRect(dc, &r, i < 4 ? active : inactive);
    }

    DeleteObject(active);
    DeleteObject(inactive);
}

const char kLogoBase64[] =
"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"
"SUVORK5CYII=";

bool DrawEmbeddedLogo(HDC dc)
{
    DWORD decodedSize = 0;
    if (!CryptStringToBinaryA(
            kLogoBase64,
            0,
            CRYPT_STRING_BASE64,
            nullptr,
            &decodedSize,
            nullptr,
            nullptr) ||
        decodedSize == 0) {
        return false;
    }

    std::vector<BYTE> decoded(
        static_cast<size_t>(decodedSize));
    if (!CryptStringToBinaryA(
            kLogoBase64,
            0,
            CRYPT_STRING_BASE64,
            decoded.data(),
            &decodedSize,
            nullptr,
            nullptr)) {
        return false;
    }

    IStream *stream =
        SHCreateMemStream(
            decoded.data(),
            decodedSize);
    if (!stream)
        return false;

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(
            &token,
            &startupInput,
            nullptr) != Gdiplus::Ok) {
        stream->Release();
        return false;
    }

    bool drawn = false;
    {
        Gdiplus::Image image(
            stream,
            FALSE);
        if (image.GetLastStatus() ==
            Gdiplus::Ok) {
            Gdiplus::Graphics graphics(dc);
            graphics.SetCompositingMode(
                Gdiplus::CompositingModeSourceOver);
            graphics.SetCompositingQuality(
                Gdiplus::CompositingQualityHighQuality);
            graphics.SetInterpolationMode(
                Gdiplus::InterpolationModeHighQualityBicubic);
            graphics.SetSmoothingMode(
                Gdiplus::SmoothingModeHighQuality);
            graphics.SetPixelOffsetMode(
                Gdiplus::PixelOffsetModeHighQuality);

            drawn =
                graphics.DrawImage(
                    &image,
                    Gdiplus::Rect(
                        127, 1,
                        16, 16)) ==
                Gdiplus::Ok;
        }
    }

    Gdiplus::GdiplusShutdown(token);
    stream->Release();
    return drawn;
}

void AddRoundedRectPath(
    Gdiplus::GraphicsPath &path,
    const Gdiplus::RectF &rect,
    Gdiplus::REAL radius)
{
    const Gdiplus::REAL diameter =
        std::max<Gdiplus::REAL>(
            0.0f,
            std::min<Gdiplus::REAL>(
                radius * 2.0f,
                std::min(rect.Width, rect.Height)));

    if (diameter <= 0.0f) {
        path.AddRectangle(rect);
        return;
    }

    const Gdiplus::REAL right =
        rect.X + rect.Width;
    const Gdiplus::REAL bottom =
        rect.Y + rect.Height;

    path.StartFigure();
    path.AddArc(
        rect.X,
        rect.Y,
        diameter,
        diameter,
        180.0f,
        90.0f);
    path.AddArc(
        right - diameter,
        rect.Y,
        diameter,
        diameter,
        270.0f,
        90.0f);
    path.AddArc(
        right - diameter,
        bottom - diameter,
        diameter,
        diameter,
        0.0f,
        90.0f);
    path.AddArc(
        rect.X,
        bottom - diameter,
        diameter,
        diameter,
        90.0f,
        90.0f);
    path.CloseFigure();
}

bool DrawAudioSourceIcons(HDC dc)
{
    if (!dc)
        return false;

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(
            &token,
            &startupInput,
            nullptr) != Gdiplus::Ok) {
        return false;
    }

    bool drawn = false;
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(
            Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(
            Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(
            Gdiplus::CompositingQualityHighQuality);

        Gdiplus::Pen pen(
            Gdiplus::Color(
                255,
                174,
                181,
                187),
            0.85f);
        pen.SetStartCap(
            Gdiplus::LineCapRound);
        pen.SetEndCap(
            Gdiplus::LineCapRound);
        pen.SetLineJoin(
            Gdiplus::LineJoinRound);

        // Exact geometry from ClatashaHudWindow::paintEvent().
        Gdiplus::GraphicsPath monitor;
        AddRoundedRectPath(
            monitor,
            Gdiplus::RectF(
                2.5f,
                37.5f,
                7.0f,
                5.0f),
            0.8f);
        graphics.DrawPath(
            &pen,
            &monitor);
        graphics.DrawLine(
            &pen,
            6.0f,
            42.5f,
            6.0f,
            44.5f);
        graphics.DrawLine(
            &pen,
            4.0f,
            44.5f,
            8.0f,
            44.5f);

        Gdiplus::GraphicsPath mic;
        AddRoundedRectPath(
            mic,
            Gdiplus::RectF(
                15.2f,
                37.2f,
                3.6f,
                5.8f),
            1.8f);
        graphics.DrawPath(
            &pen,
            &mic);

        // Qt's +180 degree span renders the lower pickup arc here.
        graphics.DrawArc(
            &pen,
            14.3f,
            39.6f,
            5.4f,
            4.8f,
            0.0f,
            180.0f);
        graphics.DrawLine(
            &pen,
            17.0f,
            44.3f,
            17.0f,
            46.0f);
        graphics.DrawLine(
            &pen,
            15.3f,
            46.0f,
            18.7f,
            46.0f);

        drawn = true;
    }

    Gdiplus::GdiplusShutdown(token);
    return drawn;
}

bool DrawPolishedHudChrome(HDC dc)
{
    if (!dc)
        return false;

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(
            &token,
            &startupInput,
            nullptr) != Gdiplus::Ok) {
        return false;
    }

    bool drawn = false;
    {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(
            Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(
            Gdiplus::PixelOffsetModeHighQuality);
        graphics.SetCompositingQuality(
            Gdiplus::CompositingQualityHighQuality);
        graphics.SetTextRenderingHint(
            Gdiplus::TextRenderingHintAntiAliasGridFit);

        Gdiplus::GraphicsPath panel;
        AddRoundedRectPath(
            panel,
            Gdiplus::RectF(
                0.5f,
                0.5f,
                static_cast<Gdiplus::REAL>(kHudWidth) - 1.0f,
                static_cast<Gdiplus::REAL>(kHudHeight) - 1.0f),
            4.0f);

        Gdiplus::SolidBrush panelBrush(
            Gdiplus::Color(
                238,
                8,
                10,
                12));
        Gdiplus::Pen borderPen(
            Gdiplus::Color(
                105,
                100,
                109,
                116),
            1.0f);
        graphics.FillPath(
            &panelBrush,
            &panel);
        graphics.DrawPath(
            &borderPen,
            &panel);

        // Exact renderer badge geometry/colors from the Qt HUD.
        Gdiplus::GraphicsPath badge;
        AddRoundedRectPath(
            badge,
            Gdiplus::RectF(
                102.0f,
                2.0f,
                23.0f,
                10.0f),
            2.0f);
        Gdiplus::SolidBrush badgeBrush(
            Gdiplus::Color(
                205,
                22,
                27,
                32));
        Gdiplus::Pen badgePen(
            Gdiplus::Color(
                170,
                96,
                106,
                116),
            0.8f);
        graphics.FillPath(
            &badgeBrush,
            &badge);
        graphics.DrawPath(
            &badgePen,
            &badge);

        Gdiplus::FontFamily family(
            L"Segoe UI");
        Gdiplus::Font rendererFont(
            &family,
            8.0f,
            Gdiplus::FontStyleBold,
            Gdiplus::UnitPixel);
        Gdiplus::SolidBrush rendererBrush(
            Gdiplus::Color(
                255,
                210,
                216,
                221));
        Gdiplus::StringFormat format;
        format.SetAlignment(
            Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(
            Gdiplus::StringAlignmentCenter);
        graphics.DrawString(
            L"OGL",
            -1,
            &rendererFont,
            Gdiplus::RectF(
                102.0f,
                2.0f,
                23.0f,
                10.0f),
            &format,
            &rendererBrush);

        drawn = true;
    }

    Gdiplus::GdiplusShutdown(token);
    return drawn;
}

void BlendRoundedRect(
    std::vector<std::uint8_t> &patch,
    int patchWidth,
    int patchHeight,
    double x,
    double y,
    double width,
    double height,
    double radius,
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue,
    std::uint8_t alpha)
{
    constexpr int samples = 4;
    constexpr double invSamples =
        1.0 / static_cast<double>(samples);
    constexpr double invSampleCount =
        1.0 / static_cast<double>(samples * samples);

    const int minX =
        std::max(0, static_cast<int>(std::floor(x - 1.0)));
    const int maxX =
        std::min(
            patchWidth - 1,
            static_cast<int>(std::ceil(x + width + 1.0)));
    const int minY =
        std::max(0, static_cast<int>(std::floor(y - 1.0)));
    const int maxY =
        std::min(
            patchHeight - 1,
            static_cast<int>(std::ceil(y + height + 1.0)));

    const double left = x;
    const double top = y;
    const double right = x + width;
    const double bottom = y + height;
    const double r =
        std::max(
            0.0,
            std::min(
                radius,
                std::min(width, height) * 0.5));

    const auto inside =
        [&](double px, double py) {
            if (px < left || px > right ||
                py < top || py > bottom) {
                return false;
            }
            if (r <= 0.0)
                return true;

            const double cx =
                std::clamp(
                    px,
                    left + r,
                    right - r);
            const double cy =
                std::clamp(
                    py,
                    top + r,
                    bottom - r);
            const double dx = px - cx;
            const double dy = py - cy;
            return dx * dx + dy * dy <= r * r;
        };

    for (int py = minY; py <= maxY; ++py) {
        for (int px = minX; px <= maxX; ++px) {
            int covered = 0;
            for (int sy = 0; sy < samples; ++sy) {
                for (int sx = 0; sx < samples; ++sx) {
                    const double sampleX =
                        px + (sx + 0.5) * invSamples;
                    const double sampleY =
                        py + (sy + 0.5) * invSamples;
                    if (inside(sampleX, sampleY))
                        ++covered;
                }
            }

            if (covered == 0)
                continue;

            const double coverage =
                covered * invSampleCount;
            const int effectiveAlpha =
                static_cast<int>(
                    std::lround(
                        static_cast<double>(alpha) *
                        coverage));
            if (effectiveAlpha <= 0)
                continue;

            const size_t index =
                (static_cast<size_t>(py) *
                     static_cast<size_t>(patchWidth) +
                 static_cast<size_t>(px)) *
                4;
            const int inverse =
                255 - effectiveAlpha;

            patch[index + 0] =
                static_cast<std::uint8_t>(
                    (red * effectiveAlpha +
                     patch[index + 0] * inverse) /
                    255);
            patch[index + 1] =
                static_cast<std::uint8_t>(
                    (green * effectiveAlpha +
                     patch[index + 1] * inverse) /
                    255);
            patch[index + 2] =
                static_cast<std::uint8_t>(
                    (blue * effectiveAlpha +
                     patch[index + 2] * inverse) /
                    255);
            patch[index + 3] =
                std::max<std::uint8_t>(
                    patch[index + 3],
                    static_cast<std::uint8_t>(
                        effectiveAlpha));
        }
    }
}

bool BuildStaticHudPixels()
{
    if (!g_hudPixels.empty())
        return true;

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc)
        return false;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = kHudWidth;
    info.bmiHeader.biHeight = -kHudHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(
        dc,
        &info,
        DIB_RGB_COLORS,
        &bits,
        nullptr,
        0);
    if (!bitmap || !bits) {
        if (bitmap)
            DeleteObject(bitmap);
        DeleteDC(dc);
        return false;
    }

    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    ZeroMemory(
        bits,
        static_cast<SIZE_T>(kHudWidth) *
            static_cast<SIZE_T>(kHudHeight) * 4);

    HGDIOBJ oldPen =
        SelectObject(
            dc,
            GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush =
        SelectObject(
            dc,
            GetStockObject(NULL_BRUSH));

    if (!DrawPolishedHudChrome(dc)) {
        HPEN fallbackBorder =
            CreatePen(
                PS_SOLID,
                1,
                RGB(100, 109, 116));
        HBRUSH fallbackPanel =
            CreateSolidBrush(
                RGB(8, 10, 12));
        SelectObject(dc, fallbackBorder);
        SelectObject(dc, fallbackPanel);
        RoundRect(
            dc,
            0,
            0,
            kHudWidth,
            kHudHeight,
            8,
            8);
        DeleteObject(fallbackBorder);
        DeleteObject(fallbackPanel);
    }

    SetBkMode(dc, TRANSPARENT);

    HFONT fpsFont = CreateFontW(
        -32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT smallBold = CreateFontW(
        -13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT tinyFont = CreateFontW(
        -11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    HFONT oldFont =
        static_cast<HFONT>(SelectObject(dc, fpsFont));
    SetTextColor(dc, RGB(45, 143, 255));

    // Leave the FPS rectangle clean in the base texture. Live FPS patches are
    // pre-rendered below during this same one-time setup and uploaded later.
    SelectObject(dc, smallBold);
    SetTextColor(dc, RGB(165, 171, 176));
    // Leave the OBS FPS rectangle clean. Its /NN value is pre-rendered into
    // cached patches below and updated independently from game FPS.

    HBRUSH badgeBrush = nullptr;
    HPEN badgePen = nullptr;

    SelectObject(dc, tinyFont);
    SetTextColor(dc, RGB(205, 209, 212));
    // Leave the timer rectangle clean. Timer glyphs are cached once below.

    HBRUSH dotBrush =
        CreateSolidBrush(RGB(158, 164, 169));
    SelectObject(dc, dotBrush);
    SelectObject(dc, GetStockObject(NULL_PEN));
    // Activity dots/ring and disk text are dynamic in fullscreen mode.

    SetTextColor(dc, RGB(165, 171, 176));

    HPEN iconPen = nullptr;
    if (!DrawAudioSourceIcons(dc)) {
        // Conservative fallback if GDI+ initialization fails.
        iconPen =
            CreatePen(
                PS_SOLID,
                1,
                RGB(174, 181, 187));
        SelectObject(dc, iconPen);
        SelectObject(
            dc,
            GetStockObject(NULL_BRUSH));
        Rectangle(dc, 3, 38, 10, 43);
        MoveToEx(dc, 6, 43, nullptr);
        LineTo(dc, 6, 46);
        MoveToEx(dc, 4, 46, nullptr);
        LineTo(dc, 9, 46);

        RoundRect(dc, 15, 37, 19, 43, 3, 3);
        Arc(dc, 14, 39, 20, 45, 14, 41, 20, 41);
        MoveToEx(dc, 17, 44, nullptr);
        LineTo(dc, 17, 47);
        MoveToEx(dc, 15, 46, nullptr);
        LineTo(dc, 19, 46);
    }

    HBRUSH logoBrush = nullptr;
    if (!DrawEmbeddedLogo(dc)) {
        logoBrush =
            CreateSolidBrush(
                RGB(240, 243, 245));
        SelectObject(dc, logoBrush);
        SelectObject(
            dc,
            GetStockObject(NULL_PEN));
        Ellipse(dc, 128, 2, 143, 17);

        SelectObject(dc, smallBold);
        SetTextColor(
            dc,
            RGB(18, 22, 26));
        RECT logoRect{128, 1, 143, 18};
        DrawTextW(
            dc, L"C", -1, &logoRect,
            DT_CENTER |
                DT_VCENTER |
                DT_SINGLELINE);
    }

    const auto *source =
        static_cast<const std::uint8_t *>(bits);
    g_hudPixels.resize(
        static_cast<size_t>(kHudWidth) *
        static_cast<size_t>(kHudHeight) * 4);

    for (int y = 0; y < kHudHeight; ++y) {
        for (int x = 0; x < kHudWidth; ++x) {
            const size_t index =
                (static_cast<size_t>(y) * kHudWidth + x) * 4;
            const std::uint8_t b = source[index + 0];
            const std::uint8_t g = source[index + 1];
            const std::uint8_t r = source[index + 2];

            g_hudPixels[index + 0] = r;
            g_hudPixels[index + 1] = g;
            g_hudPixels[index + 2] = b;

            const std::uint8_t sourceAlpha =
                source[index + 3];
            if (sourceAlpha != 0) {
                g_hudPixels[index + 3] =
                    sourceAlpha;
            } else if (r == 0 && g == 0 && b == 0) {
                g_hudPixels[index + 3] = 0;
            } else if (r <= 14 && g <= 16 && b <= 18) {
                g_hudPixels[index + 3] = 238;
            } else {
                g_hudPixels[index + 3] = 255;
            }
        }
    }

    // Pre-render 0..999 into tiny CPU-side RGBA patches. No GDI work happens
    // during gameplay; the render thread only uploads one cached 53x34 patch
    // when the received integer FPS changes.
    HDC fpsDc = CreateCompatibleDC(nullptr);
    if (!fpsDc) {
        SelectObject(dc, oldFont);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldBitmap);
        if (iconPen)
            DeleteObject(iconPen);
        DeleteObject(dotBrush);
        DeleteObject(logoBrush);
        if (badgePen)
            DeleteObject(badgePen);
        if (badgeBrush)
            DeleteObject(badgeBrush);
        DeleteObject(tinyFont);
        DeleteObject(smallBold);
        DeleteObject(fpsFont);
                          DeleteObject(bitmap);
        DeleteDC(dc);
        return false;
    }

    BITMAPINFO fpsInfo{};
    fpsInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    fpsInfo.bmiHeader.biWidth = kFpsPatchWidth;
    fpsInfo.bmiHeader.biHeight = -kFpsPatchHeight;
    fpsInfo.bmiHeader.biPlanes = 1;
    fpsInfo.bmiHeader.biBitCount = 32;
    fpsInfo.bmiHeader.biCompression = BI_RGB;

    void *fpsBits = nullptr;
    HBITMAP fpsBitmap = CreateDIBSection(
        fpsDc,
        &fpsInfo,
        DIB_RGB_COLORS,
        &fpsBits,
        nullptr,
        0);
    if (!fpsBitmap || !fpsBits) {
        if (fpsBitmap)
            DeleteObject(fpsBitmap);
        DeleteDC(fpsDc);
        SelectObject(dc, oldFont);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldBitmap);
        if (iconPen)
            DeleteObject(iconPen);
        DeleteObject(dotBrush);
        DeleteObject(logoBrush);
        if (badgePen)
            DeleteObject(badgePen);
        if (badgeBrush)
            DeleteObject(badgeBrush);
        DeleteObject(tinyFont);
        DeleteObject(smallBold);
        DeleteObject(fpsFont);
                          DeleteObject(bitmap);
        DeleteDC(dc);
        return false;
    }

    HGDIOBJ fpsOldBitmap = SelectObject(fpsDc, fpsBitmap);
    HFONT fpsOldFont =
        static_cast<HFONT>(SelectObject(fpsDc, fpsFont));
    SetBkMode(fpsDc, TRANSPARENT);
    SetTextColor(fpsDc, RGB(45, 143, 255));

    std::vector<std::uint8_t> baseFpsPatch(
        static_cast<size_t>(kFpsPatchWidth) *
        static_cast<size_t>(kFpsPatchHeight) * 4);

    for (int y = 0; y < kFpsPatchHeight; ++y) {
        for (int x = 0; x < kFpsPatchWidth; ++x) {
            const size_t srcIndex =
                (static_cast<size_t>(y + kFpsPatchY) * kHudWidth +
                 static_cast<size_t>(x + kFpsPatchX)) * 4;
            const size_t dstIndex =
                (static_cast<size_t>(y) * kFpsPatchWidth +
                 static_cast<size_t>(x)) * 4;
            baseFpsPatch[dstIndex + 0] = g_hudPixels[srcIndex + 0];
            baseFpsPatch[dstIndex + 1] = g_hudPixels[srcIndex + 1];
            baseFpsPatch[dstIndex + 2] = g_hudPixels[srcIndex + 2];
            baseFpsPatch[dstIndex + 3] = g_hudPixels[srcIndex + 3];
        }
    }

    g_liveFpsPatches.clear();
    g_liveFpsPatches.resize(kMaxCachedFps + 1);

    auto *fpsSource =
        static_cast<std::uint8_t *>(fpsBits);

    for (int fpsValue = 0; fpsValue <= kMaxCachedFps; ++fpsValue) {
        for (int y = 0; y < kFpsPatchHeight; ++y) {
            for (int x = 0; x < kFpsPatchWidth; ++x) {
                const size_t index =
                    (static_cast<size_t>(y) * kFpsPatchWidth +
                     static_cast<size_t>(x)) * 4;
                fpsSource[index + 0] = baseFpsPatch[index + 2];
                fpsSource[index + 1] = baseFpsPatch[index + 1];
                fpsSource[index + 2] = baseFpsPatch[index + 0];
                fpsSource[index + 3] = baseFpsPatch[index + 3];
            }
        }

        wchar_t fpsText[16] = {};
        swprintf_s(fpsText, L"%d", fpsValue);
        RECT localFpsRect{0, -3, 52, 33};
        DrawTextW(
            fpsDc,
            fpsText,
            -1,
            &localFpsRect,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        auto &patch = g_liveFpsPatches[fpsValue];
        patch.resize(
            static_cast<size_t>(kFpsPatchWidth) *
            static_cast<size_t>(kFpsPatchHeight) * 4);

        for (int y = 0; y < kFpsPatchHeight; ++y) {
            for (int x = 0; x < kFpsPatchWidth; ++x) {
                const size_t index =
                    (static_cast<size_t>(y) * kFpsPatchWidth +
                     static_cast<size_t>(x)) * 4;
                const std::uint8_t b = fpsSource[index + 0];
                const std::uint8_t g = fpsSource[index + 1];
                const std::uint8_t r = fpsSource[index + 2];

                patch[index + 0] = r;
                patch[index + 1] = g;
                patch[index + 2] = b;

                const bool changed =
                    r != baseFpsPatch[index + 0] ||
                    g != baseFpsPatch[index + 1] ||
                    b != baseFpsPatch[index + 2];
                patch[index + 3] =
                    changed ? 255 : baseFpsPatch[index + 3];
            }
        }
    }

    // Reuse the same one-time GDI surface to pre-render /OBS-FPS patches.
    SelectObject(fpsDc, smallBold);
    SetTextColor(fpsDc, RGB(165, 171, 176));

    std::vector<std::uint8_t> baseObsFpsPatch(
        static_cast<size_t>(kObsFpsPatchWidth) *
        static_cast<size_t>(kObsFpsPatchHeight) * 4);

    for (int y = 0; y < kObsFpsPatchHeight; ++y) {
        for (int x = 0; x < kObsFpsPatchWidth; ++x) {
            const size_t srcIndex =
                (static_cast<size_t>(y + kObsFpsPatchY) * kHudWidth +
                 static_cast<size_t>(x + kObsFpsPatchX)) * 4;
            const size_t dstIndex =
                (static_cast<size_t>(y) * kObsFpsPatchWidth +
                 static_cast<size_t>(x)) * 4;
            baseObsFpsPatch[dstIndex + 0] = g_hudPixels[srcIndex + 0];
            baseObsFpsPatch[dstIndex + 1] = g_hudPixels[srcIndex + 1];
            baseObsFpsPatch[dstIndex + 2] = g_hudPixels[srcIndex + 2];
            baseObsFpsPatch[dstIndex + 3] = g_hudPixels[srcIndex + 3];
        }
    }

    g_liveObsFpsPatches.clear();
    g_liveObsFpsPatches.resize(kMaxCachedFps + 1);

    for (int obsFpsValue = 0; obsFpsValue <= kMaxCachedFps; ++obsFpsValue) {
        for (int y = 0; y < kObsFpsPatchHeight; ++y) {
            for (int x = 0; x < kObsFpsPatchWidth; ++x) {
                const size_t srcIndex =
                    (static_cast<size_t>(y) * kObsFpsPatchWidth +
                     static_cast<size_t>(x)) * 4;
                const size_t dstIndex =
                    (static_cast<size_t>(y) * kFpsPatchWidth +
                     static_cast<size_t>(x)) * 4;
                fpsSource[dstIndex + 0] = baseObsFpsPatch[srcIndex + 2];
                fpsSource[dstIndex + 1] = baseObsFpsPatch[srcIndex + 1];
                fpsSource[dstIndex + 2] = baseObsFpsPatch[srcIndex + 0];
                fpsSource[dstIndex + 3] = baseObsFpsPatch[srcIndex + 3];
            }
        }

        wchar_t obsFpsText[16] = {};
        swprintf_s(obsFpsText, L"/%d", obsFpsValue);
        RECT localObsFpsRect{
            0, 1,
            kObsFpsPatchWidth,
            24};
        DrawTextW(
            fpsDc,
            obsFpsText,
            -1,
            &localObsFpsRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        auto &patch = g_liveObsFpsPatches[obsFpsValue];
        patch.resize(
            static_cast<size_t>(kObsFpsPatchWidth) *
            static_cast<size_t>(kObsFpsPatchHeight) * 4);

        for (int y = 0; y < kObsFpsPatchHeight; ++y) {
            for (int x = 0; x < kObsFpsPatchWidth; ++x) {
                const size_t srcIndex =
                    (static_cast<size_t>(y) * kFpsPatchWidth +
                     static_cast<size_t>(x)) * 4;
                const size_t dstIndex =
                    (static_cast<size_t>(y) * kObsFpsPatchWidth +
                     static_cast<size_t>(x)) * 4;
                const std::uint8_t b = fpsSource[srcIndex + 0];
                const std::uint8_t g = fpsSource[srcIndex + 1];
                const std::uint8_t r = fpsSource[srcIndex + 2];

                patch[dstIndex + 0] = r;
                patch[dstIndex + 1] = g;
                patch[dstIndex + 2] = b;

                const bool changed =
                    r != baseObsFpsPatch[dstIndex + 0] ||
                    g != baseObsFpsPatch[dstIndex + 1] ||
                    b != baseObsFpsPatch[dstIndex + 2];
                patch[dstIndex + 3] =
                    changed ? 255 : baseObsFpsPatch[dstIndex + 3];
            }
        }
    }

    // Cache tiny timer glyphs (0-9 and colon) while GDI is already active.
    // Runtime timer updates only compose these cached pixels.
    SelectObject(fpsDc, tinyFont);
    SetBkMode(fpsDc, TRANSPARENT);
    SetTextColor(fpsDc, RGB(205, 209, 212));

    g_timerBasePatch.resize(
        static_cast<size_t>(kTimerPatchWidth) *
        static_cast<size_t>(kTimerPatchHeight) * 4);
    for (int y = 0; y < kTimerPatchHeight; ++y) {
        for (int x = 0; x < kTimerPatchWidth; ++x) {
            const size_t srcIndex =
                (static_cast<size_t>(y + kTimerPatchY) * kHudWidth +
                 static_cast<size_t>(x + kTimerPatchX)) * 4;
            const size_t dstIndex =
                (static_cast<size_t>(y) * kTimerPatchWidth +
                 static_cast<size_t>(x)) * 4;
            g_timerBasePatch[dstIndex + 0] = g_hudPixels[srcIndex + 0];
            g_timerBasePatch[dstIndex + 1] = g_hudPixels[srcIndex + 1];
            g_timerBasePatch[dstIndex + 2] = g_hudPixels[srcIndex + 2];
            g_timerBasePatch[dstIndex + 3] = g_hudPixels[srcIndex + 3];
        }
    }

    g_timerGlyphs.clear();
    g_timerGlyphs.resize(kTimerGlyphCount);

    for (int glyphIndex = 0; glyphIndex < kTimerGlyphCount; ++glyphIndex) {
        ZeroMemory(
            fpsBits,
            static_cast<SIZE_T>(kFpsPatchWidth) *
                static_cast<SIZE_T>(kFpsPatchHeight) * 4);

        wchar_t glyphText[2] = {
            glyphIndex < 10
                ? static_cast<wchar_t>(L'0' + glyphIndex)
                : L':',
            L'\0'};
        RECT glyphRect{
            0, 0,
            kTimerGlyphWidth,
            kTimerGlyphHeight};
        DrawTextW(
            fpsDc,
            glyphText,
            1,
            &glyphRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        auto &glyph = g_timerGlyphs[glyphIndex];
        glyph.resize(
            static_cast<size_t>(kTimerGlyphWidth) *
            static_cast<size_t>(kTimerGlyphHeight) * 4);

        for (int y = 0; y < kTimerGlyphHeight; ++y) {
            for (int x = 0; x < kTimerGlyphWidth; ++x) {
                const size_t srcIndex =
                    (static_cast<size_t>(y) * kFpsPatchWidth +
                     static_cast<size_t>(x)) * 4;
                const size_t dstIndex =
                    (static_cast<size_t>(y) * kTimerGlyphWidth +
                     static_cast<size_t>(x)) * 4;
                const std::uint8_t b = fpsSource[srcIndex + 0];
                const std::uint8_t g = fpsSource[srcIndex + 1];
                const std::uint8_t r = fpsSource[srcIndex + 2];
                const int coverageSource =
                    std::max<int>(r, std::max<int>(g, b));
                const std::uint8_t alpha =
                    static_cast<std::uint8_t>(
                        std::min(
                            255,
                            coverageSource * 255 / 212));

                glyph[dstIndex + 0] = 205;
                glyph[dstIndex + 1] = 209;
                glyph[dstIndex + 2] = 212;
                glyph[dstIndex + 3] = alpha;
            }
        }
    }

    g_timerScratch = g_timerBasePatch;

    g_audioBasePatch.resize(
        static_cast<size_t>(kAudioPatchWidth) *
        static_cast<size_t>(kAudioPatchHeight) * 4);
    for (int y = 0; y < kAudioPatchHeight; ++y) {
        for (int x = 0; x < kAudioPatchWidth; ++x) {
            const size_t srcIndex =
                (static_cast<size_t>(y + kAudioPatchY) * kHudWidth +
                 static_cast<size_t>(x + kAudioPatchX)) * 4;
            const size_t dstIndex =
                (static_cast<size_t>(y) * kAudioPatchWidth +
                 static_cast<size_t>(x)) * 4;
            g_audioBasePatch[dstIndex + 0] = g_hudPixels[srcIndex + 0];
            g_audioBasePatch[dstIndex + 1] = g_hudPixels[srcIndex + 1];
            g_audioBasePatch[dstIndex + 2] = g_hudPixels[srcIndex + 2];
            g_audioBasePatch[dstIndex + 3] = g_hudPixels[srcIndex + 3];
        }
    }
    g_audioScratch = g_audioBasePatch;

    g_audioLevelPatches.clear();
    g_audioLevelPatches.resize(
        (kAudioSegments + 1) *
        (kAudioSegments + 1));

    for (int desktopActive = 0;
         desktopActive <= kAudioSegments;
         ++desktopActive) {
        for (int micActive = 0;
             micActive <= kAudioSegments;
             ++micActive) {
            auto &patch =
                g_audioLevelPatches[
                    static_cast<size_t>(
                        desktopActive *
                            (kAudioSegments + 1) +
                        micActive)];
            patch = g_audioBasePatch;

            const auto drawMeter =
                [&](double meterX,
                    int activeSegments) {
                    for (int i = 0;
                         i < kAudioSegments;
                         ++i) {
                        const double segmentY =
                            25.0 -
                            i *
                                (kAudioSegmentHeight +
                                 kAudioSegmentGap);
                        const bool active =
                            i < activeSegments;
                        BlendRoundedRect(
                            patch,
                            kAudioPatchWidth,
                            kAudioPatchHeight,
                            meterX,
                            segmentY,
                            3.0,
                            4.0,
                            0.8,
                            active ? 31 : 49,
                            active ? 218 : 55,
                            active ? 102 : 59,
                            active ? 255 : 205);
                    }
                };

            drawMeter(1.0, desktopActive);
            drawMeter(13.0, micActive);
        }
    }

    if (!g_audioLevelPatches.empty()) {
        const auto &inactive =
            g_audioLevelPatches.front();
        for (int y = 0;
             y < kAudioPatchHeight;
             ++y) {
            for (int x = 0;
                 x < kAudioPatchWidth;
                 ++x) {
                const size_t srcIndex =
                    (static_cast<size_t>(y) *
                         kAudioPatchWidth +
                     static_cast<size_t>(x)) *
                    4;
                const size_t dstIndex =
                    (static_cast<size_t>(
                         y + kAudioPatchY) *
                         kHudWidth +
                     static_cast<size_t>(
                         x + kAudioPatchX)) *
                    4;
                g_hudPixels[dstIndex + 0] =
                    inactive[srcIndex + 0];
                g_hudPixels[dstIndex + 1] =
                    inactive[srcIndex + 1];
                g_hudPixels[dstIndex + 2] =
                    inactive[srcIndex + 2];
                g_hudPixels[dstIndex + 3] =
                    inactive[srcIndex + 3];
            }
        }
    }

    g_diskBasePatch.resize(
        static_cast<size_t>(kDiskPatchWidth) *
        static_cast<size_t>(kDiskPatchHeight) * 4);
    for (int y = 0; y < kDiskPatchHeight; ++y) {
        for (int x = 0; x < kDiskPatchWidth; ++x) {
            const size_t srcIndex =
                (static_cast<size_t>(y + kDiskPatchY) * kHudWidth +
                 static_cast<size_t>(x + kDiskPatchX)) * 4;
            const size_t dstIndex =
                (static_cast<size_t>(y) * kDiskPatchWidth +
                 static_cast<size_t>(x)) * 4;
            g_diskBasePatch[dstIndex + 0] = g_hudPixels[srcIndex + 0];
            g_diskBasePatch[dstIndex + 1] = g_hudPixels[srcIndex + 1];
            g_diskBasePatch[dstIndex + 2] = g_hudPixels[srcIndex + 2];
            g_diskBasePatch[dstIndex + 3] = g_hudPixels[srcIndex + 3];
        }
    }
    g_diskScratch = g_diskBasePatch;

    SelectObject(fpsDc, tinyFont);
    SetBkMode(fpsDc, TRANSPARENT);
    SetTextColor(fpsDc, RGB(165, 171, 176));
    g_diskGlyphs.clear();
    g_diskGlyphs.resize(kDiskGlyphCount);

    const wchar_t diskChars[kDiskGlyphCount] = {
        L'0', L'1', L'2', L'3', L'4',
        L'5', L'6', L'7', L'8', L'9',
        L'.', L' ', L'G', L'B'};

    for (int glyphIndex = 0;
         glyphIndex < kDiskGlyphCount;
         ++glyphIndex) {
        ZeroMemory(
            fpsBits,
            static_cast<SIZE_T>(kFpsPatchWidth) *
                static_cast<SIZE_T>(kFpsPatchHeight) * 4);

        RECT glyphRect{
            0, 0,
            kDiskGlyphWidth,
            kDiskGlyphHeight};
        DrawTextW(
            fpsDc,
            &diskChars[glyphIndex],
            1,
            &glyphRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        auto &glyph =
            g_diskGlyphs[static_cast<size_t>(glyphIndex)];
        glyph.resize(
            static_cast<size_t>(kDiskGlyphWidth) *
            static_cast<size_t>(kDiskGlyphHeight) * 4);

        for (int y = 0; y < kDiskGlyphHeight; ++y) {
            for (int x = 0; x < kDiskGlyphWidth; ++x) {
                const size_t srcIndex =
                    (static_cast<size_t>(y) * kFpsPatchWidth +
                     static_cast<size_t>(x)) * 4;
                const size_t dstIndex =
                    (static_cast<size_t>(y) * kDiskGlyphWidth +
                     static_cast<size_t>(x)) * 4;
                const std::uint8_t b = fpsSource[srcIndex + 0];
                const std::uint8_t g = fpsSource[srcIndex + 1];
                const std::uint8_t r = fpsSource[srcIndex + 2];
                const int coverage =
                    std::max<int>(r, std::max<int>(g, b));
                const std::uint8_t alpha =
                    static_cast<std::uint8_t>(
                        std::min(255, coverage * 255 / 176));

                glyph[dstIndex + 0] = 165;
                glyph[dstIndex + 1] = 171;
                glyph[dstIndex + 2] = 176;
                glyph[dstIndex + 3] = alpha;
            }
        }
    }

    g_stateBasePatch.resize(
        static_cast<size_t>(kStatePatchWidth) *
        static_cast<size_t>(kStatePatchHeight) * 4);
    for (int y = 0; y < kStatePatchHeight; ++y) {
        for (int x = 0; x < kStatePatchWidth; ++x) {
            const size_t srcIndex =
                (static_cast<size_t>(y + kStatePatchY) * kHudWidth +
                 static_cast<size_t>(x + kStatePatchX)) * 4;
            const size_t dstIndex =
                (static_cast<size_t>(y) * kStatePatchWidth +
                 static_cast<size_t>(x)) * 4;
            g_stateBasePatch[dstIndex + 0] = g_hudPixels[srcIndex + 0];
            g_stateBasePatch[dstIndex + 1] = g_hudPixels[srcIndex + 1];
            g_stateBasePatch[dstIndex + 2] = g_hudPixels[srcIndex + 2];
            g_stateBasePatch[dstIndex + 3] = g_hudPixels[srcIndex + 3];
        }
    }
    g_stateScratch = g_stateBasePatch;
    g_liveFpsScratch.resize(
        static_cast<size_t>(kFpsPatchWidth) *
        static_cast<size_t>(kFpsPatchHeight) * 4);

    SelectObject(fpsDc, fpsOldFont);
    SelectObject(fpsDc, fpsOldBitmap);
    DeleteObject(fpsBitmap);
    DeleteDC(fpsDc);

    SelectObject(dc, oldFont);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldBitmap);

    if (iconPen)
        DeleteObject(iconPen);
    DeleteObject(dotBrush);
    if (logoBrush)
        DeleteObject(logoBrush);
    if (badgePen)
        DeleteObject(badgePen);
    if (badgeBrush)
        DeleteObject(badgeBrush);
    DeleteObject(tinyFont);
    DeleteObject(smallBold);
    DeleteObject(fpsFont);
              DeleteObject(bitmap);
    DeleteDC(dc);

    return true;
}

using GlSizePtr = std::ptrdiff_t;

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_ARRAY_BUFFER_BINDING
#define GL_ARRAY_BUFFER_BINDING 0x8894
#endif
#ifndef GL_STREAM_DRAW
#define GL_STREAM_DRAW 0x88E0
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

using GlCreateShaderFn = GLuint (APIENTRYP)(GLenum);
using GlShaderSourceFn = void (APIENTRYP)(
    GLuint, GLsizei, const char *const *, const GLint *);
using GlCompileShaderFn = void (APIENTRYP)(GLuint);
using GlGetShaderivFn = void (APIENTRYP)(GLuint, GLenum, GLint *);
using GlDeleteShaderFn = void (APIENTRYP)(GLuint);
using GlCreateProgramFn = GLuint (APIENTRYP)();
using GlAttachShaderFn = void (APIENTRYP)(GLuint, GLuint);
using GlBindAttribLocationFn = void (APIENTRYP)(GLuint, GLuint, const char *);
using GlBindFragDataLocationFn = void (APIENTRYP)(GLuint, GLuint, const char *);
using GlLinkProgramFn = void (APIENTRYP)(GLuint);
using GlGetProgramivFn = void (APIENTRYP)(GLuint, GLenum, GLint *);
using GlDeleteProgramFn = void (APIENTRYP)(GLuint);
using GlUseProgramFn = void (APIENTRYP)(GLuint);
using GlGetUniformLocationFn = GLint (APIENTRYP)(GLuint, const char *);
using GlUniform1iFn = void (APIENTRYP)(GLint, GLint);
using GlUniform1fFn = void (APIENTRYP)(GLint, GLfloat);
using GlGenBuffersFn = void (APIENTRYP)(GLsizei, GLuint *);
using GlBindBufferFn = void (APIENTRYP)(GLenum, GLuint);
using GlBufferDataFn = void (APIENTRYP)(
    GLenum, GlSizePtr, const void *, GLenum);
using GlGenVertexArraysFn = void (APIENTRYP)(GLsizei, GLuint *);
using GlBindVertexArrayFn = void (APIENTRYP)(GLuint);
using GlEnableVertexAttribArrayFn = void (APIENTRYP)(GLuint);
using GlVertexAttribPointerFn = void (APIENTRYP)(
    GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
using GlActiveTextureFn = void (APIENTRYP)(GLenum);

GlCreateShaderFn g_glCreateShader = nullptr;
GlShaderSourceFn g_glShaderSource = nullptr;
GlCompileShaderFn g_glCompileShader = nullptr;
GlGetShaderivFn g_glGetShaderiv = nullptr;
GlDeleteShaderFn g_glDeleteShader = nullptr;
GlCreateProgramFn g_glCreateProgram = nullptr;
GlAttachShaderFn g_glAttachShader = nullptr;
GlBindAttribLocationFn g_glBindAttribLocation = nullptr;
GlBindFragDataLocationFn g_glBindFragDataLocation = nullptr;
GlLinkProgramFn g_glLinkProgram = nullptr;
GlGetProgramivFn g_glGetProgramiv = nullptr;
GlDeleteProgramFn g_glDeleteProgram = nullptr;
GlUseProgramFn g_glUseProgram = nullptr;
GlGetUniformLocationFn g_glGetUniformLocation = nullptr;
GlUniform1iFn g_glUniform1i = nullptr;
GlUniform1fFn g_glUniform1f = nullptr;
GlGenBuffersFn g_glGenBuffers = nullptr;
GlBindBufferFn g_glBindBuffer = nullptr;
GlBufferDataFn g_glBufferData = nullptr;
GlGenVertexArraysFn g_glGenVertexArrays = nullptr;
GlBindVertexArrayFn g_glBindVertexArray = nullptr;
GlEnableVertexAttribArrayFn g_glEnableVertexAttribArray = nullptr;
GlVertexAttribPointerFn g_glVertexAttribPointer = nullptr;
GlActiveTextureFn g_glActiveTexture = nullptr;

GLuint g_hudProgram = 0;
GLuint g_hudVao = 0;
GLuint g_hudVbo = 0;
GLint g_hudSampler = -1;
GLint g_hudOpacity = -1;
HGLRC g_modernContext = nullptr;
bool g_modernLoadFailed = false;

void *LoadGlProc(const char *name)
{
    void *proc =
        reinterpret_cast<void *>(wglGetProcAddress(name));

    if (!proc ||
        proc == reinterpret_cast<void *>(1) ||
        proc == reinterpret_cast<void *>(2) ||
        proc == reinterpret_cast<void *>(3) ||
        proc == reinterpret_cast<void *>(-1)) {
        HMODULE gl = GetModuleHandleW(L"opengl32.dll");
        proc = gl
            ? reinterpret_cast<void *>(
                  GetProcAddress(gl, name))
            : nullptr;
    }

    return proc;
}

template<typename T>
bool LoadProc(T &target, const char *name)
{
    target = reinterpret_cast<T>(LoadGlProc(name));
    return target != nullptr;
}

bool LoadModernGl()
{
    return
        LoadProc(g_glCreateShader, "glCreateShader") &&
        LoadProc(g_glShaderSource, "glShaderSource") &&
        LoadProc(g_glCompileShader, "glCompileShader") &&
        LoadProc(g_glGetShaderiv, "glGetShaderiv") &&
        LoadProc(g_glDeleteShader, "glDeleteShader") &&
        LoadProc(g_glCreateProgram, "glCreateProgram") &&
        LoadProc(g_glAttachShader, "glAttachShader") &&
        LoadProc(g_glBindAttribLocation, "glBindAttribLocation") &&
        LoadProc(g_glBindFragDataLocation, "glBindFragDataLocation") &&
        LoadProc(g_glLinkProgram, "glLinkProgram") &&
        LoadProc(g_glGetProgramiv, "glGetProgramiv") &&
        LoadProc(g_glDeleteProgram, "glDeleteProgram") &&
        LoadProc(g_glUseProgram, "glUseProgram") &&
        LoadProc(g_glGetUniformLocation, "glGetUniformLocation") &&
        LoadProc(g_glUniform1i, "glUniform1i") &&
        LoadProc(g_glUniform1f, "glUniform1f") &&
        LoadProc(g_glGenBuffers, "glGenBuffers") &&
        LoadProc(g_glBindBuffer, "glBindBuffer") &&
        LoadProc(g_glBufferData, "glBufferData") &&
        LoadProc(g_glGenVertexArrays, "glGenVertexArrays") &&
        LoadProc(g_glBindVertexArray, "glBindVertexArray") &&
        LoadProc(g_glEnableVertexAttribArray, "glEnableVertexAttribArray") &&
        LoadProc(g_glVertexAttribPointer, "glVertexAttribPointer") &&
        LoadProc(g_glActiveTexture, "glActiveTexture");
}

GLuint CompileShader(GLenum type, const char *source)
{
    GLuint shader = g_glCreateShader(type);
    if (!shader)
        return 0;

    g_glShaderSource(shader, 1, &source, nullptr);
    g_glCompileShader(shader);

    GLint compiled = GL_FALSE;
    g_glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        g_glDeleteShader(shader);
        return 0;
    }

    return shader;
}

bool CreateHudProgram()
{
    static const char *vertexSource =
        "#version 150 core\n"
        "in vec2 aPos;\n"
        "in vec2 aUv;\n"
        "out vec2 vUv;\n"
        "void main() {\n"
        "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "  vUv = aUv;\n"
        "}\n";

    static const char *fragmentSource =
        "#version 150 core\n"
        "uniform sampler2D uTex;\n"
        "uniform float uOpacity;\n"
        "in vec2 vUv;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "  vec4 sampled = texture(uTex, vUv);\n"
        "  fragColor = vec4(sampled.rgb, sampled.a * uOpacity);\n"
        "}\n";

    const GLuint vertex =
        CompileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment =
        CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vertex || !fragment) {
        if (vertex)
            g_glDeleteShader(vertex);
        if (fragment)
            g_glDeleteShader(fragment);
        return false;
    }

    g_hudProgram = g_glCreateProgram();
    if (!g_hudProgram) {
        g_glDeleteShader(vertex);
        g_glDeleteShader(fragment);
        return false;
    }

    g_glAttachShader(g_hudProgram, vertex);
    g_glAttachShader(g_hudProgram, fragment);
    g_glBindAttribLocation(g_hudProgram, 0, "aPos");
    g_glBindAttribLocation(g_hudProgram, 1, "aUv");
    g_glBindFragDataLocation(g_hudProgram, 0, "fragColor");
    g_glLinkProgram(g_hudProgram);

    g_glDeleteShader(vertex);
    g_glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    g_glGetProgramiv(g_hudProgram, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        g_glDeleteProgram(g_hudProgram);
        g_hudProgram = 0;
        return false;
    }

    g_hudSampler =
        g_glGetUniformLocation(g_hudProgram, "uTex");
    g_hudOpacity =
        g_glGetUniformLocation(g_hudProgram, "uOpacity");
    return g_hudSampler >= 0 &&
           g_hudOpacity >= 0;
}

bool CreateHudTexture()
{
    if (!BuildStaticHudPixels())
        return false;

    GLint oldTexture = 0;
    GLint oldUnpackAlignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);

    glGenTextures(1, &g_hudTexture);
    if (!g_hudTexture)
        return false;

    glBindTexture(GL_TEXTURE_2D, g_hudTexture);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,
        kHudWidth,
        kHudHeight,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        g_hudPixels.data());

    glPixelStorei(
        GL_UNPACK_ALIGNMENT,
        oldUnpackAlignment);
    glBindTexture(
        GL_TEXTURE_2D,
        static_cast<GLuint>(oldTexture));

    g_lastRenderedLiveFps = -1;
    g_lastRenderedLiveObsFps = -1;
    g_lastRenderedSessionSeconds = -1;
    g_lastRenderedAudioLevels = -1;
    g_lastRenderedHudStatus = -1;
    g_lastRenderedDiskTenths = -1;
    g_lastRenderedSessionActive = false;
    g_lastStatusAnimationTick = 0;
    InterlockedExchange(&g_shared->liveSessionSecondsAck, 0);
    InterlockedExchange(&g_shared->liveAudioLevelsAck, 0);
    InterlockedExchange(&g_shared->liveHudStatusAck, 0);
    return true;
}

void UpdateLiveFpsTexture()
{
    if (!g_shared ||
        !g_hudTexture ||
        g_liveFpsPatches.empty()) {
        return;
    }

    const LONG packedLiveFps =
        InterlockedCompareExchange(
            &g_shared->liveFpsAck,
            0,
            0);
    const LONG receivedFps =
        packedLiveFps & 0xFFFF;
    if (receivedFps <= 0)
        return;

    const LONG clampedFps =
        std::clamp<LONG>(
            receivedFps,
            0,
            kMaxCachedFps);

    const std::uint32_t packedStatus =
        static_cast<std::uint32_t>(
            InterlockedCompareExchange(
                &g_shared->liveHudStatus,
                0,
                0));
    const bool sessionActive =
        (packedStatus &
         (kHudStatusRecordingBit |
          kHudStatusStreamingBit)) != 0;

    if (clampedFps == g_lastRenderedLiveFps &&
        sessionActive == g_lastRenderedSessionActive) {
        return;
    }

    SetDrawStage(DrawStageLiveFpsUploadEntry);

    GLint oldTexture = 0;
    GLint oldUnpackAlignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);

    const auto &sourcePatch =
        g_liveFpsPatches[
            static_cast<size_t>(clampedFps)];
    const std::uint8_t *fpsPixels = sourcePatch.data();

    if (sessionActive) {
        g_liveFpsScratch = sourcePatch;
        for (size_t i = 0;
             i + 3 < g_liveFpsScratch.size();
             i += 4) {
            const int r = g_liveFpsScratch[i + 0];
            const int g = g_liveFpsScratch[i + 1];
            const int b = g_liveFpsScratch[i + 2];
            if (b > r + 25 && b > g + 20) {
                const int strength =
                    std::clamp((b - r) * 255 / 210, 0, 255);
                g_liveFpsScratch[i + 0] =
                    static_cast<std::uint8_t>(
                        (r * (255 - strength) +
                         232 * strength) / 255);
                g_liveFpsScratch[i + 1] =
                    static_cast<std::uint8_t>(
                        (g * (255 - strength) +
                         24 * strength) / 255);
                g_liveFpsScratch[i + 2] =
                    static_cast<std::uint8_t>(
                        (b * (255 - strength) +
                         43 * strength) / 255);
            }
        }
        fpsPixels = g_liveFpsScratch.data();
    }

    glBindTexture(GL_TEXTURE_2D, g_hudTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        kFpsPatchX,
        kFpsPatchY,
        kFpsPatchWidth,
        kFpsUploadHeight,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        fpsPixels);

    glPixelStorei(
        GL_UNPACK_ALIGNMENT,
        oldUnpackAlignment);
    glBindTexture(
        GL_TEXTURE_2D,
        static_cast<GLuint>(oldTexture));

    g_lastRenderedLiveFps = clampedFps;
    g_lastRenderedSessionActive = sessionActive;
    SetDrawStage(DrawStageLiveFpsUploadDone);
}

void UpdateLiveObsFpsTexture()
{
    if (!g_shared ||
        !g_hudTexture ||
        g_liveObsFpsPatches.empty()) {
        return;
    }

    const LONG packedLiveFps =
        InterlockedCompareExchange(
            &g_shared->liveFpsAck,
            0,
            0);
    const LONG receivedObsFps =
        static_cast<LONG>(
            (static_cast<std::uint32_t>(packedLiveFps) >> 16) &
            0xFFFFu);
    if (receivedObsFps <= 0)
        return;

    const LONG clampedObsFps =
        std::clamp<LONG>(
            receivedObsFps,
            0,
            kMaxCachedFps);
    if (clampedObsFps == g_lastRenderedLiveObsFps)
        return;

    SetDrawStage(DrawStageLiveObsFpsUploadEntry);

    GLint oldTexture = 0;
    GLint oldUnpackAlignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);

    glBindTexture(GL_TEXTURE_2D, g_hudTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        kObsFpsPatchX,
        kObsFpsPatchY,
        kObsFpsPatchWidth,
        kObsFpsPatchHeight,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        g_liveObsFpsPatches[
            static_cast<size_t>(clampedObsFps)].data());

    glPixelStorei(
        GL_UNPACK_ALIGNMENT,
        oldUnpackAlignment);
    glBindTexture(
        GL_TEXTURE_2D,
        static_cast<GLuint>(oldTexture));

    g_lastRenderedLiveObsFps = clampedObsFps;
    SetDrawStage(DrawStageLiveObsFpsUploadDone);
}

void UpdateLiveSessionTimerTexture()
{
    if (!g_shared ||
        !g_hudTexture ||
        g_timerGlyphs.size() != kTimerGlyphCount ||
        g_timerBasePatch.empty()) {
        return;
    }

    const LONG receivedSeconds =
        InterlockedCompareExchange(
            &g_shared->liveSessionSeconds,
            0,
            0);
    const LONG clampedSeconds =
        std::clamp<LONG>(
            receivedSeconds,
            0,
            kMaxTimerSeconds);
    if (clampedSeconds == g_lastRenderedSessionSeconds)
        return;

    const LONG hours = clampedSeconds / 3600;
    const LONG minutes = (clampedSeconds % 3600) / 60;
    const LONG seconds = clampedSeconds % 60;

    wchar_t timerText[16] = {};
    swprintf_s(
        timerText,
        L"%ld:%02ld:%02ld",
        hours,
        minutes,
        seconds);

    g_timerScratch = g_timerBasePatch;

    const size_t textLength = std::wcslen(timerText);
    const int textWidth =
        textLength > 0
            ? (static_cast<int>(textLength) - 1) *
                  kTimerGlyphAdvance +
                  kTimerGlyphWidth
            : 0;
    const int startX =
        std::max(0, (kTimerPatchWidth - textWidth) / 2);

    for (size_t charIndex = 0;
         charIndex < textLength;
         ++charIndex) {
        const wchar_t ch = timerText[charIndex];
        int glyphIndex = -1;
        if (ch >= L'0' && ch <= L'9')
            glyphIndex = static_cast<int>(ch - L'0');
        else if (ch == L':')
            glyphIndex = 10;

        if (glyphIndex < 0 ||
            glyphIndex >= static_cast<int>(g_timerGlyphs.size())) {
            continue;
        }

        const auto &glyph =
            g_timerGlyphs[static_cast<size_t>(glyphIndex)];
        const int dstX =
            startX +
            static_cast<int>(charIndex) *
                kTimerGlyphAdvance;

        for (int y = 0; y < kTimerGlyphHeight; ++y) {
            for (int x = 0; x < kTimerGlyphWidth; ++x) {
                const int px = dstX + x;
                if (px < 0 || px >= kTimerPatchWidth)
                    continue;

                const size_t glyphIndexPx =
                    (static_cast<size_t>(y) * kTimerGlyphWidth +
                     static_cast<size_t>(x)) * 4;
                const std::uint8_t alpha =
                    glyph[glyphIndexPx + 3];
                if (alpha == 0)
                    continue;

                const size_t dstIndex =
                    (static_cast<size_t>(y) * kTimerPatchWidth +
                     static_cast<size_t>(px)) * 4;
                const int inverseAlpha = 255 - alpha;

                for (int channel = 0; channel < 3; ++channel) {
                    g_timerScratch[dstIndex + channel] =
                        static_cast<std::uint8_t>(
                            (static_cast<int>(
                                 glyph[glyphIndexPx + channel]) *
                                 alpha +
                             static_cast<int>(
                                 g_timerScratch[dstIndex + channel]) *
                                 inverseAlpha) /
                            255);
                }
                g_timerScratch[dstIndex + 3] =
                    std::max(
                        g_timerScratch[dstIndex + 3],
                        alpha);
            }
        }
    }

    SetDrawStage(DrawStageLiveTimerUploadEntry);

    GLint oldTexture = 0;
    GLint oldUnpackAlignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);

    glBindTexture(GL_TEXTURE_2D, g_hudTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        kTimerPatchX,
        kTimerPatchY,
        kTimerPatchWidth,
        kTimerPatchHeight,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        g_timerScratch.data());

    glPixelStorei(
        GL_UNPACK_ALIGNMENT,
        oldUnpackAlignment);
    glBindTexture(
        GL_TEXTURE_2D,
        static_cast<GLuint>(oldTexture));

    g_lastRenderedSessionSeconds = clampedSeconds;
    InterlockedExchange(
        &g_shared->liveSessionSecondsAck,
        clampedSeconds);
    SetDrawStage(DrawStageLiveTimerUploadDone);
}

void UpdateLiveAudioMetersTexture()
{
    if (!g_shared ||
        !g_hudTexture ||
        g_audioBasePatch.empty()) {
        return;
    }

    const LONG packedAudio =
        InterlockedCompareExchange(
            &g_shared->liveAudioLevels,
            0,
            0);
    if (packedAudio == g_lastRenderedAudioLevels)
        return;

    const int desktopValue =
        static_cast<int>(
            static_cast<std::uint32_t>(packedAudio) & 0xFFFFu);
    const int micValue =
        static_cast<int>(
            (static_cast<std::uint32_t>(packedAudio) >> 16) &
            0xFFFFu);

    const int desktopActive =
        std::clamp(
            (desktopValue * kAudioSegments + 999) / 1000,
            0,
            kAudioSegments);
    const int micActive =
        std::clamp(
            (micValue * kAudioSegments + 999) / 1000,
            0,
            kAudioSegments);

    const size_t audioPatchIndex =
        static_cast<size_t>(
            desktopActive *
                (kAudioSegments + 1) +
            micActive);
    if (audioPatchIndex >=
        g_audioLevelPatches.size()) {
        return;
    }
    const auto &audioPatch =
        g_audioLevelPatches[audioPatchIndex];

    SetDrawStage(DrawStageLiveAudioUploadEntry);

    GLint oldTexture = 0;
    GLint oldUnpackAlignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);

    glBindTexture(GL_TEXTURE_2D, g_hudTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        kAudioPatchX,
        kAudioPatchY,
        kAudioPatchWidth,
        kAudioPatchHeight,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        audioPatch.data());

    glPixelStorei(
        GL_UNPACK_ALIGNMENT,
        oldUnpackAlignment);
    glBindTexture(
        GL_TEXTURE_2D,
        static_cast<GLuint>(oldTexture));

    g_lastRenderedAudioLevels = packedAudio;
    InterlockedExchange(
        &g_shared->liveAudioLevelsAck,
        packedAudio);
    SetDrawStage(DrawStageLiveAudioUploadDone);
}

void UpdateLiveHudStatusTexture()
{
    if (!g_shared ||
        !g_hudTexture ||
        g_diskGlyphs.size() != kDiskGlyphCount ||
        g_diskBasePatch.empty() ||
        g_stateBasePatch.empty()) {
        return;
    }

    const LONG packedStatusLong =
        InterlockedCompareExchange(
            &g_shared->liveHudStatus,
            0,
            0);
    const std::uint32_t packedStatus =
        static_cast<std::uint32_t>(packedStatusLong);

    const LONG diskTenths =
        static_cast<LONG>(
            packedStatus & kHudStatusDiskMask);
    const bool recording =
        (packedStatus & kHudStatusRecordingBit) != 0;
    const bool streaming =
        (packedStatus & kHudStatusStreamingBit) != 0;
    const bool replay =
        (packedStatus & kHudStatusReplayBit) != 0;
    const bool sessionActive = recording || streaming;

    const ULONGLONG now = GetTickCount64();
    const bool animationDue =
        (sessionActive || replay) &&
        (g_lastStatusAnimationTick == 0 ||
         now - g_lastStatusAnimationTick >= 100);
    const bool statusChanged =
        packedStatusLong != g_lastRenderedHudStatus;

    if (!statusChanged && !animationDue)
        return;

    SetDrawStage(DrawStageLiveStatusUploadEntry);

    GLint oldTexture = 0;
    GLint oldUnpackAlignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);
    glBindTexture(GL_TEXTURE_2D, g_hudTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (diskTenths != g_lastRenderedDiskTenths) {
        g_diskScratch = g_diskBasePatch;

        LONG displayTenths =
            std::clamp<LONG>(
                diskTenths,
                0,
                99990);
        wchar_t diskText[16] = {};
        if (displayTenths >= 1000) {
            const LONG wholeGiB =
                (displayTenths + 5) / 10;
            swprintf_s(
                diskText,
                L"%ld GB",
                wholeGiB);
        } else {
            swprintf_s(
                diskText,
                L"%ld.%ld GB",
                displayTenths / 10,
                displayTenths % 10);
        }

        const auto glyphIndexFor =
            [](wchar_t ch) -> int {
                if (ch >= L'0' && ch <= L'9')
                    return static_cast<int>(ch - L'0');
                if (ch == L'.')
                    return 10;
                if (ch == L' ')
                    return 11;
                if (ch == L'G')
                    return 12;
                if (ch == L'B')
                    return 13;
                return -1;
            };

        const size_t textLength =
            std::wcslen(diskText);
        const int textWidth =
            textLength > 0
                ? (static_cast<int>(textLength) - 1) *
                      kDiskGlyphAdvance +
                      kDiskGlyphWidth
                : 0;
        const int startX =
            std::max(
                0,
                (kDiskPatchWidth - textWidth) / 2);

        for (size_t charIndex = 0;
             charIndex < textLength;
             ++charIndex) {
            const int glyphIndex =
                glyphIndexFor(diskText[charIndex]);
            if (glyphIndex < 0)
                continue;

            const auto &glyph =
                g_diskGlyphs[
                    static_cast<size_t>(glyphIndex)];
            const int dstX =
                startX +
                static_cast<int>(charIndex) *
                    kDiskGlyphAdvance;

            for (int y = 0;
                 y < kDiskGlyphHeight;
                 ++y) {
                for (int x = 0;
                     x < kDiskGlyphWidth;
                     ++x) {
                    const int px = dstX + x;
                    if (px < 0 ||
                        px >= kDiskPatchWidth) {
                        continue;
                    }

                    const size_t srcIndex =
                        (static_cast<size_t>(y) *
                             kDiskGlyphWidth +
                         static_cast<size_t>(x)) * 4;
                    const std::uint8_t alpha =
                        glyph[srcIndex + 3];
                    if (alpha == 0)
                        continue;

                    const size_t dstIndex =
                        (static_cast<size_t>(y) *
                             kDiskPatchWidth +
                         static_cast<size_t>(px)) * 4;
                    const int inverseAlpha =
                        255 - alpha;

                    for (int channel = 0;
                         channel < 3;
                         ++channel) {
                        g_diskScratch[
                            dstIndex + channel] =
                            static_cast<std::uint8_t>(
                                (static_cast<int>(
                                     glyph[
                                         srcIndex +
                                         channel]) *
                                     alpha +
                                 static_cast<int>(
                                     g_diskScratch[
                                         dstIndex +
                                         channel]) *
                                     inverseAlpha) /
                                255);
                    }
                    g_diskScratch[dstIndex + 3] =
                        std::max(
                            g_diskScratch[dstIndex + 3],
                            alpha);
                }
            }
        }

        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            kDiskPatchX,
            kDiskPatchY,
            kDiskPatchWidth,
            kDiskPatchHeight,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            g_diskScratch.data());
        g_lastRenderedDiskTenths = diskTenths;
    }

    if (statusChanged || animationDue) {
        g_stateScratch = g_stateBasePatch;

        const auto setPixel =
            [&](int x,
                int y,
                std::uint8_t r,
                std::uint8_t g,
                std::uint8_t b,
                std::uint8_t a = 255) {
                if (x < 0 ||
                    x >= kStatePatchWidth ||
                    y < 0 ||
                    y >= kStatePatchHeight) {
                    return;
                }
                const size_t index =
                    (static_cast<size_t>(y) *
                         kStatePatchWidth +
                     static_cast<size_t>(x)) * 4;
                const int inverseAlpha =
                    255 - a;
                g_stateScratch[index + 0] =
                    static_cast<std::uint8_t>(
                        (r * a +
                         g_stateScratch[index + 0] *
                             inverseAlpha) /
                        255);
                g_stateScratch[index + 1] =
                    static_cast<std::uint8_t>(
                        (g * a +
                         g_stateScratch[index + 1] *
                             inverseAlpha) /
                        255);
                g_stateScratch[index + 2] =
                    static_cast<std::uint8_t>(
                        (b * a +
                         g_stateScratch[index + 2] *
                             inverseAlpha) /
                        255);
                g_stateScratch[index + 3] =
                    std::max(
                        g_stateScratch[index + 3],
                        a);
            };

        const double centerX =
            110.0 - kStatePatchX;
        const double centerY =
            24.0 - kStatePatchY;

        if (sessionActive) {
            const double phase =
                std::fmod(
                    static_cast<double>(now) *
                        0.18,
                    360.0);
            constexpr double kPi =
                3.14159265358979323846;

            for (int y = 0;
                 y < kStatePatchHeight;
                 ++y) {
                for (int x = 0;
                     x < kStatePatchWidth;
                     ++x) {
                    const double dx =
                        (x + 0.5) - centerX;
                    const double dy =
                        (y + 0.5) - centerY;
                    const double radius =
                        std::sqrt(dx * dx + dy * dy);
                    if (radius < 7.0 ||
                        radius > 9.0) {
                        continue;
                    }

                    double angle =
                        std::atan2(-dy, dx) *
                        180.0 / kPi;
                    if (angle < 0.0)
                        angle += 360.0;

                    double relative =
                        std::fmod(
                            angle - phase + 360.0,
                            360.0);
                    if (relative <= 255.0) {
                        setPixel(
                            x, y,
                            226, 25, 47);
                    }
                }
            }
        } else {
            const int dotXs[] = {3, 9, 15};
            const int dotY = 8;
            for (int dotX : dotXs) {
                for (int y = dotY - 2;
                     y <= dotY + 2;
                     ++y) {
                    for (int x = dotX - 2;
                         x <= dotX + 2;
                         ++x) {
                        const int dx = x - dotX;
                        const int dy = y - dotY;
                        if (dx * dx + dy * dy <= 3) {
                            setPixel(
                                x, y,
                                158, 164, 169);
                        }
                    }
                }
            }
        }

        if (replay) {
            const double shimmer =
                (std::sin(
                     static_cast<double>(now) *
                     0.012) +
                 1.0) *
                0.5;
            const std::uint8_t boltR = 255;
            const std::uint8_t boltG =
                static_cast<std::uint8_t>(
                    190 + 40 * shimmer);
            const std::uint8_t boltB = 55;

            const double polygon[6][2] = {
                {34.4, 5.0},
                {29.7, 13.4},
                {33.2, 13.4},
                {31.4, 20.0},
                {38.4, 11.1},
                {34.8, 11.1},
            };

            for (int y = 3;
                 y < kStatePatchHeight;
                 ++y) {
                for (int x = 27;
                     x < kStatePatchWidth;
                     ++x) {
                    const double px = x + 0.5;
                    const double py = y + 0.5;
                    bool inside = false;
                    for (int i = 0, j = 5;
                         i < 6;
                         j = i++) {
                        const double xi =
                            polygon[i][0];
                        const double yi =
                            polygon[i][1];
                        const double xj =
                            polygon[j][0];
                        const double yj =
                            polygon[j][1];

                        const bool intersects =
                            ((yi > py) != (yj > py)) &&
                            (px <
                             (xj - xi) *
                                     (py - yi) /
                                     (yj - yi) +
                                 xi);
                        if (intersects)
                            inside = !inside;
                    }
                    if (inside) {
                        setPixel(
                            x, y,
                            boltR,
                            boltG,
                            boltB);
                    }
                }
            }
        }

        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            kStatePatchX,
            kStatePatchY,
            kStatePatchWidth,
            kStatePatchHeight,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            g_stateScratch.data());

        g_lastStatusAnimationTick = now;
    }

    glPixelStorei(
        GL_UNPACK_ALIGNMENT,
        oldUnpackAlignment);
    glBindTexture(
        GL_TEXTURE_2D,
        static_cast<GLuint>(oldTexture));

    g_lastRenderedHudStatus =
        packedStatusLong;
    InterlockedExchange(
        &g_shared->liveHudStatusAck,
        packedStatusLong);
    SetDrawStage(DrawStageLiveStatusUploadDone);
}

bool EnsureModernHudRenderer()
{
    const HGLRC context = wglGetCurrentContext();
    if (!context)
        return false;

    if (g_modernContext == context &&
        g_hudProgram &&
        g_hudVao &&
        g_hudVbo &&
        g_hudTexture) {
        return true;
    }

    g_modernContext = context;
    g_modernLoadFailed = false;
    g_hudTexture = 0;
    g_hudProgram = 0;
    g_hudVao = 0;
    g_hudVbo = 0;
    g_hudSampler = -1;

    if (!LoadModernGl() ||
        !CreateHudProgram() ||
        !CreateHudTexture()) {
        g_modernLoadFailed = true;
        return false;
    }

    GLint oldVao = 0;
    GLint oldBuffer = 0;
    GLint oldProgram = 0;

    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldBuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);

    g_glGenVertexArrays(1, &g_hudVao);
    g_glGenBuffers(1, &g_hudVbo);
    if (!g_hudVao || !g_hudVbo) {
        g_modernLoadFailed = true;
        return false;
    }

    g_glBindVertexArray(g_hudVao);
    g_glBindBuffer(GL_ARRAY_BUFFER, g_hudVbo);
    g_glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GlSizePtr>(6 * 4 * sizeof(GLfloat)),
        nullptr,
        GL_STREAM_DRAW);

    g_glEnableVertexAttribArray(0);
    g_glVertexAttribPointer(
        0, 2, GL_FLOAT, GL_FALSE,
        4 * sizeof(GLfloat),
        reinterpret_cast<const void *>(0));

    g_glEnableVertexAttribArray(1);
    g_glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE,
        4 * sizeof(GLfloat),
        reinterpret_cast<const void *>(
            2 * sizeof(GLfloat)));

    g_glUseProgram(g_hudProgram);
    g_glUniform1i(g_hudSampler, 0);
    g_glUniform1f(g_hudOpacity, 1.0f);

    g_glUseProgram(static_cast<GLuint>(oldProgram));
    g_glBindBuffer(
        GL_ARRAY_BUFFER,
        static_cast<GLuint>(oldBuffer));
    g_glBindVertexArray(
        static_cast<GLuint>(oldVao));

    return true;
}

void DrawFallbackMarker()
{
    GLint viewport[4] = {};
    GLint oldScissor[4] = {};
    GLfloat oldClearColor[4] = {};
    GLboolean oldColorMask[4] = {};

    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] < 64 || viewport[3] < 64)
        return;

    const GLboolean scissorWasEnabled =
        glIsEnabled(GL_SCISSOR_TEST);

    glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, oldClearColor);
    glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);

    glEnable(GL_SCISSOR_TEST);
    glScissor(
        viewport[0] + viewport[2] - 30,
        viewport[1] + viewport[3] - 30,
        18,
        18);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.10f, 1.0f, 0.20f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glColorMask(
        oldColorMask[0], oldColorMask[1],
        oldColorMask[2], oldColorMask[3]);
    glClearColor(
        oldClearColor[0], oldClearColor[1],
        oldClearColor[2], oldClearColor[3]);
    glScissor(
        oldScissor[0], oldScissor[1],
        oldScissor[2], oldScissor[3]);

    if (!scissorWasEnabled)
        glDisable(GL_SCISSOR_TEST);
}

void DrawStaticHud(HDC dc)
{
    SetDrawStage(DrawStageIdle);
    if (!ShouldDrawOverlay(dc))
        return;

    SetDrawStage(DrawStageEligible);
    SetDrawStage(DrawStageRendererEntry);
    if (!EnsureModernHudRenderer()) {
        SetDrawStage(DrawStageFallbackEntry);
        InterlockedExchange(&g_shared->renderMode, 2);
        DrawFallbackMarker();

        InterlockedIncrement64(&g_shared->drawCount);
        InterlockedExchange64(
            &g_shared->lastDrawTick,
            static_cast<LONG64>(GetTickCount64()));
        SetDrawStage(DrawStageComplete);
        return;
    }

    SetDrawStage(DrawStageRendererReady);
    InterlockedExchange(&g_shared->renderMode, 1);
    UpdateLiveFpsTexture();
    UpdateLiveObsFpsTexture();
    UpdateLiveSessionTimerTexture();
    UpdateLiveAudioMetersTexture();
    UpdateLiveHudStatusTexture();

    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] < kHudWidth + kHudMargin * 2 ||
        viewport[3] < kHudHeight + kHudMargin * 2) {
        return;
    }
    SetDrawStage(DrawStageViewportReady);

    const GLfloat leftPx =
        static_cast<GLfloat>(
            viewport[2] - kHudWidth - kHudMargin);
    const GLfloat rightPx =
        leftPx + static_cast<GLfloat>(kHudWidth);
    const GLfloat topPx =
        static_cast<GLfloat>(
            viewport[3] - kHudMargin);
    const GLfloat bottomPx =
        topPx - static_cast<GLfloat>(kHudHeight);

    const auto ndcX = [viewport](GLfloat px) {
        return px * 2.0f /
                   static_cast<GLfloat>(viewport[2]) -
               1.0f;
    };
    const auto ndcY = [viewport](GLfloat py) {
        return py * 2.0f /
                   static_cast<GLfloat>(viewport[3]) -
               1.0f;
    };

    const GLfloat left = ndcX(leftPx);
    const GLfloat right = ndcX(rightPx);
    const GLfloat top = ndcY(topPx);
    const GLfloat bottom = ndcY(bottomPx);

    const GLfloat vertices[] = {
        left,  bottom, 0.0f, 1.0f,
        right, bottom, 1.0f, 1.0f,
        right, top,    1.0f, 0.0f,
        left,  bottom, 0.0f, 1.0f,
        right, top,    1.0f, 0.0f,
        left,  top,    0.0f, 0.0f,
    };

    GLint oldProgram = 0;
    GLint oldVao = 0;
    GLint oldBuffer = 0;
    GLint oldActiveTexture = GL_TEXTURE0;
    GLint oldTexture0 = 0;
    GLint oldBlendSrc = GL_ONE;
    GLint oldBlendDst = GL_ZERO;
    GLboolean oldColorMask[4] = {};

    glGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &oldVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &oldBuffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &oldActiveTexture);
    glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);

    const GLboolean blendWasEnabled =
        glIsEnabled(GL_BLEND);
    const GLboolean depthWasEnabled =
        glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cullWasEnabled =
        glIsEnabled(GL_CULL_FACE);
    const GLboolean scissorWasEnabled =
        glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean stencilWasEnabled =
        glIsEnabled(GL_STENCIL_TEST);

    glGetIntegerv(GL_BLEND_SRC, &oldBlendSrc);
    glGetIntegerv(GL_BLEND_DST, &oldBlendDst);

    g_glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture0);
    SetDrawStage(DrawStageStateCaptured);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    SetDrawStage(DrawStageOverlayStateApplied);

    g_glUseProgram(g_hudProgram);
    g_glBindVertexArray(g_hudVao);
    g_glBindBuffer(GL_ARRAY_BUFFER, g_hudVbo);
    g_glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GlSizePtr>(sizeof(vertices)),
        vertices,
        GL_STREAM_DRAW);
    SetDrawStage(DrawStageVerticesUploaded);

    const std::uint32_t packedStatus =
        g_shared
            ? static_cast<std::uint32_t>(
                  InterlockedCompareExchange(
                      &g_shared->liveHudStatus,
                      0,
                      0))
            : 0u;
    int opacityPercent =
        static_cast<int>(
            (packedStatus >>
             kHudStatusOpacityShift) &
            0x7Fu);
    if (opacityPercent <= 0 ||
        opacityPercent > 100) {
        opacityPercent = 100;
    }

    glBindTexture(GL_TEXTURE_2D, g_hudTexture);
    g_glUniform1i(g_hudSampler, 0);
    g_glUniform1f(
        g_hudOpacity,
        static_cast<GLfloat>(
            opacityPercent) /
            100.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    SetDrawStage(DrawStageDrawReturned);

    glBindTexture(
        GL_TEXTURE_2D,
        static_cast<GLuint>(oldTexture0));
    g_glBindBuffer(
        GL_ARRAY_BUFFER,
        static_cast<GLuint>(oldBuffer));
    g_glBindVertexArray(
        static_cast<GLuint>(oldVao));
    g_glUseProgram(
        static_cast<GLuint>(oldProgram));
    g_glActiveTexture(
        static_cast<GLenum>(oldActiveTexture));

    glBlendFunc(
        static_cast<GLenum>(oldBlendSrc),
        static_cast<GLenum>(oldBlendDst));
    glColorMask(
        oldColorMask[0], oldColorMask[1],
        oldColorMask[2], oldColorMask[3]);

    if (!blendWasEnabled)
        glDisable(GL_BLEND);
    if (depthWasEnabled)
        glEnable(GL_DEPTH_TEST);
    if (cullWasEnabled)
        glEnable(GL_CULL_FACE);
    if (scissorWasEnabled)
        glEnable(GL_SCISSOR_TEST);
    if (stencilWasEnabled)
        glEnable(GL_STENCIL_TEST);
    SetDrawStage(DrawStageStateRestored);

    InterlockedIncrement64(&g_shared->drawCount);
    InterlockedExchange64(
        &g_shared->lastDrawTick,
        static_cast<LONG64>(GetTickCount64()));
    SetDrawStage(DrawStageComplete);
}

void SwapBegin(HDC dc)
{
    if (g_swapDepth++ == 0) {
        RecordPresent();
        DrawStaticHud(dc);
    }
}

void SwapEnd()
{
    if (g_swapDepth > 0)
        --g_swapDepth;
}

BOOL WINAPI HookedSwapBuffers(HDC dc)
{
    SwapBegin(dc);
    const BOOL result =
        g_realSwapBuffers ? g_realSwapBuffers(dc) : FALSE;
    SwapEnd();
    return result;
}

BOOL WINAPI HookedWglSwapBuffers(HDC dc)
{
    SwapBegin(dc);
    const BOOL result =
        g_realWglSwapBuffers ? g_realWglSwapBuffers(dc) : FALSE;
    SwapEnd();
    return result;
}

BOOL WINAPI HookedWglSwapLayerBuffers(HDC dc, UINT planes)
{
    SwapBegin(dc);
    const BOOL result =
        g_realWglSwapLayerBuffers
            ? g_realWglSwapLayerBuffers(dc, planes)
            : FALSE;
    SwapEnd();
    return result;
}

bool CreateSharedState()
{
    wchar_t name[96] = {};
    std::swprintf(
        name,
        sizeof(name) / sizeof(name[0]),
        L"Local\\ClatashaHUD_OGL_%lu",
        GetCurrentProcessId());

    g_sharedMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(OpenGlPresentShared),
        name);
    if (!g_sharedMapping)
        return false;

    g_shared = static_cast<OpenGlPresentShared *>(
        MapViewOfFile(
            g_sharedMapping,
            FILE_MAP_ALL_ACCESS,
            0,
            0,
            sizeof(OpenGlPresentShared)));
    if (!g_shared) {
        CloseHandle(g_sharedMapping);
        g_sharedMapping = nullptr;
        return false;
    }

    ZeroMemory(g_shared, sizeof(*g_shared));
    g_shared->magic = kSharedMagic;
    g_shared->version = kSharedVersion;
    g_shared->pid = GetCurrentProcessId();
    return true;
}

bool InstallPresentHooks()
{
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");

    if (!gdi || !gl)
        return false;

    g_realSwapBuffers =
        reinterpret_cast<SwapBuffersFn>(
            GetProcAddress(gdi, "SwapBuffers"));
    g_realWglSwapBuffers =
        reinterpret_cast<WglSwapBuffersFn>(
            GetProcAddress(gl, "wglSwapBuffers"));
    g_realWglSwapLayerBuffers =
        reinterpret_cast<WglSwapLayerBuffersFn>(
            GetProcAddress(gl, "wglSwapLayerBuffers"));

    if (!g_realSwapBuffers &&
        !g_realWglSwapBuffers &&
        !g_realWglSwapLayerBuffers) {
        return false;
    }

    LONG mask = 0;
    LONG error = DetourTransactionBegin();
    if (error != NO_ERROR)
        return false;

    error = DetourUpdateThread(GetCurrentThread());
    if (error != NO_ERROR) {
        DetourTransactionAbort();
        return false;
    }

    if (g_realSwapBuffers) {
        error = DetourAttach(
            reinterpret_cast<PVOID *>(&g_realSwapBuffers),
            HookedSwapBuffers);
        if (error != NO_ERROR) {
            DetourTransactionAbort();
            return false;
        }
        mask |= HookSwapBuffers;
    }

    if (g_realWglSwapBuffers) {
        error = DetourAttach(
            reinterpret_cast<PVOID *>(&g_realWglSwapBuffers),
            HookedWglSwapBuffers);
        if (error != NO_ERROR) {
            DetourTransactionAbort();
            return false;
        }
        mask |= HookWglSwapBuffers;
    }

    if (g_realWglSwapLayerBuffers) {
        error = DetourAttach(
            reinterpret_cast<PVOID *>(&g_realWglSwapLayerBuffers),
            HookedWglSwapLayerBuffers);
        if (error != NO_ERROR) {
            DetourTransactionAbort();
            return false;
        }
        mask |= HookWglSwapLayerBuffers;
    }

    error = DetourTransactionCommit();
    if (error != NO_ERROR)
        return false;

    SetHookState(1, mask);
    return true;
}

DWORD WINAPI HookWorker(void *)
{
    if (!CreateSharedState())
        return 1;

    // The helper only injects after opengl32.dll has been observed, but allow
    // a short grace period for renderer initialization races.
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (InstallPresentHooks())
            return 0;
        Sleep(250);
    }

    SetHookState(2, 0);
    return 2;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        DetourRestoreAfterWith();

        HANDLE thread = CreateThread(
            nullptr,
            0,
            HookWorker,
            nullptr,
            0,
            nullptr);
        if (thread)
            CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_shared) {
            UnmapViewOfFile(g_shared);
            g_shared = nullptr;
        }
        if (g_sharedMapping) {
            CloseHandle(g_sharedMapping);
            g_sharedMapping = nullptr;
        }
    }

    return TRUE;
}
