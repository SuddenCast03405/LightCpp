#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include "LightCpp/App/ToolchainHeaders.hpp"

// Forward declaration *outside* the namespace on purpose: inside
// LightCpp::App::Msvc it would declare a nested class named QThread, and
// every use of the real Qt class would fail with an incomplete type.
class QThread;

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
    /// What this toolchain targets, as an LLVM triple. Fed to clangd so it
    /// parses the headers below the same way cl.exe does.
    QString targetTriple;
    /// Minor half of the cl.exe version: 30 for "19.30.30715". Zero means the
    /// version could not be determined (the pre-2017 layouts), which only
    /// suppresses the version hint handed to clangd.
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
///
/// Blocking: it shells out to vswhere and waits for it. Call it from a worker
/// thread (ToolchainDetector does) or accept the wait.
[[nodiscard]] Toolchain detect();

/// Runs detect() on its own thread and reports the result through a signal.
///
/// detect() blocks on vswhere — which on a machine without it costs the full
/// start/finish timeouts, tens of seconds — so calling it from the GUI thread
/// froze the window before it was even shown. The result is delivered back on
/// the receiver's thread as a queued signal; a caller that cannot wait (the
/// build path, when a build is the first thing to need the answer) may block
/// on waitForFinished() and then read result().
class ToolchainDetector : public QObject {
    Q_OBJECT
public:
    explicit ToolchainDetector(QObject* parent = nullptr);
    ~ToolchainDetector() override;  // waits for a running detection

    /// Starts the detection. Idempotent: does nothing once started.
    void start();

    [[nodiscard]] bool isRunning() const;

    /// Blocks until the detection has finished. False when it timed out.
    [[nodiscard]] bool waitForFinished(int msecs = 30000);

    /// The finished result. Only meaningful after the detection finished —
    /// waitForFinished() returned true, or detected() was delivered.
    [[nodiscard]] Toolchain result() const { return result_; }

signals:
    /// Emitted once, when the detection finishes.
    void detected(const LightCpp::App::Msvc::Toolchain& toolchain);

private:
    QThread* thread_ = nullptr;
    Toolchain result_;
};

/// The headers clangd must resolve against for this toolchain.
///
/// Without this, clangd answers from its own bundled clang defaults — on this
/// machine MinGW's libstdc++ — so `<iostream>` opens the GCC header and MSVC
/// code gets underlined for things cl.exe is perfectly happy with. The include
/// directories are the same ones cl.exe reads through INCLUDE, so the editor
/// and the compiler agree by construction.
[[nodiscard]] ToolchainHeaders clangdHeaders(const Toolchain& toolchain);

}  // namespace LightCpp::App::Msvc

/// The detector's result travels through a queued signal, which is only
/// possible for types the meta-object system knows.
Q_DECLARE_METATYPE(LightCpp::App::Msvc::Toolchain)
