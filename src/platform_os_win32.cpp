#include "dcmake.hpp"
#include "platform_win32_util.hpp"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <string>

// Win32 OS half of the platform layer.  Paired with
// platform_gui_win32.cpp for native Windows (MSVC / MinGW) builds.
// Contains gdb subprocess launch via CreateProcessW + stdio pipes,
// job-object process-tree cleanup, and wide-char filesystem helpers.
// No windowing / dialog code lives here.

// --- Platform pipe implementation ---

struct Win32Platform {
    HANDLE gdb_stdin_write = INVALID_HANDLE_VALUE;
    HANDLE gdb_stdout_read = INVALID_HANDLE_VALUE;
    HANDLE job = INVALID_HANDLE_VALUE;
    HANDLE gdb_process = INVALID_HANDLE_VALUE;
};

static int win32_pipe_read(void *ctx, char *buf, int len)
{
    auto *p = (Win32Platform *)ctx;
    DWORD n = 0;
    if (ReadFile(p->gdb_stdout_read, buf, (DWORD)len, &n, nullptr))
        return (int)n;
    return 0;
}

static bool win32_pipe_write(void *ctx, const char *buf, int len)
{
    auto *p = (Win32Platform *)ctx;
    DWORD n = 0;
    return WriteFile(p->gdb_stdin_write, buf, (DWORD)len, &n, nullptr) &&
           (int)n == len;
}

static void win32_pipe_shutdown(void *ctx)
{
    auto *p = (Win32Platform *)ctx;
    if (p->gdb_stdin_write != INVALID_HANDLE_VALUE) {
        CloseHandle(p->gdb_stdin_write);
        p->gdb_stdin_write = INVALID_HANDLE_VALUE;
    }
    if (p->job != INVALID_HANDLE_VALUE) {
        TerminateJobObject(p->job, 1);
    }
}

// Skip past argv[0] in the raw command line to get the arguments portion.
// If the first character is a double quote, skip to the closing double quote.
// Otherwise skip to the first space or tab. Then trim leading whitespace.
std::string platform_quote_argv(int, char **)
{
    const wchar_t *cmd = GetCommandLineW();
    if (*cmd == L'"') {
        cmd++;
        while (*cmd && *cmd != L'"') cmd++;
        if (*cmd) cmd++;
    } else {
        while (*cmd && *cmd != L' ' && *cmd != L'\t') cmd++;
    }
    while (*cmd == L' ' || *cmd == L'\t') cmd++;
    return to_utf8(cmd);
}

bool platform_launch(Debugger *dbg, const char *args)
{
    auto *p = new Win32Platform;
    dbg->platform = p;
    dbg->pipe_read = win32_pipe_read;
    dbg->pipe_write = win32_pipe_write;
    dbg->pipe_shutdown = win32_pipe_shutdown;
    dbg->stdout_read = nullptr;
    dbg->stdout_shutdown = nullptr;

    std::wstring cmdline = L"gdb.exe -nx -q --interpreter=mi --args ";
    cmdline += to_wide(args);

    // Create job object so the entire process tree dies together.
    // KILL_ON_JOB_CLOSE ensures cleanup even if ggdb crashes.
    p->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
    jeli.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(p->job, JobObjectExtendedLimitInformation,
                            &jeli, sizeof(jeli));

    // Create pipes for GDB's MI stdin/stdout. Stderr joins stdout so all
    // protocol diagnostics are visible in the MI log/output path.
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE stdin_read_h = INVALID_HANDLE_VALUE;
    HANDLE stdin_write_h = INVALID_HANDLE_VALUE;
    HANDLE stdout_read_h = INVALID_HANDLE_VALUE;
    HANDLE stdout_write_h = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&stdin_read_h, &stdin_write_h, &sa, 0) ||
        !CreatePipe(&stdout_read_h, &stdout_write_h, &sa, 0)) {
        dbg->status = "Failed to create GDB pipes";
        return false;
    }
    SetHandleInformation(stdin_write_h, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdout_read_h, HANDLE_FLAG_INHERIT, 0);

    // Launch GDB suspended, assign to job, then resume.
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read_h;
    si.hStdOutput = stdout_write_h;
    si.hStdError = stdout_write_h;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr,
                        TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW,
                        nullptr, nullptr,
                        &si, &pi)) {
        CloseHandle(stdin_read_h);
        CloseHandle(stdin_write_h);
        CloseHandle(stdout_read_h);
        CloseHandle(stdout_write_h);
        dbg->status = "Failed to start gdb.exe";
        return false;
    }
    CloseHandle(stdin_read_h);
    CloseHandle(stdout_write_h);
    p->gdb_stdin_write = stdin_write_h;
    p->gdb_stdout_read = stdout_read_h;
    AssignProcessToJobObject(p->job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    p->gdb_process = pi.hProcess;

    return true;
}

void platform_cleanup(Debugger *dbg)
{
    auto *p = (Win32Platform *)dbg->platform;
    if (!p) return;

    if (p->job != INVALID_HANDLE_VALUE) {
        TerminateJobObject(p->job, 1);
        if (p->gdb_process != INVALID_HANDLE_VALUE) {
            WaitForSingleObject(p->gdb_process, 3000);
        }
        CloseHandle(p->job);
        p->job = INVALID_HANDLE_VALUE;
    }
    if (p->gdb_stdin_write != INVALID_HANDLE_VALUE) {
        CloseHandle(p->gdb_stdin_write);
        p->gdb_stdin_write = INVALID_HANDLE_VALUE;
    }
    if (p->gdb_stdout_read != INVALID_HANDLE_VALUE) {
        CloseHandle(p->gdb_stdout_read);
        p->gdb_stdout_read = INVALID_HANDLE_VALUE;
    }
    if (p->gdb_process != INVALID_HANDLE_VALUE) {
        CloseHandle(p->gdb_process);
        p->gdb_process = INVALID_HANDLE_VALUE;
    }

    delete p;
    dbg->platform = nullptr;
}

std::string platform_now_iso8601()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    TIME_ZONE_INFORMATION tz;
    DWORD mode = GetTimeZoneInformation(&tz);
    int bias = -(int)tz.Bias;
    if (mode == TIME_ZONE_ID_DAYLIGHT)
        bias -= (int)tz.DaylightBias;
    char sign = bias >= 0 ? '+' : '-';
    if (bias < 0) bias = -bias;
    char buf[64];
    int n = snprintf(buf, sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d:%02d",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        sign, bias / 60, bias % 60);
    return std::string(buf, (size_t)n);
}

bool platform_chdir(const char *path)
{
    return SetCurrentDirectoryW(to_wide(path).c_str());
}

std::string platform_read_file(const char *path)
{
    HANDLE h = CreateFileW(to_wide(path).c_str(), GENERIC_READ,
                           FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        CloseHandle(h);
        return {};
    }
    std::string out((size_t)size.QuadPart, '\0');
    DWORD got = 0;
    ReadFile(h, out.data(), (DWORD)size.QuadPart, &got, nullptr);
    CloseHandle(h);
    out.resize(got);
    return out;
}

bool platform_write_file(const char *path, const char *data, size_t len)
{
    HANDLE h = CreateFileW(to_wide(path).c_str(), GENERIC_WRITE,
                           0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    WriteFile(h, data, (DWORD)len, &wrote, nullptr);
    CloseHandle(h);
    return wrote == (DWORD)len;
}

std::string platform_config_dir()
{
    std::wstring dir;
    wchar_t buf[MAX_PATH];
    DWORD n;
    n = GetEnvironmentVariableW(L"XDG_CONFIG_HOME", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        dir = buf;
    } else {
        n = GetEnvironmentVariableW(L"HOME", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            dir = buf;
            dir += L"/.config";
        } else {
            wchar_t appdata[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA,
                                           NULL, 0, appdata)))
                dir = appdata;
        }
    }
    if (dir.empty()) return {};
    dir += L"/ggdb";
    // Create directory and all parents (like mkdir -p).
    {
        std::wstring seg;
        for (const wchar_t *p = dir.c_str(); *p; p++) {
            if ((*p == L'/' || *p == L'\\') && !seg.empty())
                CreateDirectoryW(seg.c_str(), nullptr);
            seg += *p;
        }
        CreateDirectoryW(seg.c_str(), nullptr);
    }
    return to_utf8(dir.c_str());
}

std::string platform_realpath(const std::string &path)
{
    std::wstring wpath = to_wide(path.c_str());
    HANDLE h = CreateFileW(wpath.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return path;
    wchar_t buf[MAX_PATH];
    DWORD len = GetFinalPathNameByHandleW(h, buf, MAX_PATH,
                                          FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (len == 0 || len >= MAX_PATH) return path;
    std::wstring result(buf, len);
    if (result.starts_with(L"\\\\?\\"))
        result = result.substr(4);
    std::string out = to_utf8(result.c_str());
    for (char &c : out)
        if (c == '\\') c = '/';
    return out;
}
