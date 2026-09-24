#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <gl/GL.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstring>

namespace {

using SwapBuffersFn = BOOL (WINAPI *)(HDC);
using WglSwapLayerBuffersFn = BOOL (WINAPI *)(HDC, UINT);

SwapBuffersFn g_originalSwapBuffers = nullptr;
WglSwapLayerBuffersFn g_originalWglSwapLayerBuffers = nullptr;
std::atomic_bool g_running{true};

bool IsFullscreenOpenGlDc(HDC dc)
{
    if (!dc)
        return false;

    HWND hwnd = WindowFromDC(dc);
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd))
        return false;

    RECT client{};
    if (!GetClientRect(hwnd, &client))
        return false;

    POINT topLeft{client.left, client.top};
    POINT bottomRight{client.right, client.bottom};
    if (!ClientToScreen(hwnd, &topLeft) ||
        !ClientToScreen(hwnd, &bottomRight)) {
        return false;
    }

    const HMONITOR monitor =
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

void DrawFullscreenProbe()
{
    if (!wglGetCurrentContext())
        return;

    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] < 40 || viewport[3] < 24)
        return;

    GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint oldScissor[4] = {};
    GLfloat oldClearColor[4] = {};
    GLboolean oldColorMask[4] = {};

    glGetIntegerv(GL_SCISSOR_BOX, oldScissor);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, oldClearColor);
    glGetBooleanv(GL_COLOR_WRITEMASK, oldColorMask);

    // Small proof marker in the top-right corner. glClear + scissor works in
    // both legacy and modern/core OpenGL contexts and avoids shader/state
    // dependencies for this first fullscreen test.
    glEnable(GL_SCISSOR_TEST);
    glScissor(
        viewport[0] + viewport[2] - 38,
        viewport[1] + viewport[3] - 14,
        30,
        7);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.15f, 1.0f, 0.35f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glColorMask(
        oldColorMask[0],
        oldColorMask[1],
        oldColorMask[2],
        oldColorMask[3]);
    glClearColor(
        oldClearColor[0],
        oldClearColor[1],
        oldClearColor[2],
        oldClearColor[3]);
    glScissor(
        oldScissor[0],
        oldScissor[1],
        oldScissor[2],
        oldScissor[3]);

    if (!scissorEnabled)
        glDisable(GL_SCISSOR_TEST);
}

BOOL WINAPI HookSwapBuffers(HDC dc)
{
    if (IsFullscreenOpenGlDc(dc))
        DrawFullscreenProbe();

    return g_originalSwapBuffers
               ? g_originalSwapBuffers(dc)
               : FALSE;
}

BOOL WINAPI HookWglSwapLayerBuffers(HDC dc, UINT planes)
{
    if (IsFullscreenOpenGlDc(dc))
        DrawFullscreenProbe();

    return g_originalWglSwapLayerBuffers
               ? g_originalWglSwapLayerBuffers(dc, planes)
               : FALSE;
}

bool PatchImportInModule(
    HMODULE module,
    const char *functionName,
    void *replacement,
    void **original)
{
    if (!module || !functionName || !replacement || !original)
        return false;

    auto *base = reinterpret_cast<unsigned char *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const IMAGE_DATA_DIRECTORY &directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return false;

    auto *descriptor =
        reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(
            base + directory.VirtualAddress);

    for (; descriptor->Name; ++descriptor) {
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk)
            continue;

        auto *names =
            reinterpret_cast<IMAGE_THUNK_DATA *>(
                base + descriptor->OriginalFirstThunk);
        auto *iat =
            reinterpret_cast<IMAGE_THUNK_DATA *>(
                base + descriptor->FirstThunk);

        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;

            auto *import =
                reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(
                    base + names->u1.AddressOfData);
            if (std::strcmp(
                    reinterpret_cast<const char *>(import->Name),
                    functionName) != 0) {
                continue;
            }

            void **slot = reinterpret_cast<void **>(&iat->u1.Function);
            if (*slot == replacement)
                return true;

            DWORD oldProtect = 0;
            if (!VirtualProtect(
                    slot,
                    sizeof(void *),
                    PAGE_READWRITE,
                    &oldProtect)) {
                return false;
            }

            if (!*original)
                *original = *slot;
            *slot = replacement;

            DWORD ignored = 0;
            VirtualProtect(
                slot,
                sizeof(void *),
                oldProtect,
                &ignored);
            FlushInstructionCache(
                GetCurrentProcess(),
                slot,
                sizeof(void *));
            return true;
        }
    }

    return false;
}

void PatchOpenGlPresentImports()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snapshot, &entry)) {
        do {
            HMODULE module = entry.hModule;

            if (!g_originalSwapBuffers) {
                PatchImportInModule(
                    module,
                    "SwapBuffers",
                    reinterpret_cast<void *>(&HookSwapBuffers),
                    reinterpret_cast<void **>(&g_originalSwapBuffers));
            }

            if (!g_originalWglSwapLayerBuffers) {
                PatchImportInModule(
                    module,
                    "wglSwapLayerBuffers",
                    reinterpret_cast<void *>(&HookWglSwapLayerBuffers),
                    reinterpret_cast<void **>(&g_originalWglSwapLayerBuffers));
            }

            if (g_originalSwapBuffers ||
                g_originalWglSwapLayerBuffers) {
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
}

DWORD WINAPI ProbeWorker(void *)
{
    // Give the application loader a moment to settle, then retry for a short
    // period in case the renderer module is loaded just after our DLL.
    for (int attempt = 0;
         attempt < 30 && g_running.load(std::memory_order_acquire);
         ++attempt) {
        PatchOpenGlPresentImports();

        if (g_originalSwapBuffers ||
            g_originalWglSwapLayerBuffers) {
            break;
        }

        Sleep(250);
    }

    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(
            nullptr,
            0,
            &ProbeWorker,
            nullptr,
            0,
            nullptr);
        if (thread)
            CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_running.store(false, std::memory_order_release);
    }

    return TRUE;
}
