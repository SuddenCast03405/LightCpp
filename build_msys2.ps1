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

$ucrt = "E:\msys64\ucrt64\bin"
if (-not (Test-Path "$ucrt\g++.exe")) {
    Write-Error "MSYS2 UCRT64 toolchain not found at $ucrt"
    exit 1
}

$env:PATH = "$ucrt;" + $env:PATH
$cmake = "$ucrt\cmake.exe"
$ctest = "$ucrt\ctest.exe"
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
# pipe this environment cannot create, so a full build always exits non-zero
# even when every object file and every test binary was produced. Treating that
# as fatal is how `-Test` used to end before ctest ever ran; the exit code is
# still reported, and a real compile error shows up as a missing test binary
# (or as failures in the run itself).
if ($buildExit -ne 0) {
    Write-Warning "cmake --build exited $buildExit (windeployqt always fails here; see the log above for real errors)"
}
if (-not $Test) { exit $buildExit }

$ctestArgs = @('--test-dir', "$root\build\msys2-ucrt64", '--output-on-failure', '--parallel', $Jobs)
if ($Tests) {
    $ctestArgs += @('-R', $Tests)
}
& $ctest @ctestArgs
$testExit = $LASTEXITCODE

Write-Host "build exit = $buildExit, test exit = $testExit"
if ($testExit -ne 0) { exit $testExit }
if ($buildExit -ne 0 -and -not (Test-Path "$root\build\msys2-ucrt64\tests")) { exit $buildExit }
exit 0
