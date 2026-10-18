# LightC++ — Lightweight C/C++ IDE

**Code identifier:** `LightCpp` · **Display name:** LightC++ · **Binary:** `lightcpp`

## Current status

A working Qt6 Widgets IDE per the engineering standards in `要求.md`:
editing (syntax highlighting, code folding, bracket matching), project
management (MSVC/Dev-C++/CMake/.dsp/.dsw), building and running, a
pluggable completion/syntax-check backend (clangd / libclang / ccls over
LSP), debugging (GDB-MI / LLDB / cdb), Git status, Lua 5.4 plugins, and a
settings system with per-project overlays.

Six static logic libraries plus the Qt application layer:

| Module | Purpose |
|--------|---------|
| `src/LightCppCore/` | Encoding/UTF-8 utilities, `Result` sum type, Qt-free |
| `src/LightCppText/` | Text document model, encoding detection, line endings, folding |
| `src/LightCppProject/` | Project/build-configuration parsing (`.vcxproj`/`.vcproj`/`.dev`/`.dsp`/`.dsw`/CMake), compiler diagnostics |
| `src/LightCppSettings/` | Preferences/toolchains/kits, JSON config store |
| `src/LightCppPlugin/` | Plugin interface and registry |
| `src/LightCppLua/` | Sandboxed Lua 5.4 plugin host (`ide` API bound with LuaBridge3) |
| `src/LightCppApp/` | Qt Widgets application layer (MainWindow, editors, panels, dialogs) |

Each logic library has a GTest suite under `tests/`, registered with CTest —
`ctest --preset msys2-ucrt64` prints the current count (593 cases on
2026-10-18). The number is deliberately not repeated here as a fact: it changes
with every batch, and a stale count in a README is worse than no count.

## Module: LightCpp::Core::Text (UTF-8)

| File | Purpose |
|------|---------|
| `src/LightCppCore/Include/LightCpp/Core/Utf8.hpp` | `constexpr` UTF-8 decoder & validator |
| `src/LightCppCore/Include/LightCpp/Core/Utf8File.hpp` | Public API: `readUtf8File`, `ReadResult`, `ReadOptions` |
| `src/LightCppCore/Source/Utf8File.cpp` | Implementation (BOM handling, size limits, sanitization) |

Features:
- UTF-8 BOM detection and optional stripping
- Strict UTF-8 validation; invalid sequences replaced with U+FFFD (or rejected in strict mode)
- Configurable size limit (default 32 MiB, 0 = unlimited)
- Fine-grained error classification: `NotFound` / `NotARegularFile` / `TooLarge` / `IoError` / `InvalidEncoding`
- Binary-mode reads (no CRLF translation), chunked reading via `FILE*` RAII
- Wide-character path support on Windows (`_wfopen`), thread-safe, Qt-free

Decoding behaviour (matches the code, not older promises):
- On a broken continuation byte the decoder resynchronises at the first
  invalid byte (one U+FFFD for the valid prefix) — the WHATWG
  "maximal subpart" strategy for that case.
- On scalar-value violations (surrogate range, overlong encodings,
  above U+10FFFF) the whole sequence is consumed as a single U+FFFD,
  which differs from WHATWG's byte-level resynchronisation.

Note: `readUtf8File()` is a standalone utility of this module; the
product's file-loading pipeline lives in `LightCppText` (encoding
detection incl. GBK/Big5) and does not call it.

Design notes (spec compliance):
- `Utf8.hpp` is header-only and `constexpr`, so decoding/validation can run at
  compile time and be unit-tested without touching the filesystem.
- No Qt dependency in the logic layers (spec 4.1/4.4): pure functions of
  their inputs, callable from any thread. The Qt application layer drives
  file I/O on background threads and marshals results back with queued
  signals, never blocking the GUI thread.
- `ReadResult` is a small C++20 sum type; `std::expected` was deliberately
  avoided because it is C++23 and the project baseline is C++20.

## Build (MSYS2 UCRT64)

```bash
E:/msys64/ucrt64/bin/cmake.exe --preset msys2-ucrt64
E:/msys64/ucrt64/bin/cmake.exe --build --preset msys2-ucrt64
E:/msys64/ucrt64/bin/ctest.exe  --preset msys2-ucrt64      # every suite; prints the count
```

Or use the helper script (it puts the toolchain on `PATH` first, and runs at
`BelowNormal` priority so a full build does not take the desktop with it):

```powershell
powershell -File build_msys2.ps1 -Test                              # build + all suites
powershell -File build_msys2.ps1 -Test -Target LightCppTextTests -Tests 'TextFileTest'
powershell -File build_msys2.ps1 -FullSpeed                          # do not lower priority
```

`-Jobs` (default 2) feeds both `cmake --build --parallel` and `ctest --parallel`;
`-Tests` is a **ctest** regex, so alternatives are separated with `|`, not with
gtest's `:` — a filter that matches nothing now fails (`--no-tests=error`)
instead of reporting a green run over zero tests.

Notes:
- GoogleTest 1.14.0 is vendored in `third_party/` and handed to CPM via
  `SOURCE_DIR`, so configure works offline. Override with
  `-DLIGHTCPP_GTEST_SOURCE=<path>`.
- Lua 5.4.7 is vendored in `third_party/lua-5.4.7/src` and built from
  source. The `ide` surface is registered with LuaBridge3 (vendored in
  `third_party/LuaBridge3-3.0-rc12`): one `addFunction` per member on the
  `ide`/`ide.editor`/`ide.ui`/`ide.commands`/`ide.snippets` namespaces, so
  scripts call it with dot syntax.
- On MinGW the test binaries link the GCC runtime statically
  (`-static-libgcc -static-libstdc++`) so `ctest` does not need
  `E:\msys64\ucrt64\bin` on `PATH`.

Coverage build: `cmake --build build/msys2-ucrt64-coverage --target LightCppCoverage`
(requires lcov/genhtml; enable with the `msys2-ucrt64-coverage` preset).

## Layout

```
cmake/CPM.cmake              # CPM.cmake bootstrap (spec 4.2)
src/LightCppCore/            # core logic library (static)
src/LightCppText/            # text model / encoding / folding (static)
src/LightCppProject/         # project & build configs, diagnostics (static)
src/LightCppSettings/        # settings store, toolchains/kits (static)
src/LightCppPlugin/          # plugin interface (static)
src/LightCppLua/             # Lua 5.4 plugin host (static)
src/LightCppApp/             # Qt Widgets application (lightcpp.exe)
tests/LightCpp*Tests/        # GTest suites, run via CTest
CMakePresets.json            # MSYS2 UCRT64 debug & coverage presets
```
