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
    if (const HWND foreground = GetForegroundWindow())
        GetWindowThreadProcessId(foreground, &foregroundPid);

    const std::string foregroundRenderer =
        RendererTagForProcess(foregroundPid);
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

    std::fprintf(fp, "# Clatasha HUD ETW FPS: DXGI,D3D9,DXGKRNL\n");

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

        if (bestFps > 0.0) {
            if (pid == foregroundPid && !foregroundRenderer.empty()) {
                std::fprintf(
                    fp,
                    "%lu,%.3f,%s\n",
                    pid,
                    bestFps,
                    foregroundRenderer.c_str());
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
            "%lu,0.000,%s\n",
            foregroundPid,
            foregroundRenderer.c_str());
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
