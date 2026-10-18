#include "LightCpp/App/ToolchainPaths.hpp"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace LightCpp::App::ToolchainPaths {
namespace {

/// MSYS2 layouts this project has been built against. They are **candidates**,
/// not answers: each one is checked for the tool it is supposed to hold, so a
/// machine without MSYS2 gets "not found" rather than a path that is not
/// there.
///
/// `E:` is still listed, and still first, for a measured reason: the machines
/// this IDE is written on keep MSYS2 there, and the IDE started from Explorer
/// inherits no MSYS2 environment at all — neither the machine PATH nor the
/// user PATH on the development machine carries a single `msys`/`ucrt` entry,
/// the toolchain only reaches PATH inside an MSYS2 shell. Dropping the known
/// layout entirely would turn "builds with the default toolchain" into "builds
/// nothing" for exactly the setup this project is developed in. What changed
/// is that it is no longer *asserted*: it is one candidate among several, and
/// it has to exist to be returned.
///
/// `ucrt64` before `mingw64`: the preset this project ships (`msys2-ucrt64`)
/// builds against the UCRT runtime, so those DLLs are the ones a program built
/// with no kit selected links against.
const char* const kKnownBins[] = {
    "E:/msys64/ucrt64/bin",
    "E:/msys64/mingw64/bin",
    "C:/msys64/ucrt64/bin",
    "C:/msys64/mingw64/bin",
    "D:/msys64/ucrt64/bin",
    "D:/msys64/mingw64/bin",
};

bool hasFile(const QString& dir, const QString& name)
{
    return !dir.isEmpty() && QFileInfo::exists(QDir(dir).filePath(name));
}

/// The directory `tool` is on PATH from, or empty.
QString dirOnPath(const QString& tool)
{
    const QString found =
        QStandardPaths::findExecutable(QFileInfo(tool).completeBaseName());
    return found.isEmpty() ? QString() : QFileInfo(found).absolutePath();
}

/// A tool: the copy in the build bin directory first, then PATH.
QString toolPath(const QString& exe)
{
    const QString bin = mingwBin();
    if (hasFile(bin, exe)) {
        return QDir(bin).filePath(exe);
    }
    return QStandardPaths::findExecutable(QFileInfo(exe).completeBaseName());
}

}  // namespace

QString firstWithTool(const QStringList& candidates,
                      const std::function<bool(const QString&)>& hasTool)
{
    if (!hasTool) {
        return {};
    }
    for (const QString& candidate : candidates) {
        if (candidate.isEmpty() || !hasTool(candidate)) {
            continue;
        }
        return candidate;
    }
    return {};
}

QString mingwBin()
{
    static const QString resolved = [] {
        QStringList candidates;
        for (const char* bin : kKnownBins) {
            candidates.append(QString::fromLatin1(bin));
        }
        const QString known = firstWithTool(candidates, [](const QString& dir) {
            return hasFile(dir, QStringLiteral("g++.exe"));
        });
        if (!known.isEmpty()) {
            return known;
        }
        return dirOnPath(QStringLiteral("g++.exe"));
    }();
    return resolved;
}

QString clangdPath()
{
    static const QString resolved = toolPath(QStringLiteral("clangd.exe"));
    return resolved;
}

QString clangFormatPath()
{
    static const QString resolved = toolPath(QStringLiteral("clang-format.exe"));
    return resolved;
}

QString gitPath()
{
    static const QString resolved = toolPath(QStringLiteral("git.exe"));
    return resolved;
}

}  // namespace LightCpp::App::ToolchainPaths
