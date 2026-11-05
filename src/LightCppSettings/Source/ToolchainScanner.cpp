#include "LightCpp/Settings/ToolchainScanner.hpp"

#include <QDir>
#include <QFileInfo>

namespace LightCpp::Settings {

namespace {

// Candidate file names per toolchain role. The first existing candidate
// per role wins. Compiler/debugger roles fill the `compilers` map keyed
// by program name; the remaining roles fill their dedicated fields.
struct ScanSpec {
    const char* key;
    std::initializer_list<const char*> fileNames;
};

constexpr ScanSpec kScanSpecs[] = {
    {"gcc", {"gcc", "gcc.exe"}},
    {"g++", {"g++", "g++.exe"}},
    {"clang", {"clang", "clang.exe"}},
    {"clang++", {"clang++", "clang++.exe"}},
    {"cl", {"cl.exe"}},
    {"gdb", {"gdb", "gdb.exe"}},
    {"lldb", {"lldb", "lldb.exe"}},
    {"clangd", {"clangd", "clangd.exe"}},
    {"clangFormat", {"clang-format", "clang-format.exe"}},
    {"cmake", {"cmake", "cmake.exe"}},
    {"ninja", {"ninja", "ninja.exe"}},
    {"gnuMake", {"mingw32-make.exe", "gmake", "make.exe", "make"}},
    {"msbuild", {"MSBuild.exe"}},
};

constexpr const char* kCompilerRoleKeys[] = {
    "gcc", "g++", "clang", "clang++", "cl", "gdb", "lldb"};

[[nodiscard]] bool isCompilerRoleKey(const QString& key)
{
    for (const char* role : kCompilerRoleKeys) {
        if (key == QLatin1String(role)) {
            return true;
        }
    }
    return false;
}

QString toolPath(const Toolchain& toolchain, const QString& key)
{
    if (isCompilerRoleKey(key)) {
        return toolchain.compilers.value(key);
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
    if (isCompilerRoleKey(key)) {
        if (path.isEmpty()) {
            toolchain->compilers.remove(key);
        } else {
            toolchain->compilers.insert(key, path);
        }
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
            const QFileInfo info(path);
            if (!info.isFile() || !info.isExecutable()) {
                continue;
            }
            if (toolPath(*out, QLatin1String(spec.key)).isEmpty()) {
                setToolPath(out, QLatin1String(spec.key), path);
                filled.append(QLatin1String(spec.key));
            }
            break;  // one hit per role is enough
        }
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
    if (scanAborted(options)) {
        return;  // the caller is tearing down; stop between directories
    }
    Toolchain toolchain;
    const QStringList filled = fillFromDirectory(dir, &toolchain);
    const bool hostsCompilers =
        !toolchain.compilers.value(QStringLiteral("gcc")).isEmpty()
        || !toolchain.compilers.value(QStringLiteral("g++")).isEmpty()
        || !toolchain.compilers.value(QStringLiteral("clang")).isEmpty()
        || !toolchain.compilers.value(QStringLiteral("clang++")).isEmpty()
        || !toolchain.compilers.value(QStringLiteral("cl")).isEmpty();
    if (hostsCompilers) {
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

/// The directory a toolchain came from: Toolchain itself carries no path
/// field, but every tool it holds is a file inside that directory.
QString toolDirectory(const Toolchain& toolchain)
{
    const auto firstValue = [](const QHash<QString, QString>& map) {
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            if (!it.value().isEmpty()) {
                return it.value();
            }
        }
        return QString();
    };
    for (const QString& path : {firstValue(toolchain.compilers), toolchain.clangd,
                                toolchain.clangFormat,
                                firstValue(toolchain.buildTools)}) {
        if (!path.isEmpty()) {
            return QFileInfo(path).absolutePath();
        }
    }
    return QString();
}

QStringList pathSegmentsOf(const QString& path)
{
    return QDir::fromNativeSeparators(path).split(QLatin1Char('/'),
                                                  Qt::SkipEmptyParts);
}

/// The last `count` segments, or the whole path when it has fewer.
QString nameSuffix(const QStringList& segments, int count)
{
    const int take = qMin(count, static_cast<int>(segments.size()));
    return QStringList(segments.mid(segments.size() - take))
        .join(QLatin1Char('/'));
}

/// Make names unique. A directory holding a compiler is very often called
/// `bin`, so scanning an msys64-style tree produced a picker full of entries
/// all called "bin" (R2-35). Only colliding names are touched, and they are
/// qualified one parent segment at a time until they differ — never renamed
/// past the root of their own path.
void disambiguateNames(QList<Toolchain>* toolchains)
{
    const int count = toolchains->size();
    QList<QStringList> segments;
    segments.reserve(count);
    for (const Toolchain& toolchain : std::as_const(*toolchains)) {
        segments.append(pathSegmentsOf(toolDirectory(toolchain)));
    }

    for (int i = 0; i < count; ++i) {
        if (segments.at(i).isEmpty()) {
            continue;  // no tool path to derive a name from
        }
        const int available = segments.at(i).size();
        for (int parts = 1; parts <= available; ++parts) {
            const QString candidate = nameSuffix(segments.at(i), parts);
            bool collides = false;
            for (int j = 0; j < count && !collides; ++j) {
                if (j != i) {
                    collides = nameSuffix(segments.at(j), parts) == candidate;
                }
            }
            if (!collides) {
                (*toolchains)[i].name = candidate;
                break;
            }
            if (parts == available) {
                // Two different directories with identical trailing paths
                // cannot happen, but if it ever did, the full path is the
                // best name available and it is at least deterministic.
                (*toolchains)[i].name = candidate;
            }
        }
    }
}

QStringList nonEmptyEntries(const QHash<QString, QString>& map)
{
    QStringList entries;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (!it.value().isEmpty()) {
            entries.append(it.key() + QLatin1Char('=') + it.value());
        }
    }
    entries.sort();  // QHash iteration order is unspecified
    return entries;
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
    // Copy rather than aggregate-initialise: a braced init here would drop
    // any option added to the struct later (the abort flag was exactly that).
    ToolchainScanOptions effective = options;
    effective.maxDepth = maxDepth;
    walk(QDir(root).absolutePath(), 0, effective, &result);
    disambiguateNames(&result);
    return result;
}

bool sameTools(const Toolchain& a, const Toolchain& b)
{
    return nonEmptyEntries(a.compilers) == nonEmptyEntries(b.compilers)
           && nonEmptyEntries(a.buildTools) == nonEmptyEntries(b.buildTools)
           && a.clangd == b.clangd && a.clangFormat == b.clangFormat;
}

}  // namespace LightCpp::Settings
