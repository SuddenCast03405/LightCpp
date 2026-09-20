# Build helper: runs CMake presets with the MSYS2 UCRT64 environment on PATH.
# Usage: powershell -File build_msys2.ps1 [-Test] [-Clean]

param(
    [switch]$Test,
    [switch]$Clean
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

if ($Clean -and (Test-Path "$root\build")) {
    Remove-Item -Recurse -Force "$root\build"
}

& $cmake --preset msys2-ucrt64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build --preset msys2-ucrt64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Test) {
    & $ctest --preset msys2-ucrt64
    exit $LASTEXITCODE
}
