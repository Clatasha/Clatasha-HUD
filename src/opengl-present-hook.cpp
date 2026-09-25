#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <detours.h>
#include <GL/gl.h>

#include <cstdint>
#include <cwchar>
#include <vector>

namespace {

constexpr std::uint32_t kSharedMagic = 0x4C474F43; // "COGL"
constexpr std::uint32_t kSharedVersion = 2;

enum HookBits : LONG {
    HookSwapBuffers = 1 << 0,
    HookWglSwapBuffers = 1 << 1,
    HookWglSwapLayerBuffers = 1 << 2,
};

struct alignas(8) OpenGlPresentShared {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t pid;
    std::uint32_t reserved;
    volatile LONG64 presentCount;
    volatile LONG64 lastPresentTick;
    volatile LONG hookState; // 0 = starting, 1 = active, 2 = failed
    volatile LONG hookedMask;
    volatile LONG drawMarker;
    volatile LONG reservedControl;
    volatile LONG64 drawCount;
    volatile LONG64 lastDrawTick;
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

    InterlockedIncrement64(&g_shared->presentCount);
    InterlockedExchange64(
        &g_shared->lastPresentTick,
        static_cast<LONG64>(GetTickCount64()));
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

GLuint g_hudTexture = 0;
HGLRC g_hudTextureContext = nullptr;
std::vector<std::uint8_t> g_hudPixels;

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
    RECT fpsRect{22, -2, 75, 34};
    DrawTextW(
        dc, L"60", -1, &fpsRect,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, smallBold);
    SetTextColor(dc, RGB(165, 171, 176));
    RECT obsRect{74, 3, 103, 25};
    DrawTextW(
        dc, L"/60", -1, &obsRect,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

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
    RECT timerRect{23, 28, 92, 44};
    DrawTextW(
        dc, L"0:12:34", -1, &timerRect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

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

bool EnsureHudTexture()
{
    const HGLRC currentContext = wglGetCurrentContext();
    if (!currentContext)
        return false;

    if (g_hudTexture != 0 &&
        g_hudTextureContext == currentContext) {
        return true;
    }

    g_hudTexture = 0;
    g_hudTextureContext = currentContext;

    if (!BuildStaticHudPixels())
        return false;

    GLint oldTexture = 0;
    GLint oldUnpackAlignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);

    glGenTextures(1, &g_hudTexture);
    if (g_hudTexture == 0)
        return false;

    glBindTexture(GL_TEXTURE_2D, g_hudTexture);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(
        GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
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

    return true;
}

void DrawStaticHud(HDC dc)
{
    if (!ShouldDrawOverlay(dc) || !EnsureHudTexture())
        return;

    GLint viewport[4] = {};
    GLint oldMatrixMode = GL_MODELVIEW;
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_MATRIX_MODE, &oldMatrixMode);

    if (viewport[2] < kHudWidth + kHudMargin * 2 ||
        viewport[3] < kHudHeight + kHudMargin * 2) {
        return;
    }

    glPushAttrib(GL_ALL_ATTRIB_BITS);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_LIGHTING);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_hudTexture);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(
        0.0,
        static_cast<GLdouble>(viewport[2]),
        0.0,
        static_cast<GLdouble>(viewport[3]),
        -1.0,
        1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    const GLfloat left =
        static_cast<GLfloat>(
            viewport[2] - kHudWidth - kHudMargin);
    const GLfloat right =
        left + static_cast<GLfloat>(kHudWidth);
    const GLfloat top =
        static_cast<GLfloat>(
            viewport[3] - kHudMargin);
    const GLfloat bottom =
        top - static_cast<GLfloat>(kHudHeight);

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 1.0f);
    glVertex2f(left, bottom);
    glTexCoord2f(1.0f, 1.0f);
    glVertex2f(right, bottom);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2f(right, top);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(left, top);
    glEnd();

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(oldMatrixMode);

    glPopAttrib();

    InterlockedIncrement64(&g_shared->drawCount);
    InterlockedExchange64(
        &g_shared->lastDrawTick,
        static_cast<LONG64>(GetTickCount64()));
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
