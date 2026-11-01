# LightCpp review progress

- Date: 2026-11-01
- Target: `master` only
- Repository: `SuddenCast03405/LightCpp`
- Review scope: logic correctness first; Windows/Linux/macOS portability; absolute-path and machine-specific assumptions; build/test configuration.
- Baseline commit: `2a77ac7f064e4d98ef85ee2942e6c2a472abf860`
- Default branch is `main`; the requested `master` branch exists and contains the full project. Do not merge or copy from `main`.

## Initial inventory

- `master` has 440 tracked blobs in the recursive tree, including application, core, project, settings, plugin, Lua, and test modules; 245 blobs are under vendored `third_party/`.
- Existing review history is substantial: `bugs/审查结论-2026-10-11.md` and `bugs/审查结论-2026-10-12.md` record prior fixes and remaining architecture/test gaps. Treat them as historical evidence, then verify against current HEAD before acting.
- Recent commits (2026-10-31) added cross-platform path normalization and host-aware project parsing/build behavior. This area needs regression-focused review rather than assuming portability is complete.
- No `.chatgpt/` directory existed at the baseline.

## Review plan

1. Verify the current build matrix and recent portability changes against the actual source and tests.
2. Review path handling (drive-letter, UNC, POSIX, relative paths), process launching, debugger, plugin loading, toolchain discovery, and project parsers.
3. Scan repository-owned source/build/configuration files for machine-specific absolute paths; distinguish harmless examples/comments from runtime dependencies.
4. Inspect high-risk lifecycle/state logic and compare against the historical audit's still-open items.
5. Make focused fixes with regression tests where evidence supports them; avoid broad speculative rewrites.
6. Record each finding, change, and verification result here. Do not claim tests ran unless their result is available.

## Current findings

- Confirmed: the `master` branch is the requested target; `main` is a stub and is not the target.
- Preliminary source scan of the root CMake files, toolchain/path modules, selected project parsers, debugger, and process-tree backend found no obvious machine-specific absolute path literals in runtime code. A few drive paths occur only in comments/examples. This is not yet a complete repository-wide scan.
- Verification status: source inspection in progress; no fresh build or test run has been performed in this environment.

## Work log

- 2026-11-01: Located repository and branch; inventoried tracked files; reviewed README, root CMake/presets, CI, historical audit, and selected path/toolchain/project/debugger sources.

## Findings and changes (2026-11-01)

- CI baseline `2a77ac7`: Linux/macOS could not compile `PluginManagerTests.cpp` because `TrackedPlugin` was under `Q_OS_WIN` but used by cross-platform tests; Windows CTest failed because the process-tree fixture did not model the persistent `cmd /k` terminal shell.
- Fixed test portability: moved `TrackedPlugin` outside the Windows-only guard, kept only the actual Dev-C++ fixture helpers under `Q_OS_WIN`; changed the Windows process-tree fixture from `cmd /c` to `cmd /k`. On CI commit `f6b8cbf`, Windows build and CTest completed successfully; Linux exposed the additional unused-fixture warning, fixed in `bb432b6`.
- GitClient logic fix: each queued job captures its working directory; stale status results and stale status timeout/failure reports are discarded after a project switch. A failed `git status` launch now emits a dedicated `statusFailed` signal (not `commandFinished`, preventing the MainWindow's ordinary-command refresh callback from retrying the failed status forever). `FailedToStart` advances the queue on the next event-loop turn because Windows may emit it synchronously inside `QProcess::start()`.
- Added GitClient tests for one-shot status launch failure and queue recovery after `FailedToStart`.
- Absolute-path scan covered 36 selected first-party source/build files (including root CMake, presets, CI, scripts, project parsers, settings/toolchain, debugger, Git and process-tree modules). No machine-specific absolute path literals were found in runtime/build logic; matches were explanatory comments/examples only. This remains a selected-file scan, not yet a complete audit of all tracked first-party files.
- Latest validation state: GitHub Actions run for `bb432b6` is queued/in progress; no final Linux/macOS result is available yet. No local build was run.

## Remaining review targets

- Complete the absolute-path scan across remaining first-party source, tests, and configuration files.
- Review Windows/UNC/POSIX path semantics and add direct regression tests for the shared project-path helpers.
- Revisit remaining historical architecture risks against current HEAD: debugger MI command/response state, LSP compilation-database discovery, synchronous file opening, and the Git CLI integration boundary. Verify each before changing it.
- Confirm all three CI platforms pass after the latest fixes, then record final counts and any platform-specific skips.
## Additional portability and safety findings (2026-11-01)

- Completed the first-party source/build-path scan across the 130 tracked `src/` files and selected root/plugin/build files. No machine-specific absolute path literals were found in runtime or build logic. Remaining path-looking strings in tests are synthetic fixtures for foreign Windows/POSIX paths; `/proc` is an OS interface, not a developer-machine path.
- macOS CI build succeeded but CTest exposed two GB18030 failures: `EncodingRoundTrip.Gb18030RoundTripsBmpText` (the platform iconv rejects at least one conversion as non-reversible) and `EncodingRoundTrip.Gb18030PreservesBytesItCannotRender` (the byte sequence `F0 A0 AE B7` re-encodes as `F0 A0 B7 AE`). This is a genuine converter/version interoperability issue; do not claim macOS test parity until resolved. Linux and Windows CTest passed on the same review commit.
- Found and fixed a file-input boundary issue: searchable-file reads previously called `readAll()` before enforcing `maxBytes`; text-file readers only preflighted `QFileInfo::size()` and did not reject non-regular devices. The fix bounds the actual read to `maxBytes + 1` and requires `QFileInfo::isFile()` before opening.
- Added explicit UNC path normalization/resolution/relative-path handling and regression tests. This closes a gap not covered by the existing drive-letter-only path tests.
- README corrected: removed the stale reference to deleted `要求.md`, removed stale spec numbering, and accurately documents that MainWindow open/reload currently performs synchronous reads.

## Current validation

- Review commit `80e1b2a`: Linux build + CTest passed; Windows MSYS2 build + CTest passed; macOS build passed but 2 GB18030 CTest cases failed as detailed above.
- The bounded-read, non-regular-file and UNC changes in this commit are not yet validated by CI.
## GB18030 portability correction

- macOS exposed a platform iconv mapping ambiguity: a byte sequence could decode to Unicode text and then re-encode to different bytes. POSIX legacy codec conversions now verify the exact reverse conversion: decode rejects non-canonical byte spellings; encode rejects output that does not decode back to the exact input text. This prevents silent byte/text loss while allowing platform converters to report a non-reversible count when the actual round trip is exact.
- Updated the GB18030 regression to accept explicit refusal of a byte sequence that the platform cannot round-trip exactly, rather than requiring the platform codec to invent a reversible mapping. Added UTF-8 input bytes to encoding failure diagnostics.
- CI for the current branch is running after these changes; macOS GB18030 parity is not yet confirmed.
- Follow-up correctness check: the forced-encoding reader had a second `isDir()` guard that was missed by the first replacement; it is now changed to `!isFile()` as well, and the `/dev/zero` regression covers all three read entry points.
- Updated `iconvConvert` documentation to match the new reverse-conversion validation and added UTF-8/encoded-byte diagnostics to GB18030 round-trip failures.
## Debugger state correction

- Confirmed GDB `cont`/step operations optimistically set `paused_ = false` and emit `resumed()`, then the GDB/MI `*running` record emits `resumed()` again. If GDB rejects the command, the optimistic state also leaves the UI believing execution resumed while GDB remains paused.
- GDB now changes to running only on the actual `*running` record; duplicate running records emit one paused-to-running transition. LLDB/cdb retain their current optimistic transition because their output parsers do not yet expose an equivalent asynchronous running event.
- Added a POSIX fake-GDB regression that emits two `*running` records and verifies one `resumed()` notification. Remaining debugger work: correlate MI command tokens/results, and replace the synchronous 2s graceful + 2s forced stop waits with an asynchronous UI shutdown path.
- Added a GDB execution-command pending flag: a second continue/step click is ignored until `*running`, `*stopped` or `^error` resolves the first command. This preserves the old duplicate-click guard without optimistic state changes. The fake-GDB test now sends two continue clicks before the running event and verifies only one command reaches the debugger.

## MSVC support review (2026-11-01)

- Added a dedicated GitHub Actions job using the native Visual Studio 2022 generator, MSVC x64, and the matching Qt 6.8.3 `win64_msvc2022_64` kit. It installs LLVM development headers, vendors Lua/LuaBridge3, builds every configured target, and runs CTest. This is intentionally separate from the existing MSYS2 UCRT64/GCC job; a Windows GCC pass is not evidence of MSVC compatibility.
- The project already selects MSVC warning flags (`/W4 /WX /permissive- /utf-8`) and uses C++20, but before this addition there was no native MSVC compile/test job. The new job is the first end-to-end validation of those settings and the Windows-specific source paths.
- Rechecked CI run `41274503312` for commit `a551b91c98`: MSYS2 UCRT64 build + all 608 CTest cases passed; Linux build passed but 1/608 tests failed; macOS build passed but 2/608 tests failed. The Linux/macOS shared failure is the fake-GDB regression's command counter: the fixture expected a `continue-command` marker but never emitted it. Updated the fake debugger to emit the marker when it receives `-exec-continue`.
- macOS additionally fails `EncodingRoundTrip.Gb18030RoundTripsBmpText`: Apple's iconv rejects the BMP sample's exact reverse conversion (`e282ac20e6aca7e58583e7aca6e58fb7`, beginning with EURO SIGN), so the strict no-silent-corruption guard rejects the save. This remains an explicit codec portability issue to investigate; do not weaken the exact round-trip safety check without an equally safe mapping.
- Verification pending: the new native MSVC job and the fake-GDB regression fix have not yet run in CI. No local MSVC toolchain is available in this session.


## Native MSVC compile findings (2026-11-01)

- The new Visual Studio 2022/MSVC job configured successfully with MSVC 19.44.35229, Qt 6.8.3 `win64_msvc2022_64`, Windows SDK 10.0.26100 and LLVM headers. This proves the native generator, compiler discovery, Qt kit and Clang header discovery work together.
- The first MSVC compile stopped in `src/LightCppCore/Source/Utf8File.cpp`: `_wfopen` raises MSVC C4996, promoted to an error by the intentional `/W4 /WX` policy. Fixed the MSVC branch to call `_wfopen_s`; MinGW retains `_wfopen`. Added a Unicode filename read regression so the wide-path behavior remains covered.
- Found and corrected a stale contradictory comment in `BuildConfiguration.hpp`: the implementation and tests intentionally retain MSVC `/Zi` debug information for cdb; the header previously claimed that Debug emits no debug database.
- Added a reusable `msvc2022` configure/build/test preset and documented the Visual Studio 2022, matching Qt MSVC kit, LLVM headers and command sequence in README. Updated CI to exercise those presets rather than duplicating generator commands.
- Linux passed all 608/608 CTest cases on commit `059dbfd6`, including the fake-GDB command-count regression fix. MSVC still requires a new build after the `_wfopen_s` correction; macOS GB18030 and the current run's MinGW/macOS results remain pending.


## MSVC macro compatibility pass (2026-11-01)

- CI run `41279103484` confirmed the prior `_wfopen_s` change compiles past `LightCppCore`. The next MSVC failure is `Converter.cpp` at lines 72 and 440: Windows SDK's default `max` macro expands inside `std::numeric_limits<T>::max()` (and also threatens Qt `qMax`), causing C4003/C2589 and then C2220 under /WX.
- Fixed this at the shared first-party CMake warning target: all Windows first-party targets now receive `NOMINMAX`, independent of MSVC vs MinGW. This is preferable to local #undef workarounds because the Windows SDK macro pollution is a project-wide compile-environment issue.
- Existing `ProcessTreeWindows.cpp` already guarded its own `windows.h` include with `NOMINMAX`; the compile definition makes this consistent for every other Windows SDK include as well.
- Linux and MSYS2 jobs passed on `ec65d0a8`; macOS CTest still failed (known GB18030 portability issue). The new common Windows definition is pending native MSVC/MinGW CI verification.


## MSVC 19.44 C1001 compiler crash (2026-11-01)

- Reproduced the reported failure in the native MSVC 19.44.35207 CI job: compilation reaches `DevCppProject.cpp` and the compiler raises C1001 at the closing namespace line (1156), confirming this is not a source diagnostic at that line. The local report uses the same MSVC toolset patch version.
- Reduced the compiler stress in `parseDevProject`: extracted numbered-unit collection into small named helper functions, removed the nested `consider`/warning lambdas and the local aggregate-heavy map logic from the already large parser function. The helper keeps the existing INI unit ordering, duplicate detection, unit settings, and warning behavior.
- Pending native MSVC 19.44 CI verification. Linux/MSYS2/macOS regression suites will also run on the same commit; no claim of resolution until MSVC completes the build.


## MSVC test and macOS codec follow-up (2026-11-01)

- CI on commit `49411a27` confirmed `DevCppProject.cpp` now compiles under MSVC 19.44; the reported C1001 no longer reproduces. The remaining MSVC build errors are separate test-source C7744 diagnostics: `\\x9879\\x76EE` was incorrectly used for Unicode code points, so the literals now use the correct universal-character escapes `\\u9879\\u76EE`.
- MSVC also reported C4702 in the vendored LuaBridge3 header under the project's `/W4 /WX` policy. Marked the third-party LuaBridge include directory as SYSTEM so upstream-header warnings do not fail project builds; project-owned warnings remain errors.
- macOS native compilation succeeds and 608/609 tests pass. Apple's iconv GB18030 euro-sign mapping cannot round-trip exactly, and the converter correctly rejects it to prevent silent data loss. The euro sample is now conditionally checked when the platform can encode and decode it exactly; the portable Chinese sample remains mandatory.


## Visual Studio 2026 CI lane (2026-11-01)

- Added a separate `windows-2025-vs2026` GitHub-hosted runner job using the Visual Studio 18 2026 CMake generator and a new `msvc2026` configure/build/test preset. The existing Visual Studio 2022 job remains as a compatibility baseline.
- The runner uses the available Qt 6.8.3 MSVC x64 package (`win64_msvc2022_64`); the newer MSVC toolset is used for the project build. This exercises the modern compiler while retaining a published Qt kit with the MSVC 14.x ABI-compatible runtime.
- The current VS 2026 lane is pending. Compiler identity and version will be visible in the MSBuild output.


## LuaBridge warning isolation (2026-11-01)

- The latest MSVC 19.44 build exposed C4702 (unreachable code) in the vendored LuaBridge3 header. The prior CMake interface target still declared that path as a normal include directory, so MSVC applied the project's /W4 /WX policy to dependency internals.
- Changed `LightCppLuaBridge` to publish LuaBridge3 as a SYSTEM include directory. This isolates upstream header diagnostics without weakening warnings for LightCpp-owned code.
- A new CI run is required to verify the fix; the active run on commit `3bb7a6e7` was started before this adjustment.


## GDB terminal-state parsing correction (2026-11-01)

- Logic review found that GDB/MI only treated `exited` and `exited-normally` as terminal. A common `exited-signalled` record (for example SIGTERM) fell through to the generic stop path and was reported as a source pause with no valid frame.
- The terminal branch now accepts the `reason="exited…"` family, clears any pending continue/step command, and logs target exit without emitting `paused`.
- Added a fake-GDB regression that produces `*stopped,reason="exited-signalled"` after a step and verifies it is not surfaced as a source pause.
- Validation pending CI on the resulting commit.


## Debugger session isolation (2026-11-01)

- Review found that changing the configured debugger path during a live session changed `backend_` immediately. The active process could then receive commands in the wrong debugger dialect (for example, a GDB process receiving cdb `p`/`q`).
- Added a separate `sessionBackend_` snapshot captured at start; all process arguments, commands, output decoding and parsers now use the immutable session backend, while `backend_` remains the selection for the next launch.
- Session start/finish/abort/stop now clear partial output, pending command and cdb breakpoint-frame state so an incomplete line or stack-frame request cannot leak into a later session.
- Extended the fake-GDB signal-exit regression to change the selected debugger to cdb while GDB is paused, then verify the live GDB session still handles the step and exit correctly.
- CI validation pending.
