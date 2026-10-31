# Build helper: runs CMake presets with the MSYS2 UCRT64 environment on PATH.
#
# Usage:
#   powershell -File build_msys2.ps1                       # configure + build
#   powershell -File build_msys2.ps1 -Test                 # ... then the whole ctest
#   powershell -File build_msys2.ps1 -Target LightCppTextTests -Tests 'DetectLineEndings.*'
#   powershell -File build_msys2.ps1 -FullSpeed            # do not lower priority
#
# Why the priority switch is the default: a full build plus ctest is 576 test
# processes on this machine and saturates every core for minutes, which makes
# the desktop (and the editor being tested) unusable. BelowNormal is inherited
# by every child — ninja, g++, the test exes — so the work still finishes, it
# just yields to whatever the user is doing. `-FullSpeed` restores the old
# behaviour when the machine is otherwise idle.

param(
    [switch]$Test,
    [switch]$Clean,
    [int]$Jobs = 2,
    [string[]]$Target,
    [string]$Tests,
    [switch]$FullSpeed
)

# Find the active UCRT64 installation from PATH or an explicit MSYS2_ROOT.
$compiler = Get-Command g++.exe -ErrorAction SilentlyContinue
if ($compiler) {
    $ucrt = Split-Path $compiler.Source -Parent
} elseif ($env:MSYS2_ROOT) {
    $ucrt = Join-Path $env:MSYS2_ROOT "ucrt64/bin"
} else {
    Write-Error "MSYS2 UCRT64 not found. Add ucrt64/bin to PATH or set MSYS2_ROOT."
    exit 1
}
if (-not (Test-Path (Join-Path $ucrt "g++.exe"))) {
    Write-Error "No g++.exe found in the discovered directory: $ucrt"
    exit 1
}
$prefix = Split-Path $ucrt -Parent
$env:MINGW_PREFIX = $prefix
$env:PATH = "$ucrt;" + $env:PATH
$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
$ctestCommand = Get-Command ctest.exe -ErrorAction SilentlyContinue
if (-not $cmakeCommand -or -not $ctestCommand) {
    Write-Error "cmake.exe and ctest.exe must be available in the active MSYS2 UCRT64 environment."
    exit 1
}
$cmake = $cmakeCommand.Source
$ctest = $ctestCommand.Source
$root = $PSScriptRoot

if (-not $FullSpeed) {
    try {
        (Get-Process -Id $PID).PriorityClass = 'BelowNormal'
        Write-Host "priority: BelowNormal (children inherit; use -FullSpeed to disable)"
    } catch {
        Write-Warning "could not lower the priority: $($_.Exception.Message)"
    }
}

if ($Clean -and (Test-Path "$root\build")) {
    Remove-Item -Recurse -Force "$root\build"
}

& $cmake --preset msys2-ucrt64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$buildArgs = @('--build', '--preset', 'msys2-ucrt64', '--parallel', $Jobs)
if ($Target) {
    $buildArgs += '--target'
    $buildArgs += $Target
}
& $cmake @buildArgs
$buildExit = $LASTEXITCODE

# The `lightcpp` target ends with a POST_BUILD windeployqt step that needs a
# pipe this environment cannot create, so that one build always exits non-zero
# even when every object file was produced. Treating it as fatal is how `-Test`
# used to end before ctest ever ran — but treating *every* failure as benign is
# worse: ctest does not build, so a compile error would leave ctest running the
# previous binary and reporting its results as if they were today's. That is
# not a hypothetical (it made a reverse-verification look red for the wrong
# reason). So the tolerance is limited to the builds that include `lightcpp`,
# which is the only target with the deploy step.
$buildsTheApp = (-not $Target) -or ($Target -contains 'lightcpp')
if ($buildExit -ne 0 -and -not $buildsTheApp) {
    Write-Error "cmake --build exited $buildExit; not running ctest against a stale binary (see the log above)"
    exit $buildExit
}
if ($buildExit -ne 0) {
    Write-Warning "cmake --build exited $buildExit (the windeployqt step always fails here; check the log above for a real compile error)"
}
if (-not $Test) { exit $buildExit }

$ctestArgs = @('--test-dir', "$root\build\msys2-ucrt64", '--output-on-failure', '--parallel', $Jobs,
               # A filter that matches nothing is a typo, not a pass: ctest
               # otherwise exits 0 after printing "No tests were found!!!".
               '--no-tests=error',
               # Bounded, not because tests are slow — every case here finishes
               # in well under 15 s — but because a *stalled* child used to hold
               # the whole run open forever. Measured on this machine: one
               # Utf8FileTest process spun a single core for 12 minutes while
               # ctest sat at 599/600, and the batch's "full run" never ended.
               # With this, such a stall is reported by name ("***Timeout") in
               # three minutes and the rest of the suite still gets a verdict.
               '--timeout', '180')
if ($Tests) {
    # A ctest regex, not gtest's filter: separate alternatives with `|`, not
    # with `:` — `A.*:B.*` is one regex that matches nothing at all.
    $ctestArgs += @('-R', $Tests)
}
& $ctest @ctestArgs
$testExit = $LASTEXITCODE

Write-Host "build exit = $buildExit, test exit = $testExit"
if ($testExit -ne 0) { exit $testExit }
if ($buildExit -ne 0 -and -not (Test-Path "$root\build\msys2-ucrt64\tests")) { exit $buildExit }
exit 0
