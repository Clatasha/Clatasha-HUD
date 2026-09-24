#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <detours.h>

#include <cstdint>

namespace {

constexpr std::uint32_t kSharedMagic = 0x4C474F43; // "COGL"
constexpr std::uint32_t kSharedVersion = 1;

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

void SwapBegin()
{
    if (g_swapDepth++ == 0)
        RecordPresent();
}

void SwapEnd()
{
    if (g_swapDepth > 0)
        --g_swapDepth;
}

BOOL WINAPI HookedSwapBuffers(HDC dc)
{
    SwapBegin();
    const BOOL result =
        g_realSwapBuffers ? g_realSwapBuffers(dc) : FALSE;
    SwapEnd();
    return result;
}

BOOL WINAPI HookedWglSwapBuffers(HDC dc)
{
    SwapBegin();
    const BOOL result =
        g_realWglSwapBuffers ? g_realWglSwapBuffers(dc) : FALSE;
    SwapEnd();
    return result;
}

BOOL WINAPI HookedWglSwapLayerBuffers(HDC dc, UINT planes)
{
    SwapBegin();
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
    swprintf_s(
        name,
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
