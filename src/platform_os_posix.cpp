#include "dcmake.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// POSIX OS half of the platform layer.  Paired with either
// platform_gui_glfw.cpp (macOS/Linux) or platform_gui_win32.cpp
// (Cygwin) at link time.  Contains the GDB subprocess stdio pipes,
// launch, stdout capture, and portable filesystem helpers.  No
// windowing / dialog code lives here.

// --- Platform pipe implementation ---

struct PosixPlatform {
    int gdb_stdin_fd = -1;
    int gdb_stdout_fd = -1;
    int pty_master_fd = -1;
    pid_t gdb_pid = -1;
    std::string pty_slave_path;
};

static int posix_pipe_read(void *ctx, char *buf, int len)
{
    auto *p = (PosixPlatform *)ctx;
    ssize_t n = read(p->gdb_stdout_fd, buf, (size_t)len);
    return n > 0 ? (int)n : 0;
}

static bool posix_pipe_write(void *ctx, const char *buf, int len)
{
    auto *p = (PosixPlatform *)ctx;
    return write(p->gdb_stdin_fd, buf, (size_t)len) == len;
}

static void posix_pipe_shutdown(void *ctx)
{
    auto *p = (PosixPlatform *)ctx;
    if (p->gdb_stdin_fd >= 0) {
        close(p->gdb_stdin_fd);
        p->gdb_stdin_fd = -1;
    }
    if (p->gdb_pid > 0) {
        kill(p->gdb_pid, SIGTERM);
    }
}

static int posix_stdout_read(void *ctx, char *buf, int len)
{
    auto *p = (PosixPlatform *)ctx;
    if (p->pty_master_fd < 0) return 0;
    ssize_t n = read(p->pty_master_fd, buf, (size_t)len);
    if (n > 0) return (int)n;
    if (n == 0) return 0;
    usleep(50000);
    return -1;
}

static void posix_stdout_shutdown(void *ctx)
{
    auto *p = (PosixPlatform *)ctx;
    if (p->pty_master_fd >= 0) {
        close(p->pty_master_fd);
        p->pty_master_fd = -1;
    }
}

// POSIX shell-quote: wrap in single quotes, escape embedded single quotes.
std::string platform_quote_argv(int argc, char **argv)
{
    std::string result;
    for (int i = 1; i < argc; i++) {
        if (i > 1) result += ' ';
        const char *arg = argv[i];
        // Check if quoting is needed
        bool clean = true;
        for (const char *c = arg; *c; c++) {
            if (!isalnum(*c) && *c != '/' && *c != '.' && *c != '-' &&
                *c != '_' && *c != '=' && *c != ':' && *c != '$') {
                clean = false;
                break;
            }
        }
        if (clean && *arg) {
            result += arg;
        } else {
            result += '\'';
            for (const char *c = arg; *c; c++) {
                if (*c == '\'') {
                    result += "'\\''";
                } else {
                    result += *c;
                }
            }
            result += '\'';
        }
    }
    return result;
}

bool platform_launch(Debugger *dbg, const char *args)
{
    auto *p = new PosixPlatform;
    dbg->platform = p;
    dbg->pipe_read = posix_pipe_read;
    dbg->pipe_write = posix_pipe_write;
    dbg->pipe_shutdown = posix_pipe_shutdown;
    dbg->stdout_read = posix_stdout_read;
    dbg->stdout_shutdown = posix_stdout_shutdown;

    p->pty_master_fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (p->pty_master_fd < 0 ||
        grantpt(p->pty_master_fd) != 0 ||
        unlockpt(p->pty_master_fd) != 0) {
        dbg->status = "Failed to create inferior PTY";
        return false;
    }
    char *slave = ptsname(p->pty_master_fd);
    if (!slave) {
        dbg->status = "Failed to locate inferior PTY";
        return false;
    }
    p->pty_slave_path = slave;
    dbg->inferior_tty_path = p->pty_slave_path;

    // Build shell command
    std::string cmd = "exec gdb -nx -q --interpreter=mi --args";
    if (args && *args) {
        cmd += ' ';
        cmd += args;
    }

    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0) {
        dbg->status = "Failed to create GDB pipes";
        return false;
    }

    pid_t pid = fork();
    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        execlp("sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        dbg->status = "Failed to fork gdb";
        return false;
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    p->gdb_stdin_fd = stdin_pipe[1];
    p->gdb_stdout_fd = stdout_pipe[0];
    p->gdb_pid = pid;

    return true;
}

void platform_cleanup(Debugger *dbg)
{
    auto *p = (PosixPlatform *)dbg->platform;
    if (!p) return;

    if (p->gdb_stdin_fd >= 0) {
        close(p->gdb_stdin_fd);
        p->gdb_stdin_fd = -1;
    }

    if (p->gdb_pid > 0) {
        kill(p->gdb_pid, SIGTERM);
        waitpid(p->gdb_pid, nullptr, 0);
        p->gdb_pid = -1;
    }

    if (p->gdb_stdout_fd >= 0) {
        close(p->gdb_stdout_fd);
        p->gdb_stdout_fd = -1;
    }
    if (p->pty_master_fd >= 0) {
        close(p->pty_master_fd);
        p->pty_master_fd = -1;
    }

    delete p;
    dbg->platform = nullptr;
}

std::string platform_now_iso8601()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int ms = (int)(ts.tv_nsec / 1000000);
    std::tm local;
    localtime_r(&ts.tv_sec, &local);
    char buf[64];
    int n = (int)strftime(buf, sizeof(buf), "%FT%T", &local);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, ".%03d", ms);
    char tz[8];
    strftime(tz, sizeof(tz), "%z", &local);
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%.3s:%.2s", tz, tz + 3);
    return std::string(buf, (size_t)n);
}

bool platform_chdir(const char *path)
{
    return chdir(path) == 0;
}

std::string platform_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n <= 0) { fclose(f); return {}; }
    fseek(f, 0, SEEK_SET);
    std::string out((size_t)n, '\0');
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    out.resize(got);
    return out;
}

bool platform_write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t wrote = fwrite(data, 1, len, f);
    fclose(f);
    return wrote == len;
}

std::string platform_config_dir()
{
    std::string dir;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        dir = xdg;
    } else {
        const char *home = getenv("HOME");
        dir = home ? home : ".";
        dir += "/.config";
    }
    dir += "/ggdb";
    mkdir(dir.c_str(), 0755);
    return dir;
}

std::string platform_realpath(const std::string &path)
{
    char *resolved = realpath(path.c_str(), nullptr);
    if (!resolved) return path;
    std::string result(resolved);
    free(resolved);
    return result;
}
