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