# ggdb: a GDB front-end

**THIS IS AN EARLY WORK IN PROGRESS**. It *basically* works but is not yet
useful for real debugging.

ImGui front end for GDB using the Machine Interface (MI) protocol. C++20,
CMake build, Dear ImGui UI. Allows stepping through and inspecting a CMake
build. Supports at least Windows and Linux.

## Build

On any platform:

    $ cmake -B build
    $ cmake --build build

By default dependencies are downloaded automatically, currently:

* [Dear ImGui][] ([docking branch][])
* [GLFW][] (except on Windows)

Source releases bundle dependencies in `deps/` for offline builds.
Distribution packagers can use system libraries instead:

    $ cmake -B build -DDEPS=LOCAL

The `DEPS` variable controls how nlohmann/json and GLFW are resolved:

| Value | Behavior |
|-------|----------|
| `FETCH` (default) | bundled → downloaded |
| `LOCAL` | system `find_package` only |

Dear ImGui is always bundled or downloaded because no distributions
package the docking branch. To produce a source release tarball with
bundled dependencies:

    $ cmake -P cmake/SourceRelease.cmake

Which runs `git archive`, downloads dependencies into the tree, and
produces a self-contained `dcmake-VERSION.tar.gz`.

## Usage

    $ ggdb [debuggee args..]

## Shortcuts

| Key | Action |
|-----|--------|
| F5 | Start (free-running) / Continue |
| Shift+F5 | Stop |
| Ctrl+Shift+F5 | Restart |
| F10 | Start (break at first line) / Step Over |
| F11 | Start (break at first line) / Step In |
| Shift+F11 | Step Out |
| F9 | Toggle breakpoint on current line |
| Ctrl+F | Find in source |
| F3 / Shift+F3 | Next / previous match |
| Enter / Shift+Enter | Next / previous match (while in find bar) |
| Escape | Close find or go-to-line bar |
| Ctrl+G | Go to line |
| Ctrl+O | Open file |


[Dear ImGui]: https://github.com/ocornut/imgui
[GLFW]: https://www.glfw.org/
[docking branch]: https://github.com/ocornut/imgui/wiki/Docking
