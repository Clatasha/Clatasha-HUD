#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <algorithm>
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
constexpr ULONGLONG kDxgiKeyword = 0x2ULL;
constexpr UCHAR kTraceLevel = 5;
constexpr USHORT kPresentStartEventId = 42;
constexpr DWORD kSampleMs = 250;
constexpr double kWindowMs = 1000.0;

const GUID kDxgiProvider =
    {0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};

std::mutex gMutex;
std::map<DWORD, std::uint64_t> gCounts;
std::atomic<bool> gStopping{false};
TRACEHANDLE gSession = 0;
TRACEHANDLE gTrace = INVALID_PROCESSTRACE_HANDLE;

struct RateState {
    std::deque<std::pair<ULONGLONG, std::uint64_t>> samples;
};

std::map<DWORD, RateState> gRates;

void WINAPI OnEvent(PEVENT_RECORD record)
{
    if (!record)
        return;

    const auto &header = record->EventHeader;
    if (!IsEqualGUID(header.ProviderId, kDxgiProvider))
        return;

    if (header.EventDescriptor.Id != kPresentStartEventId)
        return;

    if (header.ProcessId == 0)
        return;

    std::scoped_lock lock(gMutex);
    ++gCounts[header.ProcessId];
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

    rc = EnableTraceEx2(gSession,
                        &kDxgiProvider,
                        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                        kTraceLevel,
                        kDxgiKeyword,
                        0,
                        0,
                        nullptr);
    if (rc != ERROR_SUCCESS) {
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

void WriteStateFile(const std::wstring &path)
{
    const ULONGLONG now = GetTickCount64();
    std::map<DWORD, std::uint64_t> counts;

    {
        std::scoped_lock lock(gMutex);
        counts = gCounts;
    }

    for (const auto &[pid, count] : counts) {
        auto &state = gRates[pid];
        state.samples.emplace_back(now, count);

        while (state.samples.size() > 2 &&
               static_cast<double>(now - state.samples.front().first) > kWindowMs) {
            state.samples.pop_front();
        }
    }

    for (auto it = gRates.begin(); it != gRates.end();) {
        if (it->second.samples.empty() ||
            now - it->second.samples.back().first > 5000) {
            it = gRates.erase(it);
        } else {
            ++it;
        }
    }

    const std::wstring tmpPath = path + L".tmp";
    FILE *fp = nullptr;
    if (_wfopen_s(&fp, tmpPath.c_str(), L"wb") != 0 || !fp)
        return;

    std::fprintf(fp, "# Clatasha HUD direct ETW FPS\n");

    for (const auto &[pid, state] : gRates) {
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

        if (fps > 0.0 && fps < 2000.0)
            std::fprintf(fp, "%lu,%.3f\n", pid, fps);
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
