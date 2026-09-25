#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <evntrace.h>
#include <evntcons.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr ULONG kRealTimeMode = EVENT_TRACE_REAL_TIME_MODE;
constexpr ULONG kProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
constexpr ULONGLONG kRuntimePresentKeyword = 0x2ULL;
constexpr ULONGLONG kDxgKrnlPresentKeyword = 0x08000001ULL;
constexpr UCHAR kTraceLevel = 5;
constexpr USHORT kDxgiPresentStartEventId = 0x002a;
constexpr USHORT kDxgiMpoPresentStartEventId = 0x0037;
constexpr USHORT kD3d9PresentStartEventId = 0x0001;
constexpr USHORT kDxgKrnlPresentHistoryStartEventId = 0x00ab;
constexpr USHORT kDxgKrnlPresentHistoryDetailedStartEventId = 0x00d7;
constexpr DWORD kSampleMs = 250;
constexpr double kWindowMs = 1000.0;

const GUID kDxgiProvider =
    {0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};
const GUID kD3d9Provider =
    {0x783ACA0A, 0x790E, 0x4D7F, {0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9}};
const GUID kDxgKrnlProvider =
    {0x802EC45A, 0x1E99, 0x4B83, {0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D}};

enum class PresentStream : size_t {
    Dxgi = 0,
    D3d9,
    DxgKrnlHistory,
    DxgKrnlDetailed,
    Count,
};

constexpr size_t kPresentStreamCount =
    static_cast<size_t>(PresentStream::Count);

std::mutex gMutex;
std::map<DWORD, std::array<std::uint64_t, kPresentStreamCount>> gCounts;
std::atomic<bool> gStopping{false};
TRACEHANDLE gSession = 0;
TRACEHANDLE gTrace = INVALID_PROCESSTRACE_HANDLE;

struct RateState {
    std::deque<std::pair<ULONGLONG, std::uint64_t>> samples;
};

using RateStreams = std::array<RateState, kPresentStreamCount>;
std::map<DWORD, RateStreams> gRates;

void CountPresent(DWORD pid, PresentStream stream)
{
    if (pid == 0)
        return;

    std::scoped_lock lock(gMutex);
    ++gCounts[pid][static_cast<size_t>(stream)];
}

void WINAPI OnEvent(PEVENT_RECORD record)
{
    if (!record)
        return;

    const auto &header = record->EventHeader;
    const USHORT eventId = header.EventDescriptor.Id;

    if (IsEqualGUID(header.ProviderId, kDxgiProvider)) {
        if (eventId == kDxgiPresentStartEventId ||
            eventId == kDxgiMpoPresentStartEventId) {
            CountPresent(header.ProcessId, PresentStream::Dxgi);
        }
        return;
    }

    if (IsEqualGUID(header.ProviderId, kD3d9Provider)) {
        if (eventId == kD3d9PresentStartEventId)
            CountPresent(header.ProcessId, PresentStream::D3d9);
        return;
    }

    if (IsEqualGUID(header.ProviderId, kDxgKrnlProvider)) {
        if (eventId == kDxgKrnlPresentHistoryStartEventId) {
            CountPresent(header.ProcessId, PresentStream::DxgKrnlHistory);
        } else if (eventId == kDxgKrnlPresentHistoryDetailedStartEventId) {
            CountPresent(header.ProcessId, PresentStream::DxgKrnlDetailed);
        }
    }
}

std::vector<BYTE> MakeProperties(const std::wstring &sessionName)
{
    const ULONG nameBytes = static_cast<ULONG>((sessionName.size() + 1) * sizeof(wchar_t));
    const ULONG total = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes + sizeof(wchar_t);

    std::vector<BYTE> buffer(total);
    auto *props = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(buffer.data());

    props->Wnode.BufferSize = total;
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 1;
    props->BufferSize = 64;
    props->MinimumBuffers = 8;
    props->MaximumBuffers = 64;
    props->LogFileMode = kRealTimeMode;
    props->FlushTimer = 1;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);

    auto *name = reinterpret_cast<wchar_t *>(buffer.data() + props->LoggerNameOffset);
    wcscpy_s(name, sessionName.size() + 1, sessionName.c_str());
    return buffer;
}

void StopSession(const std::wstring &sessionName)
{
    auto buffer = MakeProperties(sessionName);
    auto *props = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(buffer.data());
    ControlTraceW(0, sessionName.c_str(), props, EVENT_TRACE_CONTROL_STOP);
}

bool StartEtw(const std::wstring &sessionName)
{
    StopSession(sessionName);

    auto buffer = MakeProperties(sessionName);
    auto *props = reinterpret_cast<EVENT_TRACE_PROPERTIES *>(buffer.data());

    ULONG rc = StartTraceW(&gSession, sessionName.c_str(), props);
    if (rc != ERROR_SUCCESS)
        return false;

    bool providerEnabled = false;

    const auto enableProvider =
        [&](const GUID &provider, ULONGLONG keyword) {
            const ULONG enableRc =
                EnableTraceEx2(gSession,
                               &provider,
                               EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                               kTraceLevel,
                               keyword,
                               0,
                               0,
                               nullptr);
            if (enableRc == ERROR_SUCCESS)
                providerEnabled = true;
        };

    // Runtime Present_Start events cover the common DXGI/D3D paths.
    enableProvider(kDxgiProvider, kRuntimePresentKeyword);
    enableProvider(kD3d9Provider, kRuntimePresentKeyword);

    // DxgKrnl PresentHistory covers presentation paths that do not surface
    // through the DXGI provider, including many modern/Vulkan game paths.
    enableProvider(kDxgKrnlProvider, kDxgKrnlPresentKeyword);

    if (!providerEnabled) {
        StopSession(sessionName);
        return false;
    }

    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LoggerName = const_cast<LPWSTR>(sessionName.c_str());
    logfile.ProcessTraceMode = kProcessTraceMode;
    logfile.EventRecordCallback = OnEvent;

    gTrace = OpenTraceW(&logfile);
    if (gTrace == INVALID_PROCESSTRACE_HANDLE) {
        StopSession(sessionName);
        return false;
    }

    return true;
}

bool IsFullscreenForegroundWindow(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd))
        return false;

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CHILD) != 0)
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

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo))
        return false;

    constexpr LONG tolerance = 3;
    const RECT &screen = monitorInfo.rcMonitor;

    return topLeft.x <= screen.left + tolerance &&
           topLeft.y <= screen.top + tolerance &&
           bottomRight.x >= screen.right - tolerance &&
           bottomRight.y >= screen.bottom - tolerance;
}

constexpr std::uint32_t kOpenGlSharedMagic = 0x4C474F43;
constexpr std::uint32_t kOpenGlSharedVersion = 6;

struct alignas(8) OpenGlPresentShared {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t pid;
    volatile LONG liveFpsAck;
    volatile LONG64 presentCount;
    volatile LONG64 lastPresentTick;
    volatile LONG hookState;
    volatile LONG hookedMask;
    volatile LONG drawMarker;
    volatile LONG reservedControl; // draw-stage diagnostic
    volatile LONG64 drawCount;
    volatile LONG64 lastDrawTick;
    volatile LONG renderMode;
    volatile LONG liveFpsInput;
    volatile LONG liveSessionSeconds;
    volatile LONG liveSessionSecondsAck;
    volatile LONG liveAudioLevels;
    volatile LONG liveAudioLevelsAck;
    volatile LONG liveHudStatus;
    volatile LONG liveHudStatusAck;
};

struct OpenGlRateState {
    std::uint64_t lastCount = 0;
    ULONGLONG lastSampleMs = 0;
    double fps = 0.0;
    bool initialized = false;
};

std::map<DWORD, OpenGlRateState> gOpenGlRates;
std::map<DWORD, std::string> gOpenGlInjectStatus;
std::map<DWORD, int> gOpenGlInjectAttempts;
std::map<DWORD, ULONGLONG> gOpenGlInjectLastAttempt;

std::wstring ProcessBaseName(DWORD pid)
{
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        pid);
    if (!process)
        return {};

    wchar_t path[MAX_PATH] = {};
    DWORD length = MAX_PATH;
    std::wstring result;

    if (QueryFullProcessImageNameW(process, 0, path, &length)) {
        result.assign(path, length);
        const size_t separator = result.find_last_of(L"\\/");
        if (separator != std::wstring::npos)
            result.erase(0, separator + 1);
    }

    CloseHandle(process);
    return result;
}

bool IsNative64BitProcess(DWORD pid)
{
#if defined(_WIN64)
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        pid);
    if (!process)
        return false;

    BOOL wow64 = FALSE;
    const BOOL ok = IsWow64Process(process, &wow64);
    CloseHandle(process);
    return ok && !wow64;
#else
    (void)pid;
    return false;
#endif
}

std::wstring OpenGlHookDllPath()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD length =
        GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};

    std::wstring result(path, length);
    const size_t separator = result.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
        return {};

    result.erase(separator + 1);
    result += L"clatasha-opengl-present-hook.dll";
    return result;
}

bool InjectLibrary(DWORD pid, const std::wstring &dllPath)
{
    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD |
            PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE |
            PROCESS_VM_READ,
        FALSE,
        pid);
    if (!process)
        return false;

    const SIZE_T bytes =
        (dllPath.size() + 1) * sizeof(wchar_t);
    void *remotePath = VirtualAllocEx(
        process,
        nullptr,
        bytes,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);

    if (!remotePath) {
        CloseHandle(process);
        return false;
    }

    SIZE_T written = 0;
    const bool wrote =
        WriteProcessMemory(
            process,
            remotePath,
            dllPath.c_str(),
            bytes,
            &written) &&
        written == bytes;

    auto loadLibrary =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(
                GetModuleHandleW(L"kernel32.dll"),
                "LoadLibraryW"));

    HANDLE thread = nullptr;
    if (wrote && loadLibrary) {
        thread = CreateRemoteThread(
            process,
            nullptr,
            0,
            loadLibrary,
            remotePath,
            0,
            nullptr);
    }

    bool loaded = false;
    if (thread) {
        if (WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0) {
            DWORD moduleResult = 0;
            if (GetExitCodeThread(thread, &moduleResult) &&
                moduleResult != 0) {
                loaded = true;
            }
        }
        CloseHandle(thread);
    }

    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return loaded;
}

void MaybeInjectOpenGlHook(
    DWORD pid,
    const std::string &renderer)
{
    if (pid == 0 || renderer != "OGL")
        return;

    const std::wstring processName = ProcessBaseName(pid);
    if (_wcsicmp(processName.c_str(), L"Allumeria.exe") != 0)
        return;

    if (!IsNative64BitProcess(pid)) {
        gOpenGlInjectStatus[pid] = "x64-required";
        return;
    }

    const std::string current = gOpenGlInjectStatus[pid];
    if (current == "loaded" || current == "active")
        return;

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lastAttempt = gOpenGlInjectLastAttempt[pid];
    if (lastAttempt != 0 && now - lastAttempt < 1500)
        return;

    int &attempts = gOpenGlInjectAttempts[pid];
    if (attempts >= 3) {
        gOpenGlInjectStatus[pid] = "failed";
        return;
    }

    ++attempts;
    gOpenGlInjectLastAttempt[pid] = now;

    const std::wstring dllPath = OpenGlHookDllPath();
    const DWORD attributes =
        dllPath.empty()
            ? INVALID_FILE_ATTRIBUTES
            : GetFileAttributesW(dllPath.c_str());

    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        gOpenGlInjectStatus[pid] = "dll-missing";
        return;
    }

    gOpenGlInjectStatus[pid] =
        InjectLibrary(pid, dllPath)
            ? "loaded"
            : (attempts >= 3 ? "failed" : "retrying");
}

struct OpenGlSample {
    double fps = 0.0;
    bool validFps = false;
    std::string status = "off";
    LONG hookedMask = 0;
    bool drawArmed = false;
    std::uint64_t drawCount = 0;
    LONG renderMode = 0;
    LONG drawStage = 0;
    LONG liveFpsSent = 0;
    LONG liveFpsAck = 0;
    LONG liveObsFpsSent = 0;
    LONG liveObsFpsAck = 0;
    LONG liveTimerSent = 0;
    LONG liveTimerAck = 0;
    LONG liveAudioSent = 0;
    LONG liveAudioAck = 0;
    LONG liveStatusSent = 0;
    LONG liveStatusAck = 0;
};

OpenGlSample ReadOpenGlSample(DWORD pid, ULONGLONG now, bool armOverlay)
{
    OpenGlSample result;
    if (pid == 0)
        return result;

    auto injectIt = gOpenGlInjectStatus.find(pid);
    if (injectIt != gOpenGlInjectStatus.end())
        result.status = injectIt->second;

    wchar_t name[96] = {};
    swprintf_s(name, L"Local\\ClatashaHUD_OGL_%lu", pid);

    HANDLE mapping = OpenFileMappingW(
        FILE_MAP_READ | FILE_MAP_WRITE,
        FALSE,
        name);
    if (!mapping)
        return result;

    auto *shared =
        static_cast<OpenGlPresentShared *>(
            MapViewOfFile(
                mapping,
                FILE_MAP_READ | FILE_MAP_WRITE,
                0,
                0,
                sizeof(OpenGlPresentShared)));
    if (!shared) {
        CloseHandle(mapping);
        return result;
    }

    if (shared->magic == kOpenGlSharedMagic &&
        shared->version == kOpenGlSharedVersion &&
        shared->pid == pid) {
        InterlockedExchange(
            &shared->drawMarker,
            armOverlay ? 1 : 0);

        const LONG state = shared->hookState;
        result.hookedMask = shared->hookedMask;
        result.drawArmed = shared->drawMarker != 0;
        result.drawCount =
            static_cast<std::uint64_t>(shared->drawCount);
        result.renderMode = shared->renderMode;
        result.drawStage = shared->reservedControl;
        const LONG packedAck =
            InterlockedCompareExchange(
                &shared->liveFpsAck,
                0,
                0);
        result.liveFpsAck = packedAck & 0xFFFF;
        result.liveObsFpsAck =
            static_cast<LONG>(
                (static_cast<std::uint32_t>(packedAck) >> 16) &
                0xFFFFu);
        result.liveTimerSent =
            InterlockedCompareExchange(
                &shared->liveSessionSeconds,
                0,
                0);
        result.liveTimerAck =
            InterlockedCompareExchange(
                &shared->liveSessionSecondsAck,
                0,
                0);
        result.liveAudioSent =
            InterlockedCompareExchange(
                &shared->liveAudioLevels,
                0,
                0);
        result.liveAudioAck =
            InterlockedCompareExchange(
                &shared->liveAudioLevelsAck,
                0,
                0);
        result.liveStatusSent =
            InterlockedCompareExchange(
                &shared->liveHudStatus,
                0,
                0);
        result.liveStatusAck =
            InterlockedCompareExchange(
                &shared->liveHudStatusAck,
                0,
                0);

        if (state == 1) {
            result.status = "active";
            gOpenGlInjectStatus[pid] = "active";
        } else if (state == 2) {
            result.status = "hook-failed";
        } else {
            result.status = "loading";
        }

        const std::uint64_t count =
            static_cast<std::uint64_t>(shared->presentCount);
        const ULONGLONG lastPresent =
            static_cast<ULONGLONG>(shared->lastPresentTick);

        auto &rate = gOpenGlRates[pid];
        if (!rate.initialized) {
            rate.lastCount = count;
            rate.lastSampleMs = now;
            rate.initialized = true;
        } else {
            const ULONGLONG elapsed = now - rate.lastSampleMs;
            if (elapsed >= 200 && count >= rate.lastCount) {
                rate.fps =
                    static_cast<double>(count - rate.lastCount) *
                    1000.0 /
                    static_cast<double>(elapsed);
                rate.lastCount = count;
                rate.lastSampleMs = now;
            }
        }

        if (state == 1 &&
            lastPresent != 0 &&
            now >= lastPresent &&
            now - lastPresent <= 1500 &&
            rate.fps > 0.0 &&
            rate.fps < 2000.0) {
            result.fps = rate.fps;
            result.validFps = true;
        }

        result.liveFpsSent =
            result.validFps
                ? static_cast<LONG>(result.fps + 0.5)
                : 0;

        LONG packedInput = 0;
        LONG nextPackedInput = 0;
        do {
            packedInput =
                InterlockedCompareExchange(
                    &shared->liveFpsInput,
                    0,
                    0);
            const LONG obsBits =
                packedInput & static_cast<LONG>(0xFFFF0000u);
            nextPackedInput =
                obsBits |
                (result.liveFpsSent & 0xFFFF);
        } while (
            InterlockedCompareExchange(
                &shared->liveFpsInput,
                nextPackedInput,
                packedInput) != packedInput);

        result.liveObsFpsSent =
            (nextPackedInput >> 16) & 0xFFFF;
    }

    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return result;
}

std::string RendererTagForProcess(DWORD pid)
{
    if (pid == 0)
        return {};

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
        pid);
    if (snapshot == INVALID_HANDLE_VALUE)
        return {};

    bool hasVulkan = false;
    bool hasOpenGl = false;
    bool hasD3d12 = false;
    bool hasD3d11 = false;
    bool hasD3d10 = false;
    bool hasD3d9 = false;
    bool hasDdraw = false;

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);

    if (Module32FirstW(snapshot, &module)) {
        do {
            const wchar_t *name = module.szModule;
            if (_wcsicmp(name, L"vulkan-1.dll") == 0)
                hasVulkan = true;
            else if (_wcsicmp(name, L"opengl32.dll") == 0)
                hasOpenGl = true;
            else if (_wcsicmp(name, L"d3d12.dll") == 0)
                hasD3d12 = true;
            else if (_wcsicmp(name, L"d3d11.dll") == 0)
                hasD3d11 = true;
            else if (_wcsicmp(name, L"d3d10.dll") == 0 ||
                     _wcsicmp(name, L"d3d10_1.dll") == 0)
                hasD3d10 = true;
            else if (_wcsicmp(name, L"d3d9.dll") == 0)
                hasD3d9 = true;
            else if (_wcsicmp(name, L"ddraw.dll") == 0)
                hasDdraw = true;
        } while (Module32NextW(snapshot, &module));
    }

    CloseHandle(snapshot);

    // Prefer explicit graphics-runtime modules over shared DXGI/kernel pieces.
    // This is renderer detection only; presentation-hook confirmation comes
    // later when the API-specific backends are added.
    if (hasVulkan)
        return "VK";
    if (hasOpenGl)
        return "OGL";
    if (hasD3d12)
        return "D12";
    if (hasD3d11)
        return "D11";
    if (hasD3d10)
        return "D10";
    if (hasD3d9)
        return "D9";
    if (hasDdraw)
        return "DD";

    return {};
}

void WriteStateFile(const std::wstring &path)
{
    const ULONGLONG now = GetTickCount64();

    DWORD foregroundPid = 0;
    const HWND foreground = GetForegroundWindow();
    if (foreground)
        GetWindowThreadProcessId(foreground, &foregroundPid);

    const bool foregroundFullscreen =
        IsFullscreenForegroundWindow(foreground);
    const std::string foregroundRenderer =
        RendererTagForProcess(foregroundPid);

    MaybeInjectOpenGlHook(
        foregroundPid,
        foregroundRenderer);
    const OpenGlSample openGlSample =
        ReadOpenGlSample(
            foregroundPid,
            now,
            foregroundRenderer == "OGL" &&
                foregroundFullscreen);

    bool foregroundWritten = false;
    std::map<DWORD, std::array<std::uint64_t, kPresentStreamCount>> counts;

    {
        std::scoped_lock lock(gMutex);
        counts = gCounts;
    }

    for (const auto &[pid, streamCounts] : counts) {
        auto &streams = gRates[pid];

        for (size_t streamIndex = 0;
             streamIndex < kPresentStreamCount;
             ++streamIndex) {
            auto &state = streams[streamIndex];
            state.samples.emplace_back(now, streamCounts[streamIndex]);

            while (state.samples.size() > 2 &&
                   static_cast<double>(now - state.samples.front().first) > kWindowMs) {
                state.samples.pop_front();
            }
        }
    }

    const std::wstring tmpPath = path + L".tmp";
    FILE *fp = nullptr;
    if (_wfopen_s(&fp, tmpPath.c_str(), L"wb") != 0 || !fp)
        return;

    std::fprintf(fp, "# pid,fps,renderer,fullscreen,ogl_hook,ogl_mask,ogl_draw,ogl_draws,ogl_render,ogl_stage,ogl_fps_tx,ogl_fps_rx,ogl_obs_tx,ogl_obs_rx,ogl_timer_tx,ogl_timer_rx,ogl_audio_tx,ogl_audio_rx,ogl_status_tx,ogl_status_rx\n");

    for (const auto &[pid, streams] : gRates) {
        double bestFps = 0.0;

        for (const auto &state : streams) {
            if (state.samples.size() < 2)
                continue;

            const auto &first = state.samples.front();
            const auto &last = state.samples.back();
            const ULONGLONG spanMs = last.first - first.first;
            if (spanMs < 120 || last.second < first.second)
                continue;

            const double fps =
                static_cast<double>(last.second - first.second) * 1000.0 /
                static_cast<double>(spanMs);

            // The same present can appear in more than one ETW provider.
            // Keep independent stream rates and use the strongest valid one
            // instead of summing and accidentally doubling the FPS.
            if (fps > bestFps && fps < 2000.0)
                bestFps = fps;
        }

        if (pid == foregroundPid &&
            foregroundRenderer == "OGL" &&
            openGlSample.validFps) {
            bestFps = openGlSample.fps;
        }

        if (bestFps > 0.0) {
            if (pid == foregroundPid && !foregroundRenderer.empty()) {
                std::fprintf(
                    fp,
                    "%lu,%.3f,%s,%d,%s,%ld,%d,%llu,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n",
                    pid,
                    bestFps,
                    foregroundRenderer.c_str(),
                    foregroundFullscreen ? 1 : 0,
                    openGlSample.status.c_str(),
                    openGlSample.hookedMask,
                    openGlSample.drawArmed ? 1 : 0,
                    static_cast<unsigned long long>(openGlSample.drawCount),
                    openGlSample.renderMode,
                    openGlSample.drawStage,
                    openGlSample.liveFpsSent,
                    openGlSample.liveFpsAck,
                    openGlSample.liveObsFpsSent,
                    openGlSample.liveObsFpsAck,
                    openGlSample.liveTimerSent,
                    openGlSample.liveTimerAck,
                    openGlSample.liveAudioSent,
                    openGlSample.liveAudioAck,
                    openGlSample.liveStatusSent,
                    openGlSample.liveStatusAck);
                foregroundWritten = true;
            } else {
                std::fprintf(fp, "%lu,%.3f\n", pid, bestFps);
            }
        }
    }

    // API detection is useful even when ETW cannot calculate FPS yet. This is
    // the expected first-stage behavior for OpenGL while its present hook is
    // still under development.
    if (foregroundPid != 0 &&
        !foregroundRenderer.empty() &&
        !foregroundWritten) {
        std::fprintf(
            fp,
            "%lu,%.3f,%s,%d,%s,%ld,%d,%llu,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n",
            foregroundPid,
            openGlSample.validFps ? openGlSample.fps : 0.0,
            foregroundRenderer.c_str(),
            foregroundFullscreen ? 1 : 0,
            openGlSample.status.c_str(),
            openGlSample.hookedMask,
            openGlSample.drawArmed ? 1 : 0,
            static_cast<unsigned long long>(openGlSample.drawCount),
            openGlSample.renderMode,
            openGlSample.drawStage,
            openGlSample.liveFpsSent,
            openGlSample.liveFpsAck,
            openGlSample.liveObsFpsSent,
            openGlSample.liveObsFpsAck,
            openGlSample.liveTimerSent,
            openGlSample.liveTimerAck,
            openGlSample.liveAudioSent,
            openGlSample.liveAudioAck,
            openGlSample.liveStatusSent,
            openGlSample.liveStatusAck);
    }

    std::fclose(fp);

    MoveFileExW(tmpPath.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    if (argc != 3)
        return 2;

    const std::wstring statePath = argv[1];
    const DWORD parentPid = static_cast<DWORD>(_wtoi(argv[2]));
    const std::wstring sessionName =
        L"ClatashaHUD_DXGI_" + std::to_wstring(parentPid);

    if (!StartEtw(sessionName))
        return 3;

    std::thread traceThread([]() {
        ProcessTrace(&gTrace, 1, nullptr, nullptr);
    });

    HANDLE parent = nullptr;
    if (parentPid != 0)
        parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);

    while (!gStopping.load()) {
        if (parent && WaitForSingleObject(parent, 0) != WAIT_TIMEOUT)
            break;

        WriteStateFile(statePath);
        Sleep(kSampleMs);
    }

    gStopping.store(true);
    StopSession(sessionName);

    if (gTrace != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(gTrace);
        gTrace = INVALID_PROCESSTRACE_HANDLE;
    }

    if (traceThread.joinable())
        traceThread.join();

    if (parent)
        CloseHandle(parent);

    DeleteFileW(statePath.c_str());
    DeleteFileW((statePath + L".tmp").c_str());
    return 0;
}
