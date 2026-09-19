#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

static std::wstring quoteArg(const std::wstring &value)
{
    std::wstring out = L"\"";
    unsigned backslashes = 0;

    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }

        if (ch == L'\"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'\"');
            backslashes = 0;
            continue;
        }

        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(ch);
    }

    out.append(backslashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc != 4)
        return 2;

    const std::wstring presentMonPath = argv[1];
    const std::wstring pipePath = argv[2];
    const DWORD parentPid = static_cast<DWORD>(_wtoi(argv[3]));

    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 100; ++attempt) {
        pipe = CreateFileW(pipePath.c_str(),
                           GENERIC_WRITE,
                           0,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL,
                           nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            break;

        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND)
            return 3;

        WaitNamedPipeW(pipePath.c_str(), 100);
        Sleep(25);
    }

    if (pipe == INVALID_HANDLE_VALUE)
        return 4;

    SetHandleInformation(pipe, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    HANDLE nullHandle = CreateFileW(L"NUL",
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (nullHandle != INVALID_HANDLE_VALUE)
        SetHandleInformation(nullHandle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    const std::wstring session =
        L"ClatashaHUD_" + std::to_wstring(parentPid);

    std::wstring command =
        quoteArg(presentMonPath) +
        L" --output_stdout --no_console_stats --no_track_gpu --no_track_input --no_track_display" +
        L" --session_name " + session +
        L" --stop_existing_session";

    std::vector<wchar_t> commandBuffer(command.begin(), command.end());
    commandBuffer.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = pipe;
    startup.hStdError = nullHandle != INVALID_HANDLE_VALUE ? nullHandle : pipe;
    startup.hStdInput = nullHandle != INVALID_HANDLE_VALUE ? nullHandle : GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process = {};
    const BOOL started = CreateProcessW(
        presentMonPath.c_str(),
        commandBuffer.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &process);

    CloseHandle(pipe);
    if (nullHandle != INVALID_HANDLE_VALUE)
        CloseHandle(nullHandle);

    if (!started)
        return 5;

    CloseHandle(process.hThread);

    HANDLE parent = nullptr;
    if (parentPid != 0)
        parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);

    if (parent) {
        HANDLE handles[2] = {process.hProcess, parent};
        const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);

        if (result == WAIT_OBJECT_0 + 1) {
            TerminateProcess(process.hProcess, 0);
            WaitForSingleObject(process.hProcess, 2000);
        }

        CloseHandle(parent);
    } else {
        WaitForSingleObject(process.hProcess, INFINITE);
    }

    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}
