# ggdb

Specialized GUI front end for GDB using the Machine Interface (MI) protocol.
C++20, CMake build, Dear ImGui UI.

## Build

    cmake -B build
    cmake --build build

ccache is used automatically when detected (`-DDCMAKE_CCACHE=OFF` to disable).

Run with a program to debug:

    ./build/ggdb /path/to/program arg1 arg2

The toolbar command field is a raw inferior command line. It is intentionally
blank by default; starting with an empty command is rejected in the UI.

## Architecture

All source files live under `src/`:

- `src/dcmake.hpp` -- Debugger state struct, session enum, lifecycle prototypes
- `src/dcmake.cpp` -- State machine, ImGui UI, toolbar, all panels
- `src/mi.cpp` -- GDB/MI line protocol, command dispatch, source/breakpoint helpers
- `src/mi.hpp` -- MI interface (`mi_command`, run-control helpers, message pump)
- `src/highlight.cpp` -- Legacy tokenizer code, not currently used by the source viewer
- `src/highlight.hpp` -- Token types and tokenizer prototype
- `src/platform_gui_glfw.cpp` -- macOS/Linux GUI: main(), GLFW+OpenGL3 render loop, dialogs
- `src/platform_gui_win32.cpp` -- Windows/Cygwin GUI: WinMain()/main(), Win32+DX11 render loop, dialogs
- `src/platform_os_posix.cpp` -- macOS/Linux/Cygwin OS: fork/exec, stdio pipes, PTY, POSIX file I/O
- `src/platform_os_win32.cpp` -- Windows OS: CreateProcess, stdio pipes, job-object cleanup
- `src/platform_win32_util.hpp` -- UTF-8/wide-string helpers shared by Win32 GUI and OS halves
- `src/filedialog_macos.mm` -- macOS native file dialogs (NSSavePanel/UTType)
- `src/dcmake.rc.in` -- Windows PE version resource template (VERSIONINFO)
- `src/jetbrains_mono_font.hpp` -- Embedded JetBrains Mono (code/mono panels)
- `src/roboto_font.hpp` -- Embedded Roboto (UI font)
- `src/icon_font.hpp` -- Embedded Codicons 0.0.45 (toolbar/gutter icons)
- `CMakeLists.txt` -- FetchContent deps, four-way platform branch (APPLE/WIN32/CYGWIN/Linux)

The platform layer is split into a GUI half (entry point, window, render loop,
file dialogs) and an OS half (GDB launch, protocol I/O, inferior output I/O,
file I/O, config directory). The two halves compose at link time:

| Platform            | GUI half           | OS half            |
|---------------------|--------------------|--------------------|
| macOS               | `platform_gui_glfw`  | `platform_os_posix`  |
| Linux               | `platform_gui_glfw`  | `platform_os_posix`  |
| Windows (native)    | `platform_gui_win32` | `platform_os_win32`  |
| Cygwin / MSYS2 msys | `platform_gui_win32` | `platform_os_posix`  |

macOS also links `filedialog_macos.mm` for Cocoa dialog overrides.

The OS half implements `platform_launch` and `platform_cleanup`, and fills in
function pointers (`pipe_read`, `pipe_write`, `pipe_shutdown`, optional
`stdout_read`) plus a `void *platform` context on the Debugger struct. The
shared code in `src/dcmake.cpp` and `src/mi.cpp` uses these for I/O — no
platform-specific includes in shared code.

## Dependencies

All via FetchContent with SHA256 hashes. Dear ImGui has no CMakeLists.txt so
we build it as a static library ourselves and conditionally add backend
sources (GLFW+OpenGL3 on macOS/Linux, Win32+DX11 on Windows/Cygwin/MSYS2).

## GDB/MI mechanism

`platform_launch` starts GDB from `PATH` as:

    gdb -nx -q --interpreter=mi --args <toolbar command>

GDB's stdin/stdout are connected to ggdb via pipes. The reader thread reads
line-oriented MI records from GDB stdout and pushes complete lines into
`Debugger::inbox`. The main thread drains the queue in `process_messages()`.
Only the main thread writes MI commands.

Outbound MI commands are tokenized with `Debugger::next_seq`, for example
`1-exec-run` and `2-stack-list-frames`. `Debugger::pending_mi_ops` maps tokens
to logical operations so responses can update stack, variables, and
breakpoint IDs.

Important MI record handling:

- `^done`, `^running`, `^error` are command results.
- `*running` and `*stopped` are exec async records.
- `=...` and `+...` async notifications are protocol metadata, not target output.
- `~"..."` and `&"..."` are GDB console/log streams and should stay out of the Output panel.
- `@"..."` is target-stream output, but the preferred output path avoids sharing target output with MI at all.
- Raw non-MI lines are treated as fallback target output only because some GDB/Windows paths can intermix inferior stdout with MI.

## Inferior Output

Avoid mixing inferior stdout/stderr with MI whenever possible. Target output can
begin with MI-looking prefixes such as `~`, `=`, or `(gdb)`, so parsing after
the fact is ambiguous.

**Windows**: ggdb sends `-gdb-set new-console on`. GDB launches the inferior in
its own console, so stdout/stderr are handled by Windows rather than the MI
pipe. The Output panel will generally not show Windows inferior console output
in this mode. This is intentional until/unless a ConPTY bridge is added.

**POSIX**: `platform_os_posix.cpp` creates a PTY with `posix_openpt`,
`grantpt`, and `unlockpt`. ggdb passes the PTY slave path to GDB with
`-inferior-tty-set`, and the output thread reads the PTY master into the Output
panel. This gives the inferior a terminal-like device and avoids MI/output
ambiguity.

## Features

**Toolbar**: Start/Continue/Pause (F5), Step Over (F10), Step Into (F11), Step
Out (Shift+F11), Restart, Stop. F5 toggles between Start, Continue, and Pause
depending on debugger state.

**Source viewer**: Plain text source with line numbers, gutter breakpoint
markers, and current-line indicator. Smooth scrolling (exponential ease-out)
with lazy margins — auto-scroll only triggers near edges, and cancels on manual
scroll.

**Breakpoints**: Click the gutter to toggle. Line breakpoints use
`-break-insert` and `-break-delete`.

**Locals panel**: Variable inspection via `-stack-list-variables --all-values`.
Nested variable expansion is not fully implemented yet.

**Call Stack panel**: Navigate stack frames by clicking entries.

**Output panel**: Inferior output only. On POSIX this is read from the PTY
master. On Windows, inferior output is expected in the external console opened
by GDB's `new-console` setting.

**MI Log**: Raw protocol inspector. Every MI line is logged with ISO 8601
timestamps. Export to NDJSON with `{"timestamp", "source", "message"}` wrapper.

## DPI scaling

Fonts are baked at physical pixel size (`size * dpi_scale`). On Wayland/macOS
the framebuffer is larger than the window, so `FontGlobalScale = 1/fb_scale`
compensates. On X11 the framebuffer equals the window, so the full scale
remains and `ScaleAllSizes` scales padding/spacing. Falls back to `GDK_SCALE`
or `QT_SCALE_FACTOR` environment variables when GLFW's X11 backend reports 1.0.

## GDB/MI gotchas

**Start sequence**: After `platform_launch`, `mi_start_session()` sends
`-gdb-set mi-async on`. Windows also sends `-gdb-set new-console on`. POSIX
sends `-inferior-tty-set <pty-slave>` when a PTY is available. Then saved
breakpoints are installed and execution starts with `-exec-run` or
`-exec-run --start` for step/start-at-entry.

**Stop handling**: On `*stopped`, ggdb requests `-stack-list-frames`. The top
frame updates `current_source`, `current_line`, and source tab focus. Then
`-stack-list-variables --all-values` refreshes the Locals panel.

**Run to line**: Implemented using a temporary breakpoint plus continue. This
depends on the regular breakpoint path.

**Not implemented yet**: Threads UI, registers, disassembly, attach, remote
targets, pretty-printer integration, conditions/watchpoints, full expression
watch UI, and Windows embedded ConPTY output capture.

## Reference material

- GDB manual: Machine Interface (MI), especially output records and async records
- GDB manual: `set new-console` / `show new-console` on Windows
- GDB manual: `-inferior-tty-set` and inferior terminal handling
