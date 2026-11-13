# C++ overlay client

Windows 10 1703+ / Windows 11, native x64. Requires Visual Studio 2022 with
Desktop development with C++, Windows SDK and CMake 3.24+. The driver must
already be loaded and expose `\\.\Dumper`. WDK is not required for this client.

From the repository root:

```bat
cmake -S client -B build/client -G "Visual Studio 17 2022" -A x64
cmake --build build/client --config Release
build\client\Release\rust_overlay.exe
```

Alternatively open `client` as a CMake folder in Visual Studio and select x64.
The first configuration downloads Dear ImGui v1.91.9b and verifies its SHA-256.
For offline builds supply `-DIMGUI_DIR=C:/deps/imgui` with that version extracted
locally (including `backends`). The upstream MIT license is in ImGui's LICENSE.txt.

The console reports connection and process-access errors using Win32 error codes.
The client waits for RustClient.exe, finds its visible top-level window, and
reconnects after process exit. END exits. The overlay follows the client area,
resizes with it, passes mouse clicks through and hides when the game loses focus
or is minimized. Use windowed or borderless mode; exclusive fullscreen is not
supported. Black is the transparency color key, so use nonblack drawing colors.
Color-key transparency has no per-pixel alpha and may leave antialiasing fringes.

`DriverInterface` matches `driver/dumper.h`: requests are 24/132 bytes and the
module-base response is 8 bytes. Reads are synchronous and limited to 1 MiB.
Ignore the raw output buffer on failure; the typed overload commits only a full
successful read. This class is intended for the render thread. OpenProcess and
the driver's own access checks must both succeed; no access bypass is implemented.

`Esp.h` intentionally contains no game offsets or assumed Rust memory layout.
Fill the constexpr offsets and implement `EnumerateEntities` using verified list,
camera and projection layouts. It currently returns an empty collection. Each
rendered frame queries RustClient.exe through GetModuleBase and displays a small
connection status. No entity boxes are drawn until the adapter is implemented.

The existing `main.c` and root `build.cmd` remain the legacy console dumper;
use the CMake commands above to build this separate C++ client.

Windows smoke checks: start before/after the target process; move, resize,
minimize and refocus its window; restart the process; verify END exits and
mouse clicks reach the target. Check missing-driver and denied-process-access
errors. Rendering and driver I/O require validation on a Windows x64 machine.
