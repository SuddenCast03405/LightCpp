#include "LightCpp/Settings/ToolchainScanner.hpp"

#include <QDir>
#include <QFileInfo>

namespace LightCpp::Settings {

namespace {

// Candidate file names per toolchain role. The first existing candidate
// per role wins.
struct ScanSpec {
    const char* key;
    std::initializer_list<const char*> fileNames;
};

constexpr ScanSpec kScanSpecs[] = {
    {"cCompiler", {"gcc.exe", "clang.exe", "cl.exe"}},
    {"cppCompiler", {"g++.exe", "clang++.exe", "cl.exe"}},
    {"debugger", {"gdb.exe", "lldb.exe"}},
    {"clangd", {"clangd.exe"}},
    {"clangFormat", {"clang-format.exe"}},
    {"cmake", {"cmake.exe"}},
    {"ninja", {"ninja.exe"}},
    {"gnuMake", {"mingw32-make.exe", "make.exe"}},
    {"msbuild", {"MSBuild.exe"}},
};

QString toolPath(const Toolchain& toolchain, const QString& key)
{
    if (key == QLatin1String("cCompiler")) {
        return toolchain.cCompiler;
    }
    if (key == QLatin1String("cppCompiler")) {
        return toolchain.cppCompiler;
    }
    if (key == QLatin1String("debugger")) {
        return toolchain.debugger;
    }
    if (key == QLatin1String("clangd")) {
        return toolchain.clangd;
    }
    if (key == QLatin1String("clangFormat")) {
        return toolchain.clangFormat;
    }
    return toolchain.buildTools.value(key);
}

void setToolPath(Toolchain* toolchain, const QString& key, const QString& path)
{
    if (key == QLatin1String("cCompiler")) {
        toolchain->cCompiler = path;
        return;
    }
    if (key == QLatin1String("cppCompiler")) {
        toolchain->cppCompiler = path;
        return;
    }
    if (key == QLatin1String("debugger")) {
        toolchain->debugger = path;
        return;
    }
    if (key == QLatin1String("clangd")) {
        toolchain->clangd = path;
        return;
    }
    if (key == QLatin1String("clangFormat")) {
        toolchain->clangFormat = path;
        return;
    }
    if (path.isEmpty()) {
        toolchain->buildTools.remove(key);
    } else {
        toolchain->buildTools.insert(key, path);
    }
}

/// Fill `out` with every known tool executable found directly in `dir`.
/// Returns the filled role keys.
QStringList fillFromDirectory(const QString& dir, Toolchain* out)
{
    QStringList filled;
    for (const ScanSpec& spec : kScanSpecs) {
        for (const char* fileName : spec.fileNames) {
            const QString path = QDir(dir).filePath(QLatin1String(fileName));
            if (!QFileInfo::exists(path)) {
                continue;
            }
            if (toolPath(*out, QLatin1String(spec.key)).isEmpty()) {
                setToolPath(out, QLatin1String(spec.key), path);
                filled.append(QLatin1String(spec.key));
            }
            break;  // one hit per role is enough
        }
    }
    // cl.exe compiles both C and C++.
    if (filled.contains(QLatin1String("cCompiler"))
        && out->cppCompiler.isEmpty()
        && out->cCompiler.endsWith(QLatin1String("cl.exe"),
                                   Qt::CaseInsensitive)) {
        out->cppCompiler = out->cCompiler;
    }
    return filled;
}

bool isExcluded(const QString& name, const QStringList& excluded)
{
    for (const QString& entry : excluded) {
        if (name.compare(entry, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

/// Depth counts levels below the root: the root is walked at depth 0, its
/// children at depth 1, and recursion stops once depth reaches maxDepth.
void walk(const QString& dir, int depth, const ToolchainScanOptions& options,
          QList<Toolchain>* out)
{
    Toolchain toolchain;
    const QStringList filled = fillFromDirectory(dir, &toolchain);
    if (filled.contains(QLatin1String("cCompiler"))
        || filled.contains(QLatin1String("cppCompiler"))) {
        toolchain.name = QFileInfo(dir).fileName();
        if (toolchain.name.isEmpty()) {
            toolchain.name = dir;
        }
        out->append(toolchain);
        return;  // a directory hosting a toolchain is a leaf
    }
    if (depth >= options.maxDepth) {
        return;
    }
    const QFileInfoList children =
        QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& child : children) {
        if (child.isSymLink()) {
            continue;  // never follow links: they can close cycles
        }
        if (isExcluded(child.fileName(), options.excludedDirectories)) {
            continue;
        }
        walk(child.absoluteFilePath(), depth + 1, options, out);
    }
}

}  // namespace

QList<Toolchain> scanForToolchains(const QString& root,
                                   const ToolchainScanOptions& options)
{
    QList<Toolchain> result;
    if (!QFileInfo(root).isDir()) {
        return result;
    }
    const int maxDepth = qMax(0, options.maxDepth);
    walk(QDir(root).absolutePath(), 0,
         ToolchainScanOptions{maxDepth, options.excludedDirectories}, &result);
    return result;
}

}  // namespace LightCpp::Settings
