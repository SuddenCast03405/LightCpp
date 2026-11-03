<<<<<<< HEAD
# LightC++ — Lightweight C/C++ IDE

**Code identifier:** `LightCpp` · **Display name:** LightC++ · **Binary:** `lightcpp`

## Current status

A working Qt6 Widgets IDE:
editing (syntax highlighting, code folding, bracket matching), project
management (native CMake builds; Windows-only MSVC/Dev-C++ build backends;
parsing of .dsp/.dsw projects), building and running, a
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
`ctest --preset msys2-ucrt64` prints the current count. The number is deliberately
not hard-coded here: it changes with every batch, and a stale count in a README
is worse than no count.

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

Legacy code-page conversion is supported on Windows through the native code
page APIs and on Linux/macOS through `iconv`. On a UTF-8 POSIX locale, an
otherwise-invalid byte stream is opened with a lossless Latin-1 guess rather
than being mislabeled UTF-8 and silently rewritten with replacement characters;
the user can select its actual encoding from the editor.

Design notes:
- `Utf8.hpp` is header-only and `constexpr`, so decoding/validation can run at
  compile time and be unit-tested without touching the filesystem.
- The core logic library is Qt-free; the text, project and settings layers
  use Qt Core where file, path and JSON APIs are needed, without depending on
  Qt Widgets. TextFile read APIs use per-call file and decoder objects and
  can run on worker threads. MainWindow open/reload currently performs
  synchronous reads; moving large-file opens off the UI thread remains
  follow-up work.
- `ReadResult` is a small C++20 sum type; `std::expected` was deliberately
  avoided because it is C++23 and the project baseline is C++20.

## Build (MSYS2 UCRT64)

Toolchain packages: `mingw-w64-ucrt-x86_64-gcc`, `-cmake`, `-ninja`,
`-qt6-base`, and `-clang` — the last one only for `clang-c/Index.h`, which
`LibClangCompleter.cpp` includes at build time (the shared library itself is
loaded dynamically with Qt's platform loader at run time). Without it the build stops at
`fatal error: clang-c/Index.h: No such file or directory`.

Open an **MSYS2 UCRT64** shell (or make its `ucrt64/bin` directory
available on `PATH`), then run:

```bash
cmake -P _fetch_deps.cmake
cmake --preset msys2-ucrt64
cmake --build --preset msys2-ucrt64
ctest --preset msys2-ucrt64
```

Or from PowerShell use the helper script. It discovers the active MSYS2
installation from `PATH` or `MSYS2_ROOT`; no drive letter or install
directory is assumed. Qt deployment is disabled by default so a failed
`windeployqt` post-build step cannot be mistaken for a successful test build.
Pass `-DeployQt` when you want to stage Qt DLLs beside the executable:

```powershell
powershell -File build_msys2.ps1 -Test
powershell -File build_msys2.ps1 -Test -Target LightCppTextTests -Tests 'TextFileTest'
powershell -File build_msys2.ps1 -FullSpeed
powershell -File build_msys2.ps1 -DeployQt
```

### Windows native MSVC (Visual Studio 2022)

Install Visual Studio 2022 with the **Desktop development with C++** workload
(MSVC v143 and a Windows SDK), CMake 3.24+, a Qt 6 MSVC 2022 x64 development
kit, and LLVM development headers containing `clang-c/Index.h`. The Qt kit
must match the compiler family: the MinGW Qt package cannot be used with MSVC.

From a Visual Studio Developer PowerShell (or a shell where the VS C++ tools
are available), run:

```powershell
cmake -P _fetch_deps.cmake
cmake --preset msvc2022 -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" -DLLVM_ROOT="C:/Program Files/LLVM"
cmake --build --preset msvc2022 --parallel 2
ctest --preset msvc2022
```

Replace the Qt and LLVM prefixes with their actual installation locations.
The preset selects the Visual Studio 2022 x64 generator and Debug
configuration; it does not assume a drive letter or a fixed Qt/LLVM location.
The project enables `/W4 /WX /permissive- /utf-8` for first-party C++ code, so
MSVC diagnostics are treated as build failures rather than silently ignored.

### Linux and macOS (native preset)

Install CMake 3.24+, Ninja, Qt 6.5 development packages (Core, Widgets and
Xml), a C++20 compiler, and Clang C API headers (`clang-c/Index.h`). Then:

```bash
cmake -P _fetch_deps.cmake
cmake --preset native
cmake --build --preset native
ctest --preset native
```

The native preset uses the host compiler and Qt installation; it does not
select a Windows toolchain or assume a particular package-manager prefix.
Linux/macOS support is still being validated by CI and should be treated as
in progress until those builds and tests pass.

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
  (`-static-libgcc -static-libstdc++`) so test execution does not depend on
  the compiler installation being added to the system-wide PATH.

Coverage build: `cmake --build build/msys2-ucrt64-coverage --target LightCppCoverage`
(requires lcov/genhtml; enable with the `msys2-ucrt64-coverage` preset).

## CI

`.github/workflows/ci.yml` builds and tests **master** on Windows, Linux and
macOS. The Windows job uses MSYS2 UCRT64; the POSIX jobs use the native host
compiler and Qt 6. Each job vendors Lua/LuaBridge3 with `_fetch_deps.cmake`,
then configures, builds and runs the complete CTest preset.

The Windows `ci` preset keeps the MSYS2 install prefix caller-supplied and
sets `LIGHTCPP_DEPLOY_QT=OFF` so CI does not depend on a writable Qt install.
The `native` preset uses the system compiler and Qt installation on Linux and
macOS. Both test presets use `--no-tests=error`, so an empty test selection
cannot report a false green result.

The Dev-C++ makefile backend and MSVC integration remain Windows-only because
their project formats and build tools use Windows command-shell and SDK
conventions. CMake projects, the editor, text conversion, toolchain discovery,
and the process-tree backend are intended to work natively on all three
supported operating systems. Platform support is not considered complete
until the corresponding CI build and tests pass.

## Layout

```
cmake/CPM.cmake              # CPM.cmake bootstrap
src/LightCppCore/            # core logic library (static)
src/LightCppText/            # text model / encoding / folding (static)
src/LightCppProject/         # project & build configs, diagnostics (static)
src/LightCppSettings/        # settings store, toolchains/kits (static)
src/LightCppPlugin/          # plugin interface (static)
src/LightCppLua/             # Lua 5.4 plugin host (static)
src/LightCppApp/             # Qt Widgets application (native host executable)
tests/LightCpp*Tests/        # GTest suites, run via CTest
CMakePresets.json            # MSYS2 and native host build/test presets
```
=======
# LightCpp
>>>>>>> origin/check/feature-audit-2026-11-01
