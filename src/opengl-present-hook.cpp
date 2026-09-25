#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <detours.h>
#include <GL/gl.h>

#include "hud-telemetry.hpp"

#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <vector>

namespace {

constexpr std::uint32_t kSharedMagic = 0x4C474F43; // "COGL"
constexpr std::uint32_t kSharedVersion = 3;

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
    volatile LONG renderMode; // 0 = idle, 1 = modern shader, 2 = fallback marker
    volatile LONG renderReserved;
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

HANDLE g_hudTelemetryMapping = nullptr;
clatasha::HudTelemetryShared *g_hudTelemetry = nullptr;
LONG g_lastHudTelemetrySequence = -1;
ULONGLONG g_lastHudTelemetryPoll = 0;
std::int32_t g_hudLocation = clatasha::HudTopRight;

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

    // The helper owns fullscreen qualification and applies hysteresis before
    // arming drawMarker. Do not repeat the fragile monitor-geometry test here;
    // only confirm that this OpenGL surface belongs to the foreground process.
    const HWND hwnd = WindowFromDC(dc);
    const HWND foreground = GetForegroundWindow();
    if (!hwnd || !foreground ||
        !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return false;
    }

    DWORD windowPid = 0;
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    GetWindowThreadProcessId(foreground, &foregroundPid);

    const DWORD selfPid = GetCurrentProcessId();
    return windowPid == selfPid && foregroundPid == selfPid;
}

bool OpenHudTelemetry()
{
    if (g_hudTelemetry)
        return true;

    g_hudTelemetryMapping =
        OpenFileMappingW(
            FILE_MAP_READ,
            FALSE,
            clatasha::kHudTelemetryMappingName);
    if (!g_hudTelemetryMapping)
        return false;

    g_hudTelemetry =
        static_cast<clatasha::HudTelemetryShared *>(
            MapViewOfFile(
                g_hudTelemetryMapping,
                FILE_MAP_READ,
                0,
                0,
                sizeof(clatasha::HudTelemetryShared)));
    if (!g_hudTelemetry) {
        CloseHandle(g_hudTelemetryMapping);
        g_hudTelemetryMapping = nullptr;
        return false;
    }

    if (g_hudTelemetry->magic != clatasha::kHudTelemetryMagic ||
        g_hudTelemetry->version != clatasha::kHudTelemetryVersion) {
        UnmapViewOfFile(g_hudTelemetry);
        g_hudTelemetry = nullptr;
        CloseHandle(g_hudTelemetryMapping);
        g_hudTelemetryMapping = nullptr;
        return false;
    }

    return true;
}

bool ReadHudTelemetry(clatasha::HudTelemetryShared &snapshot)
{
    if (!OpenHudTelemetry())
        return false;

    auto *sequence =
        reinterpret_cast<volatile LONG *>(
            &g_hudTelemetry->sequence);

    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG before =
            InterlockedCompareExchange(sequence, 0, 0);
        if ((before & 1) != 0) {
            YieldProcessor();
            continue;
        }

        std::memcpy(
            &snapshot,
            const_cast<const clatasha::HudTelemetryShared *>(
                g_hudTelemetry),
            sizeof(snapshot));
        MemoryBarrier();

        const LONG after =
            InterlockedCompareExchange(sequence, 0, 0);
        if (before == after && (after & 1) == 0)
            return true;
    }

    return false;
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
    g_hudPixels.assign(
        static_cast<size_t>(clatasha::kHudPixelBytes),
        0);

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

    return true;
}

void UpdateHudTextureFromTelemetry()
{
    const ULONGLONG now = GetTickCount64();
    if (g_lastHudTelemetryPoll != 0 &&
        now - g_lastHudTelemetryPoll < 100) {
        return;
    }
    g_lastHudTelemetryPoll = now;

    clatasha::HudTelemetryShared snapshot{};
    if (!ReadHudTelemetry(snapshot))
        return;

    const LONG sequence =
        static_cast<LONG>(snapshot.sequence);
    if (sequence == g_lastHudTelemetrySequence)
        return;

    if (snapshot.pixelWidth != clatasha::kHudPixelWidth ||
        snapshot.pixelHeight != clatasha::kHudPixelHeight ||
        snapshot.pixelBytes != clatasha::kHudPixelBytes) {
        return;
    }

    g_hudLocation =
        std::clamp(
            static_cast<std::int32_t>(snapshot.location),
            static_cast<std::int32_t>(clatasha::HudTopLeft),
            static_cast<std::int32_t>(clatasha::HudBottomRight));

    GLint oldTexture = 0;
    GLint oldUnpackAlignment = 4;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &oldUnpackAlignment);

    glBindTexture(GL_TEXTURE_2D, g_hudTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        clatasha::kHudPixelWidth,
        clatasha::kHudPixelHeight,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        snapshot.rgba);

    glPixelStorei(
        GL_UNPACK_ALIGNMENT,
        oldUnpackAlignment);
    glBindTexture(
        GL_TEXTURE_2D,
        static_cast<GLuint>(oldTexture));

    g_lastHudTelemetrySequence = sequence;
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
    if (!ShouldDrawOverlay(dc))
        return;

    if (!EnsureModernHudRenderer()) {
        InterlockedExchange(&g_shared->renderMode, 2);
        DrawFallbackMarker();

        InterlockedIncrement64(&g_shared->drawCount);
        InterlockedExchange64(
            &g_shared->lastDrawTick,
            static_cast<LONG64>(GetTickCount64()));
        return;
    }

    InterlockedExchange(&g_shared->renderMode, 1);
    UpdateHudTextureFromTelemetry();

    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] < kHudWidth + kHudMargin * 2 ||
        viewport[3] < kHudHeight + kHudMargin * 2) {
        return;
    }

    const bool placeRight =
        g_hudLocation == clatasha::HudTopRight ||
        g_hudLocation == clatasha::HudBottomRight;
    const bool placeTop =
        g_hudLocation == clatasha::HudTopLeft ||
        g_hudLocation == clatasha::HudTopRight;

    const GLfloat leftPx =
        placeRight
            ? static_cast<GLfloat>(
                  viewport[2] - kHudWidth - kHudMargin)
            : static_cast<GLfloat>(kHudMargin);
    const GLfloat rightPx =
        leftPx + static_cast<GLfloat>(kHudWidth);
    const GLfloat bottomPx =
        placeTop
            ? static_cast<GLfloat>(
                  viewport[3] - kHudMargin - kHudHeight)
            : static_cast<GLfloat>(kHudMargin);
    const GLfloat topPx =
        bottomPx + static_cast<GLfloat>(kHudHeight);

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

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    g_glUseProgram(g_hudProgram);
    g_glBindVertexArray(g_hudVao);
    g_glBindBuffer(GL_ARRAY_BUFFER, g_hudVbo);
    g_glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GlSizePtr>(sizeof(vertices)),
        vertices,
        GL_STREAM_DRAW);

    glBindTexture(GL_TEXTURE_2D, g_hudTexture);
    g_glUniform1i(g_hudSampler, 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);

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
        if (g_hudTelemetry) {
            UnmapViewOfFile(g_hudTelemetry);
            g_hudTelemetry = nullptr;
        }
        if (g_hudTelemetryMapping) {
            CloseHandle(g_hudTelemetryMapping);
            g_hudTelemetryMapping = nullptr;
        }
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
