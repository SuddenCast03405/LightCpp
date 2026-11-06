// BuildOptions — implementation. See the header for the three layers.

#include "LightCpp/Workbench/BuildOptions.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>

namespace LightCpp::Workbench::Build {
namespace {

QString shellQuote(const QString& value)
{
    if (!value.contains(QLatin1Char(' ')) && !value.contains(QLatin1Char('\t'))
        && !value.contains(QLatin1Char('"'))) {
        return value;
    }
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

}  // namespace

// ---------------------------------------------------------------------------
// CompileOptions
// ---------------------------------------------------------------------------

bool CompileOptions::isEmpty() const
{
    return defines.isEmpty() && includePaths.isEmpty() && libraries.isEmpty()
           && libraryPaths.isEmpty() && flags.isEmpty() && cStandard.isEmpty()
           && cppStandard.isEmpty();
}

QStringList CompileOptions::toArguments() const
{
    QStringList arguments;
    if (!cStandard.isEmpty()) {
        arguments << QStringLiteral("-std=%1").arg(cStandard);
    }
    if (!cppStandard.isEmpty()) {
        arguments << QStringLiteral("-std=%1").arg(cppStandard);
    }
    for (const QString& define : defines) {
        arguments << QStringLiteral("-D%1").arg(define);
    }
    for (const QString& path : includePaths) {
        arguments << QStringLiteral("-I%1").arg(path);
    }
    for (const QString& path : libraryPaths) {
        arguments << QStringLiteral("-L%1").arg(path);
    }
    for (const QString& library : libraries) {
        arguments << (library.contains(QLatin1Char('/')) ? library
                                                        : QStringLiteral("-l%1").arg(library));
    }
    arguments += flags;
    return arguments;
}

QStringList CompileOptions::toMsvcArguments() const
{
    QStringList arguments;
    if (!cStandard.isEmpty()) {
        arguments << QStringLiteral("/std:%1").arg(cStandard);
    }
    if (!cppStandard.isEmpty()) {
        arguments << QStringLiteral("/std:%1").arg(cppStandard);
    }
    for (const QString& define : defines) {
        arguments << QStringLiteral("/D%1").arg(define);
    }
    for (const QString& path : includePaths) {
        arguments << QStringLiteral("/I%1").arg(path);
    }
    for (const QString& path : libraryPaths) {
        arguments << QStringLiteral("/LIBPATH:%1").arg(path);
    }
    for (const QString& library : libraries) {
        arguments << (library.contains(QLatin1Char('/')) || library.contains(QLatin1Char('\\'))
                         ? library
                         : library + QStringLiteral(".lib"));
    }
    arguments += flags;
    return arguments;
}

QStringList CompileOptions::toCMakeArguments() const
{
    QStringList arguments;
    if (!cStandard.isEmpty()) {
        arguments << QStringLiteral("-DCMAKE_C_STANDARD=")
                         + QString(cStandard).remove(QStringLiteral("c"));
    }
    if (!cppStandard.isEmpty()) {
        QString value = cppStandard;
        value.remove(QStringLiteral("c++"));
        if (!value.isEmpty()) {
            arguments << QStringLiteral("-DCMAKE_CXX_STANDARD=") + value;
        }
    }
    if (!defines.isEmpty()) {
        // A CMake list is semicolon-separated and its values may contain
        // spaces, so each define is quoted for the cache.
        QStringList values;
        for (const QString& define : defines) {
            values << shellQuote(define);
        }
        arguments << QStringLiteral("-DCMAKE_C_FLAGS=") + values.join(QLatin1Char(' '));
    }
    if (!includePaths.isEmpty()) {
        arguments << QStringLiteral("-DCMAKE_INCLUDE_PATH=")
                         + includePaths.join(QLatin1Char(';'));
    }
    if (!libraryPaths.isEmpty()) {
        arguments << QStringLiteral("-DCMAKE_LIBRARY_PATH=")
                         + libraryPaths.join(QLatin1Char(';'));
    }
    if (!flags.isEmpty()) {
        arguments << QStringLiteral("-DCMAKE_CXX_FLAGS=") + flags.join(QLatin1Char(' '));
    }
    return arguments;
}

QStringList splitCommandLine(const QString& command)
{
    QStringList arguments;
    QString current;
    bool inSingle = false;
    bool inDouble = false;
    bool escaped = false;
    bool started = false;
    for (const QChar ch : command) {
        if (escaped) {
            current.append(ch);
            escaped = false;
            started = true;
            continue;
        }
        if (ch == QLatin1Char('\\') && !inSingle) {
            escaped = true;
            started = true;
            continue;
        }
        if (ch == QLatin1Char('\'') && !inDouble) {
            inSingle = !inSingle;
            started = true;
            continue;
        }
        if (ch == QLatin1Char('"') && !inSingle) {
            inDouble = !inDouble;
            started = true;
            continue;
        }
        if ((ch == QLatin1Char(' ') || ch == QLatin1Char('\t')) && !inSingle && !inDouble) {
            if (started) {
                arguments.append(current);
                current.clear();
                started = false;
            }
            continue;
        }
        current.append(ch);
        started = true;
    }
    if (started) {
        arguments.append(current);
    }
    return arguments;
}

CompileOptions parseCompileCommand(const QStringList& arguments)
{
    CompileOptions options;
    for (qsizetype i = 0; i < arguments.size(); ++i) {
        const QString& arg = arguments.at(i);
        if (arg == QLatin1String("-D") || arg.startsWith(QLatin1String("-D"))
            || arg.startsWith(QLatin1String("/D"))) {
            if (arg == QLatin1String("-D") || arg == QLatin1String("/D")) {
                if (i + 1 < arguments.size()) {
                    options.defines.append(arguments.at(++i));
                }
            } else {
                options.defines.append(arg.mid(2));
            }
            continue;
        }
        if (arg.startsWith(QLatin1String("-I"))) {
            if (arg == QLatin1String("-I") && i + 1 < arguments.size()) {
                options.includePaths.append(arguments.at(++i));
            } else if (arg.size() > 2) {
                options.includePaths.append(arg.mid(2));
            }
            continue;
        }
        if (arg.startsWith(QLatin1String("/I")) && arg.size() > 2
            && !arg.startsWith(QLatin1String("/INCREMENTAL"))) {
            options.includePaths.append(arg.mid(2));
            continue;
        }
        if (arg.startsWith(QLatin1String("-isystem"))) {
            if (arg == QLatin1String("-isystem") && i + 1 < arguments.size()) {
                options.includePaths.append(arguments.at(++i));
            } else if (arg.size() > 8) {
                options.includePaths.append(arg.mid(8));
            }
            continue;
        }
        if (arg.startsWith(QLatin1String("-L"))) {
            if (arg == QLatin1String("-L") && i + 1 < arguments.size()) {
                options.libraryPaths.append(arguments.at(++i));
            } else if (arg.size() > 2) {
                options.libraryPaths.append(arg.mid(2));
            }
            continue;
        }
        if (arg.startsWith(QLatin1String("-l"))) {
            if (arg.size() > 2) {
                options.libraries.append(arg.mid(2));
            }
            continue;
        }
        if (arg.startsWith(QLatin1String("-std=")) || arg.startsWith(QLatin1String("/std:"))) {
            const QString standard = arg.mid(arg.indexOf(QLatin1Char('=')) >= 0
                                                ? arg.indexOf(QLatin1Char('=')) + 1
                                                : arg.indexOf(QLatin1Char(':')) + 1);
            if (standard.startsWith(QLatin1String("c++"))
                || standard.startsWith(QLatin1String("gnu++"))) {
                options.cppStandard = standard;
            } else {
                options.cStandard = standard;
            }
            continue;
        }
        // The source file itself is not an option.
        if (arg.endsWith(QLatin1String(".cpp")) || arg.endsWith(QLatin1String(".cc"))
            || arg.endsWith(QLatin1String(".cxx")) || arg.endsWith(QLatin1String(".c"))
            || arg.endsWith(QLatin1String(".m")) || arg.endsWith(QLatin1String(".mm"))) {
            continue;
        }
        options.flags.append(arg);
    }
    return options;
}

// ---------------------------------------------------------------------------
// Cross compilation
// ---------------------------------------------------------------------------

bool CrossToolchain::isValid() const
{
    return !systemName.trimmed().isEmpty() && !cCompiler.trimmed().isEmpty()
           && !cxxCompiler.trimmed().isEmpty();
}

QStringList CrossToolchain::problems() const
{
    QStringList list;
    if (systemName.trimmed().isEmpty()) {
        list.append(QStringLiteral("CMAKE_SYSTEM_NAME is empty — CMake would build for the host"));
    }
    if (cCompiler.trimmed().isEmpty()) {
        list.append(QStringLiteral("the C compiler is not set"));
    }
    if (cxxCompiler.trimmed().isEmpty()) {
        list.append(QStringLiteral("the C++ compiler is not set"));
    }
    for (const QString& compiler : {cCompiler, cxxCompiler}) {
        if (compiler.trimmed().isEmpty()) {
            continue;
        }
        const QFileInfo info(compiler);
        // A bare name is looked up on PATH by CMake; only an absolute path can
        // be checked here.
        if (info.isAbsolute() && !info.exists()) {
            list.append(QStringLiteral("%1 does not exist").arg(compiler));
        }
    }
    return list;
}

namespace {

void appendCacheEntry(QStringList* lines, const QString& key, const QString& value,
                      const QString& comment = QString())
{
    if (!comment.isEmpty()) {
        lines->append(QStringLiteral("# %1").arg(comment));
    }
    QString escaped = value;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("/"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    lines->append(QStringLiteral("set(%1 \"%2\" CACHE STRING \"\")").arg(key, escaped));
}

}  // namespace

QString CrossToolchain::toCMakeToolchainFile() const
{
    QStringList lines;
    lines.append(QStringLiteral("# Generated by LightC++ — cross-compilation toolchain"));
    if (!name.trimmed().isEmpty()) {
        lines.append(QStringLiteral("# %1").arg(name.trimmed()));
    }
    lines.append(QString());
    appendCacheEntry(&lines, QStringLiteral("CMAKE_SYSTEM_NAME"), systemName,
                     QStringLiteral("the target system, not the host"));
    if (!systemVersion.trimmed().isEmpty()) {
        appendCacheEntry(&lines, QStringLiteral("CMAKE_SYSTEM_VERSION"), systemVersion);
    }
    if (!processor.trimmed().isEmpty()) {
        appendCacheEntry(&lines, QStringLiteral("CMAKE_SYSTEM_PROCESSOR"), processor);
    }
    lines.append(QString());
    appendCacheEntry(&lines, QStringLiteral("CMAKE_C_COMPILER"), cCompiler);
    appendCacheEntry(&lines, QStringLiteral("CMAKE_CXX_COMPILER"), cxxCompiler);
    if (!ar.trimmed().isEmpty()) {
        appendCacheEntry(&lines, QStringLiteral("CMAKE_AR"), ar);
    }
    if (!ranlib.trimmed().isEmpty()) {
        appendCacheEntry(&lines, QStringLiteral("CMAKE_RANLIB"), ranlib);
    }
    if (!strip.trimmed().isEmpty()) {
        appendCacheEntry(&lines, QStringLiteral("CMAKE_STRIP"), strip);
    }
    if (!sysroot.trimmed().isEmpty()) {
        lines.append(QString());
        appendCacheEntry(&lines, QStringLiteral("CMAKE_SYSROOT"), sysroot);
        // find_* must not pick a host library out of /usr/lib: the sysroot is
        // the only place a target library can legally come from.
        lines.append(QStringLiteral(
            "set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)"));
        const QString mode = findRootPathMode.trimmed().isEmpty()
                                 ? QStringLiteral("ONLY")
                                 : findRootPathMode.trimmed();
        lines.append(QStringLiteral(
                         "set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY %1)")
                         .arg(mode));
        lines.append(QStringLiteral(
                         "set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE %1)")
                         .arg(mode));
        lines.append(QStringLiteral(
                         "set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE %1)")
                         .arg(mode));
    }
    lines.append(QString());
    // A cross build links with the target's runtime, so the linker must not
    // be handed host paths for the implicit link directories.
    lines.append(QStringLiteral("set(CMAKE_CROSSCOMPILING_EMULATOR \"\" CACHE STRING \"\")"));
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QList<CrossToolchain> inferToolchains(const QString& compilerPath)
{
    // "…/bin/aarch64-linux-gnu-g++" → prefix "aarch64-linux-gnu", directory
    // "…/bin". The prefix is the part before "-g++"/"-gcc"/"-clang++".
    QList<CrossToolchain> toolchains;
    const QFileInfo info(compilerPath);
    const QString base = info.completeBaseName();  // "aarch64-linux-gnu-g++"
    static const QRegularExpression pattern(
        QStringLiteral("^(.*)-(?:g\\+\\+|gcc|clang\\+\\+|clang)$"));
    const auto match = pattern.match(base);
    if (!match.hasMatch()) {
        return toolchains;
    }
    const QString prefix = match.captured(1);
    if (prefix.isEmpty()) {
        return toolchains;
    }
    const QDir directory = info.absoluteDir();
    CrossToolchain toolchain;
    toolchain.name = prefix;
    toolchain.cCompiler = directory.filePath(prefix + QStringLiteral("-gcc"));
    toolchain.cxxCompiler = directory.filePath(prefix + QStringLiteral("-g++"));
    if (!QFileInfo::exists(toolchain.cCompiler)) {
        toolchain.cCompiler = directory.filePath(prefix + QStringLiteral("-clang"));
    }
    if (!QFileInfo::exists(toolchain.cxxCompiler)) {
        toolchain.cxxCompiler = directory.filePath(prefix + QStringLiteral("-clang++"));
    }
    toolchain.ar = directory.filePath(prefix + QStringLiteral("-ar"));
    toolchain.ranlib = directory.filePath(prefix + QStringLiteral("-ranlib"));
    toolchain.strip = directory.filePath(prefix + QStringLiteral("-strip"));

    // The system name is the middle field of the triplet: aarch64-linux-gnu,
    // x86_64-w64-mingw32, arm-none-eabi. CMake's names are the capitalised
    // form of the OS field.
    const QStringList parts = prefix.split(QLatin1Char('-'));
    if (parts.size() >= 2) {
        toolchain.processor = parts.at(0);
        const QString os = parts.at(1).toLower();
        if (os == QLatin1String("linux")) {
            toolchain.systemName = QStringLiteral("Linux");
        } else if (os == QLatin1String("w64") || os == QLatin1String("mingw32")
                   || os == QLatin1String("windows")) {
            toolchain.systemName = QStringLiteral("Windows");
        } else if (os == QLatin1String("darwin") || os == QLatin1String("apple")) {
            toolchain.systemName = QStringLiteral("Darwin");
        } else if (os == QLatin1String("none") || os == QLatin1String("eabi")
                   || os == QLatin1String("elf")) {
            toolchain.systemName = QStringLiteral("Generic");
        } else {
            toolchain.systemName = os.left(1).toUpper() + os.mid(1);
        }
    }
    // A sysroot next to the compiler is the layout every modern
    // cross-toolchain package uses.
    const QDir prefixDir(directory.filePath(QStringLiteral("..")));
    const QString sysroot = prefixDir.filePath(prefix);
    if (QFileInfo(sysroot).isDir()) {
        toolchain.sysroot = QDir(sysroot).absolutePath();
    }
    toolchains.append(completeToolchain(toolchain));
    return toolchains;
}

CrossToolchain completeToolchain(CrossToolchain toolchain)
{
    const QFileInfo cxx(toolchain.cxxCompiler);
    const QDir directory = cxx.absoluteDir();
    if (toolchain.ar.isEmpty()) {
        toolchain.ar = directory.filePath(cxx.completeBaseName().replace(
            QStringLiteral("g++"), QStringLiteral("ar")));
        if (!QFileInfo::exists(toolchain.ar)) {
            toolchain.ar.clear();
        }
    }
    if (toolchain.ranlib.isEmpty()) {
        const QString candidate = directory.filePath(cxx.completeBaseName().replace(
            QStringLiteral("g++"), QStringLiteral("ranlib")));
        if (QFileInfo::exists(candidate)) {
            toolchain.ranlib = candidate;
        }
    }
    if (toolchain.strip.isEmpty()) {
        const QString candidate = directory.filePath(cxx.completeBaseName().replace(
            QStringLiteral("g++"), QStringLiteral("strip")));
        if (QFileInfo::exists(candidate)) {
            toolchain.strip = candidate;
        }
    }
    if (toolchain.findRootPathMode.isEmpty()) {
        toolchain.findRootPathMode = QStringLiteral("ONLY");
    }
    return toolchain;
}

QStringList toolchainArguments(const QString& toolchainFile,
                               const CrossToolchain& toolchain)
{
    QStringList arguments;
    if (toolchainFile.trimmed().isEmpty()) {
        return arguments;
    }
    arguments << QStringLiteral("-DCMAKE_TOOLCHAIN_FILE=") + toolchainFile;
    // Repeating the two compilers in the cache is not redundant: a stale
    // CMakeCache.txt keeps the previous compiler, and the toolchain file is
    // only read on a fresh configure.
    if (!toolchain.cCompiler.trimmed().isEmpty()) {
        arguments << QStringLiteral("-DCMAKE_C_COMPILER=") + toolchain.cCompiler;
    }
    if (!toolchain.cxxCompiler.trimmed().isEmpty()) {
        arguments << QStringLiteral("-DCMAKE_CXX_COMPILER=") + toolchain.cxxCompiler;
    }
    if (!toolchain.systemName.trimmed().isEmpty()) {
        arguments << QStringLiteral("-DCMAKE_SYSTEM_NAME=") + toolchain.systemName;
    }
    return arguments;
}

bool writeToolchainFile(const QString& path, const CrossToolchain& toolchain,
                        QString* error)
{
    if (!toolchain.isValid()) {
        if (error != nullptr) {
            *error = toolchain.problems().join(QStringLiteral("; "));
        }
        return false;
    }
    const QFileInfo info(path);
    if (!info.absoluteDir().exists() && !QDir().mkpath(info.absolutePath())) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot create %1").arg(info.absolutePath());
        }
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    const QString content = toolchain.toCMakeToolchainFile();
    if (file.write(content.toUtf8()) != content.toUtf8().size()) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Launchers
// ---------------------------------------------------------------------------

QList<CompilerLauncher> detectLaunchers(const QStringList& searchPaths)
{
    QList<CompilerLauncher> launchers;
    const QStringList names{QStringLiteral("ccache"), QStringLiteral("sccache")};
    const QStringList paths = searchPaths.isEmpty()
                                  ? QProcessEnvironment::systemEnvironment()
                                        .value(QStringLiteral("PATH"))
                                        .split(QDir::listSeparator(), Qt::SkipEmptyParts)
                                  : searchPaths;
    for (const QString& name : names) {
        CompilerLauncher launcher;
        launcher.program = name;
#ifdef Q_OS_WIN
        const QStringList suffixes{QStringLiteral(".exe"), QStringLiteral("")};
#else
        const QStringList suffixes{QStringLiteral("")};
#endif
        for (const QString& directory : paths) {
            for (const QString& suffix : suffixes) {
                const QString candidate =
                    QDir(directory).filePath(name + suffix);
                if (QFileInfo(candidate).isFile()) {
                    launcher.path = QFileInfo(candidate).absoluteFilePath();
                    launcher.found = true;
                    break;
                }
            }
            if (launcher.found) {
                break;
            }
        }
        launchers.append(launcher);
    }
    return launchers;
}

QStringList launcherArguments(const CompilerLauncher& launcher)
{
    if (!launcher.isValid()) {
        return {};
    }
    return {
        QStringLiteral("-DCMAKE_C_COMPILER_LAUNCHER=") + launcher.path,
        QStringLiteral("-DCMAKE_CXX_COMPILER_LAUNCHER=") + launcher.path,
    };
}

QList<QPair<QString, QString>> launcherEnvironment(const CompilerLauncher& launcher,
                                                   const LauncherEnvironment& settings)
{
    QList<QPair<QString, QString>> environment;
    if (!launcher.isValid()) {
        return environment;
    }
    const bool sccache = launcher.program.contains(QLatin1String("sccache"));
    if (!settings.cacheDirectory.trimmed().isEmpty()) {
        environment.append(qMakePair(
            sccache ? QStringLiteral("SCCACHE_DIR") : QStringLiteral("CCACHE_DIR"),
            settings.cacheDirectory.trimmed()));
    }
    if (!settings.maxSize.trimmed().isEmpty()) {
        environment.append(qMakePair(
            sccache ? QStringLiteral("SCCACHE_CACHE_SIZE") : QStringLiteral("CCACHE_MAXSIZE"),
            settings.maxSize.trimmed()));
    }
    return environment;
}

QStringList launcherStatsArguments(const CompilerLauncher& launcher)
{
    if (!launcher.isValid()) {
        return {};
    }
    return {QStringLiteral("--show-stats")};
}

QString parseLauncherHitRate(const QString& output)
{
    // ccache: "cache hit (direct)  1234" / "cache hit rate  87.50 %"
    // sccache: "Compile requests executed  100" / "Cache hits  90"
    static const QRegularExpression ccacheRate(
        QStringLiteral("cache hit rate\\s+([0-9.]+)\\s*%"),
        QRegularExpression::CaseInsensitiveOption);
    if (const auto match = ccacheRate.match(output); match.hasMatch()) {
        return QStringLiteral("%1%").arg(match.captured(1));
    }
    static const QRegularExpression sccacheHits(
        QStringLiteral("^Cache hits\\s+(\\d+)$"),
        QRegularExpression::MultilineOption);
    static const QRegularExpression sccacheRequests(
        QStringLiteral("^Compile requests executed\\s+(\\d+)$"),
        QRegularExpression::MultilineOption);
    const auto hits = sccacheHits.match(output);
    const auto requests = sccacheRequests.match(output);
    if (hits.hasMatch() && requests.hasMatch()) {
        const int hitCount = hits.captured(1).toInt();
        const int total = requests.captured(1).toInt();
        if (total > 0) {
            return QStringLiteral("%1%").arg(100.0 * hitCount / total, 0, 'f', 1);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------

QStringList parseTargetList(const QString& helpText)
{
    QStringList targets;
    bool inTargetSection = false;
    const QStringList lines = helpText.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QLatin1String("The following are some of the valid targets"))
            || line.startsWith(QLatin1String("The following targets are available"))) {
            inTargetSection = true;
            continue;
        }
        QString candidate;
        if (line.startsWith(QLatin1String("... "))) {
            candidate = line.mid(4).section(QLatin1Char(' '), 0, 0).trimmed();
        } else if (inTargetSection && !line.contains(QLatin1Char(':'))
                   && !line.contains(QLatin1Char(' '))) {
            // MSBuild lists bare target names under its header.
            candidate = line;
        }
        if (candidate.isEmpty() || isBookkeepingTarget(candidate)) {
            continue;
        }
        if (!targets.contains(candidate)) {
            targets.append(candidate);
        }
    }
    return targets;
}

bool isBookkeepingTarget(const QString& target)
{
    static const QStringList bookkeeping{
        QStringLiteral("all"),      QStringLiteral("clean"),
        QStringLiteral("depend"),   QStringLiteral("help"),
        QStringLiteral("edit_cache"), QStringLiteral("rebuild_cache"),
        QStringLiteral("list_install_components"),
        QStringLiteral("install/local"), QStringLiteral("install/strip"),
        QStringLiteral("preinstall"), QStringLiteral("test"),
        QStringLiteral("RUN_TESTS"), QStringLiteral("ZERO_CHECK"),
        QStringLiteral("ALL_BUILD"),
    };
    return bookkeeping.contains(target);
}

QStringList buildTargetArguments(const QString& target, bool useMsvc)
{
    if (target.trimmed().isEmpty()) {
        return {};
    }
    if (useMsvc) {
        return {QStringLiteral("/t:") + target.trimmed()};
    }
    return {QStringLiteral("--target"), target.trimmed()};
}

QStringList parallelArguments(bool parallel, int jobs)
{
    if (!parallel) {
        return {};
    }
    if (jobs <= 0) {
        return {QStringLiteral("--parallel")};
    }
    return {QStringLiteral("--parallel"), QString::number(jobs)};
}

QStringList configArguments(const QString& config, bool multiConfig)
{
    if (!multiConfig || config.trimmed().isEmpty()) {
        return {};
    }
    return {QStringLiteral("--config"), config.trimmed()};
}

QStringList installArguments(const QString& prefix)
{
    // `--target install` is what builds and installs; the prefix is a
    // configure-time cache variable and is *not* passed here, because
    // `cmake --build` silently ignores anything after `--` that is not a
    // generator flag. installPrefixArguments() carries it to the reconfigure.
    Q_UNUSED(prefix);
    return {QStringLiteral("--target"), QStringLiteral("install")};
}

QStringList installPrefixArguments(const QString& prefix)
{
    if (prefix.trimmed().isEmpty()) {
        return {};
    }
    return {QStringLiteral("-DCMAKE_INSTALL_PREFIX=") + prefix.trimmed()};
}

QStringList packageArguments(const QStringList& generators,
                             const QString& outputDirectory)
{
    QStringList arguments;
    for (const QString& generator : generators) {
        if (!generator.trimmed().isEmpty()) {
            arguments << QStringLiteral("-G") << generator.trimmed();
        }
    }
    if (!outputDirectory.trimmed().isEmpty()) {
        arguments << QStringLiteral("-B") << outputDirectory.trimmed();
    }
    return arguments;
}

QString buildLogFileName(const QString& stem, const QString& timestamp)
{
    QString name = stem.trimmed().isEmpty() ? QStringLiteral("build") : stem.trimmed();
    // A Windows path separator or a colon in a timestamp must not reach the
    // file name.
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")),
                 QStringLiteral("-"));
    QString stamp = timestamp.trimmed();
    stamp.replace(QRegularExpression(QStringLiteral("[^0-9A-Za-z._-]")),
                  QStringLiteral("-"));
    if (stamp.isEmpty()) {
        return name + QStringLiteral(".log");
    }
    return name + QLatin1Char('-') + stamp + QStringLiteral(".log");
}

// ---------------------------------------------------------------------------
// Compile commands
// ---------------------------------------------------------------------------

CompileCommandEntry compileCommandFor(const QString& compileCommandsJson,
                                      const QString& filePath)
{
    CompileCommandEntry entry;
    if (compileCommandsJson.trimmed().isEmpty() || filePath.trimmed().isEmpty()) {
        return entry;
    }
    QJsonParseError error{};
    const QJsonDocument document =
        QJsonDocument::fromJson(compileCommandsJson.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        return entry;
    }
    const QString wanted = QFileInfo(filePath).absoluteFilePath();
    const QString wantedName = QFileInfo(filePath).fileName();
    const QJsonArray array = document.array();
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        const QString file = object.value(QLatin1String("file")).toString();
        if (file.isEmpty()) {
            continue;
        }
        const QString absolute = QFileInfo(file).isAbsolute()
            ? QFileInfo(file).absoluteFilePath()
            : QDir(object.value(QLatin1String("directory")).toString())
                  .absoluteFilePath(file);
        const bool match = absolute == wanted
#ifdef Q_OS_WIN
                           || absolute.compare(wanted, Qt::CaseInsensitive) == 0
#endif
                           || QFileInfo(absolute).fileName() == wantedName;
        if (!match) {
            continue;
        }
        entry.file = absolute;
        entry.directory = object.value(QLatin1String("directory")).toString();
        entry.command = object.value(QLatin1String("command")).toString();
        for (const QJsonValue& argument :
             object.value(QLatin1String("arguments")).toArray()) {
            entry.arguments.append(argument.toString());
        }
        if (entry.arguments.isEmpty() && !entry.command.isEmpty()) {
            entry.arguments = splitCommandLine(entry.command);
        }
        entry.valid = true;
        return entry;
    }
    return entry;
}

QString compileCommandText(const CompileCommandEntry& entry)
{
    if (!entry.valid) {
        return {};
    }
    if (!entry.command.isEmpty()) {
        return entry.command;
    }
    QStringList parts;
    for (const QString& argument : entry.arguments) {
        parts.append(shellQuote(argument));
    }
    return parts.join(QLatin1Char(' '));
}

}  // namespace LightCpp::Workbench::Build
