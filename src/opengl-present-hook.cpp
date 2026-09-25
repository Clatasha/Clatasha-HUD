#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <detours.h>
#include <GL/gl.h>

#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <vector>

namespace {

constexpr std::uint32_t kSharedMagic = 0x4C474F43; // "COGL"
constexpr std::uint32_t kSharedVersion = 5;

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
    DrawStageViewportReady = 30,
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
LONG g_lastRenderedLiveFps = -1;
LONG g_lastRenderedLiveObsFps = -1;
LONG g_lastRenderedSessionSeconds = -1;
LONG g_lastRenderedAudioLevels = -1;

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

    HPEN borderPen =
        CreatePen(PS_SOLID, 1, RGB(100, 109, 116));
    HBRUSH panelBrush =
        CreateSolidBrush(RGB(8, 10, 12));
    HGDIOBJ oldPen = SelectObject(dc, borderPen);
    HGDIOBJ oldBrush = SelectObject(dc, panelBrush);
    RoundRect(dc, 0, 0, kHudWidth, kHudHeight, 8, 8);

    DrawMeter(dc, 5, 5);
    DrawMeter(dc, 17, 5);

    SetBkMode(dc, TRANSPARENT);

    HFONT fpsFont = CreateFontW(
        -29, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT smallBold = CreateFontW(
        -12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT tinyFont = CreateFontW(
        -9, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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

    HBRUSH badgeBrush =
        CreateSolidBrush(RGB(22, 27, 32));
    HPEN badgePen =
        CreatePen(PS_SOLID, 1, RGB(96, 106, 116));
    SelectObject(dc, badgeBrush);
    SelectObject(dc, badgePen);
    RoundRect(dc, 102, 2, 126, 13, 4, 4);

    SelectObject(dc, tinyFont);
    SetTextColor(dc, RGB(210, 216, 221));
    RECT oglRect{102, 2, 126, 13};
    DrawTextW(
        dc, L"OGL", -1, &oglRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SetTextColor(dc, RGB(205, 209, 212));
    // Leave the timer rectangle clean. Timer glyphs are cached once below.

    HBRUSH dotBrush =
        CreateSolidBrush(RGB(158, 164, 169));
    SelectObject(dc, dotBrush);
    SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, 102, 22, 106, 26);
    Ellipse(dc, 108, 22, 112, 26);
    Ellipse(dc, 114, 22, 118, 26);

    SetTextColor(dc, RGB(165, 171, 176));
    RECT diskRect{94, 36, 135, 49};
    DrawTextW(
        dc, L"123 GB", -1, &diskRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    HPEN iconPen =
        CreatePen(PS_SOLID, 1, RGB(174, 181, 187));
    SelectObject(dc, iconPen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 3, 38, 10, 43);
    MoveToEx(dc, 6, 43, nullptr);
    LineTo(dc, 6, 46);
    MoveToEx(dc, 4, 46, nullptr);
    LineTo(dc, 9, 46);

    RoundRect(dc, 15, 37, 19, 43, 3, 3);
    Arc(dc, 14, 39, 20, 45, 14, 41, 20, 41);
    MoveToEx(dc, 17, 44, nullptr);
    LineTo(dc, 17, 47);

    HBRUSH logoBrush =
        CreateSolidBrush(RGB(240, 243, 245));
    SelectObject(dc, logoBrush);
    SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, 128, 2, 143, 17);

    SelectObject(dc, smallBold);
    SetTextColor(dc, RGB(18, 22, 26));
    RECT logoRect{128, 1, 143, 18};
    DrawTextW(
        dc, L"C", -1, &logoRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

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

            if (r == 0 && g == 0 && b == 0) {
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
        DeleteObject(iconPen);
        DeleteObject(dotBrush);
        DeleteObject(logoBrush);
        DeleteObject(badgePen);
        DeleteObject(badgeBrush);
        DeleteObject(tinyFont);
        DeleteObject(smallBold);
        DeleteObject(fpsFont);
        DeleteObject(panelBrush);
        DeleteObject(borderPen);
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
        DeleteObject(iconPen);
        DeleteObject(dotBrush);
        DeleteObject(logoBrush);
        DeleteObject(badgePen);
        DeleteObject(badgeBrush);
        DeleteObject(tinyFont);
        DeleteObject(smallBold);
        DeleteObject(fpsFont);
        DeleteObject(panelBrush);
        DeleteObject(borderPen);
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
        RECT localFpsRect{0, -5, kFpsPatchWidth, kFpsPatchHeight - 3};
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
            0, 0,
            kObsFpsPatchWidth,
            kObsFpsPatchHeight};
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

    SelectObject(fpsDc, fpsOldFont);
    SelectObject(fpsDc, fpsOldBitmap);
    DeleteObject(fpsBitmap);
    DeleteDC(fpsDc);

    SelectObject(dc, oldFont);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldBitmap);

    DeleteObject(iconPen);
    DeleteObject(dotBrush);
    DeleteObject(logoBrush);
    DeleteObject(badgePen);
    DeleteObject(badgeBrush);
    DeleteObject(tinyFont);
    DeleteObject(smallBold);
    DeleteObject(fpsFont);
    DeleteObject(panelBrush);
    DeleteObject(borderPen);
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
        "in vec2 vUv;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "  fragColor = texture(uTex, vUv);\n"
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
    return g_hudSampler >= 0;
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
    InterlockedExchange(&g_shared->liveSessionSecondsAck, 0);
    InterlockedExchange(&g_shared->liveAudioLevelsAck, 0);
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
    if (clampedFps == g_lastRenderedLiveFps)
        return;

    SetDrawStage(DrawStageLiveFpsUploadEntry);

    GLint oldTexture = 0;
    GLint oldUnpackAlignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);

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
        g_liveFpsPatches[
            static_cast<size_t>(clampedFps)].data());

    glPixelStorei(
        GL_UNPACK_ALIGNMENT,
        oldUnpackAlignment);
    glBindTexture(
        GL_TEXTURE_2D,
        static_cast<GLuint>(oldTexture));

    g_lastRenderedLiveFps = clampedFps;
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

    g_audioScratch = g_audioBasePatch;

    const auto paintMeter =
        [&](int meterX, int activeSegments) {
            for (int i = 0; i < kAudioSegments; ++i) {
                const int sy =
                    25 -
                    i * (kAudioSegmentHeight +
                         kAudioSegmentGap);
                const bool active = i < activeSegments;
                const std::uint8_t r = active ? 31 : 49;
                const std::uint8_t g = active ? 218 : 55;
                const std::uint8_t b = active ? 102 : 59;

                for (int y = 0;
                     y < kAudioSegmentHeight;
                     ++y) {
                    for (int x = 0; x < 3; ++x) {
                        const int px = meterX + x;
                        const int py = sy + y;
                        if (px < 0 ||
                            px >= kAudioPatchWidth ||
                            py < 0 ||
                            py >= kAudioPatchHeight) {
                            continue;
                        }

                        const size_t index =
                            (static_cast<size_t>(py) *
                                 kAudioPatchWidth +
                             static_cast<size_t>(px)) *
                            4;
                        g_audioScratch[index + 0] = r;
                        g_audioScratch[index + 1] = g;
                        g_audioScratch[index + 2] = b;
                        g_audioScratch[index + 3] = 255;
                    }
                }
            }
        };

    paintMeter(1, desktopActive);
    paintMeter(13, micActive);

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
        g_audioScratch.data());

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

    glBindTexture(GL_TEXTURE_2D, g_hudTexture);
    g_glUniform1i(g_hudSampler, 0);
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
