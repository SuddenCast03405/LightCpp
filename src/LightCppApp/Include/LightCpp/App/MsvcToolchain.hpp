#pragma once

#include <QString>
#include <QStringList>

namespace LightCpp::App::Msvc {

/// A located Microsoft C/C++ toolchain.
///
/// cl.exe is not usable on its own: it needs INCLUDE/LIB pointing at both the
/// toolset's own headers and the Windows SDK, plus its bin directory on PATH.
/// Normally vcvarsall.bat computes all of that, but it does so by shelling out
/// to reg.exe and cmd.exe. Doing it here instead keeps the IDE free of a
/// command-shell dependency and, more usefully, lets it work from a sandbox
/// that blocks those tools.
///
/// Layouts handled, i.e. everything from Visual C++ 6.0 (1998) to VS 2026:
///
///   VC6        6.0        <VC98>\Bin\cl.exe              <VC98>\Include, <VC98>\Lib
///   VS2002-15  7.0-14.0   <VS>\VC\bin\amd64\cl.exe       <VS>\VC\include, <VS>\VC\lib\amd64
///   VS2017+    15.0-18.x  <VS>\VC\Tools\MSVC\<v>\bin\Hostx64\x64\cl.exe
///                                                       <v>\include, <v>\lib\x64
///
/// Only the VS2017+ layout was exercised on the machine this was written on;
/// the other two follow the documented layouts but had nothing to test against.
struct Toolchain {
    /// Human-readable identification, e.g. "MSVC 19.30 (Visual Studio 2022)".
    QString label;
    /// Absolute path to cl.exe. Empty when no compiler was found.
    QString clPath;
    /// Directory to prepend to PATH so cl.exe finds its own DLLs.
    QString binDir;
    /// Directories for INCLUDE, most specific first.
    QStringList includeDirs;
    /// Directories for LIB, most specific first.
    QStringList libDirs;
    /// Minor half of the cl.exe version: 30 for "19.30.30715". Zero means the
    /// compiler predates the /std: switch entirely (VS 2015 and older).
    int clMinor = 0;
    /// Why this toolchain cannot be used. Empty when it can.
    QString problem;

    [[nodiscard]] bool isValid() const
    {
        return !clPath.isEmpty() && problem.isEmpty();
    }
};

/// Locate the newest usable MSVC on this machine.
///
/// Never returns an empty-handed result: when nothing is found, or what was
/// found is unusable, `problem` says so and the caller can show that instead of
/// a compiler failing with a wall of "cannot open include file".
[[nodiscard]] Toolchain detect();

/// The `/std:` argument matching what this compiler actually understands.
///
/// The project targets C++20, but asking for it from a compiler that predates
/// the switch is worse than asking for nothing, so this degrades: c++20 from
/// cl 19.29, c++17 from 19.14, c++latest from 19.11, and nothing at all before
/// that. Returns an empty string when no switch should be passed.
[[nodiscard]] QString standardArgument(const Toolchain& toolchain);

}  // namespace LightCpp::App::Msvc
