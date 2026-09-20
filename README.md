# LightC++ — Lightweight C/C++ IDE

**Code identifier:** `LightCpp` · **Display name:** LightC++ · **Binary:** `lightcpp`

## Current status

Implements the **UTF-8 text file reading** module of the core library
(`LightCppCore`), per the engineering standards in `要求.md`.

## Module: LightCpp::Core::Text

| File | Purpose |
|------|---------|
| `src/LightCppCore/Include/LightCpp/Core/Utf8.hpp` | `constexpr` UTF-8 decoder & validator (maximal-subpart strategy) |
| `src/LightCppCore/Include/LightCpp/Core/Utf8File.hpp` | Public API: `readUtf8File`, `ReadResult`, `ReadOptions` |
| `src/LightCppCore/Source/Utf8File.cpp` | Implementation (BOM handling, size limits, sanitization) |

Features:
- UTF-8 BOM detection and optional stripping
- Strict UTF-8 validation; invalid sequences replaced with U+FFFD (or rejected in strict mode)
- Configurable size limit (default 32 MiB, 0 = unlimited)
- Fine-grained error classification: `NotFound` / `NotARegularFile` / `TooLarge` / `IoError` / `InvalidEncoding`
- Binary-mode reads (no CRLF translation), chunked reading via `FILE*` RAII
- Wide-character path support on Windows (`_wfopen`), thread-safe, Qt-free

Design notes (spec compliance):
- `Utf8.hpp` is header-only and `constexpr`, so decoding/validation can run at
  compile time and be unit-tested without touching the filesystem.
- No Qt dependency in this layer (spec 4.1/4.4): the reader is a pure function
  of `(path, options)`, callable from any thread. Per spec 4.3 the UI layer
  will drive it through `QtConcurrent`/`QThread` and marshal results back with
  `QMetaObject::invokeMethod`, never blocking the GUI thread.
- `ReadResult` is a small C++20 sum type; `std::expected` was deliberately
  avoided because it is C++23 and the project baseline is C++20.

## Build (MSYS2 UCRT64)

```bash
E:/msys64/ucrt64/bin/cmake.exe --preset msys2-ucrt64
E:/msys64/ucrt64/bin/cmake.exe --build --preset msys2-ucrt64
E:/msys64/ucrt64/bin/ctest.exe  --preset msys2-ucrt64      # 28 tests, all passing
```

Or use the helper script (it puts the toolchain on `PATH` first):

```powershell
powershell -File build_msys2.ps1 -Test
```

Notes:
- GoogleTest 1.14.0 is vendored in `third_party/` and handed to CPM via
  `SOURCE_DIR`, so configure works offline. Override with
  `-DLIGHTCPP_GTEST_SOURCE=<path>`.
- On MinGW the test binary links the GCC runtime statically
  (`-static-libgcc -static-libstdc++`) so `ctest` does not need
  `E:\msys64\ucrt64\bin` on `PATH`.

Coverage build: `cmake --build build/msys2-ucrt64-coverage --target LightCppCoverage`
(requires lcov/genhtml; enable with the `msys2-ucrt64-coverage` preset).

## Layout

```
cmake/CPM.cmake              # CPM.cmake bootstrap (spec 4.2)
src/LightCppCore/            # core logic library (static)
tests/LightCppCoreTests/     # GTest suite, run via CTest
CMakePresets.json            # MSYS2 UCRT64 debug & coverage presets
```
