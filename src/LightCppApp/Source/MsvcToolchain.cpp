// MsvcToolchain — locate an MSVC toolset and describe how to invoke it.
//
// Deliberately avoids vcvarsall.bat: that script drives cmd.exe and reg.exe to
// discover the same information this file reads directly, which makes it
// unusable from a restricted environment and needlessly slow besides. See the
// header for the layouts that are handled.

#include "LightCpp/App/MsvcToolchain.hpp"

#include "LightCpp/App/MsvcNames.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QThread>

#include <array>
#include <algorithm>
#include <utility>

namespace {

/// Shorthand for the public namespace, which is only fully open at the bottom
/// of this file.
namespace Msvc = LightCpp::App::Msvc;

QString msg(const char* text)
{
    return QCoreApplication::translate("MsvcToolchain", text);
}

#ifdef Q_OS_WIN

/// Order two dotted version strings numerically, so "14.9" sorts below "14.30"
/// — a plain string compare gets that backwards and picks the older toolset.
int compareVersionStrings(const QString& lhs, const QString& rhs)
{
    const QStringList a = lhs.split(QLatin1Char('.'));
    const QStringList b = rhs.split(QLatin1Char('.'));
    const int count = qMax(a.size(), b.size());
    for (int i = 0; i < count; ++i) {
        const int av = i < a.size() ? a.at(i).toInt() : 0;
        const int bv = i < b.size() ? b.at(i).toInt() : 0;
        if (av != bv) {
            return av < bv ? -1 : 1;
        }
    }
    return 0;
}

/// Highest version-looking subdirectory of `parent`, optionally restricted to
/// names starting with `prefix`. Empty when there is nothing to choose.
QString newestVersionedSubdir(const QString& parent, const QString& prefix = {})
{
    const QDir dir(parent);
    if (!dir.exists()) {
        return {};
    }
    QString best;
    const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& name : entries) {
        if (!prefix.isEmpty() && !name.startsWith(prefix)) {
            continue;
        }
        if (best.isEmpty() || compareVersionStrings(name, best) > 0) {
            best = name;
        }
    }
    return best;
}

/// Collapse a Visual Studio version into a comparable score. The scale just has
/// to be shared by every source below, so that the newest install wins:
/// vswhere "17.0.…" -> 17000, registry "15.0" -> 15000, Visual C++ 6.0 -> 6000.
int scoreFromVersion(const QString& version)
{
    const QStringList parts = version.split(QLatin1Char('.'));
    const int major = parts.isEmpty() ? 0 : parts.at(0).toInt();
    const int minor = parts.size() > 1 ? parts.at(1).toInt() : 0;
    return major * 1000 + minor;
}

struct Candidate {
    QString root;   ///< directory the layouts below are resolved against
    int score = 0;
};

/// Run a helper process and return its stdout. Empty on any failure — callers
/// treat "no output" and "could not run" the same way.
QString capture(const QString& program, const QStringList& args)
{
    QProcess process;
    process.start(program, args);
    if (!process.waitForStarted(4000)) {
        return {};
    }
    if (!process.waitForFinished(8000)) {
        process.kill();
        process.waitForFinished(2000);
        return {};
    }
    if (process.exitCode() != 0) {
        return {};
    }
    // vswhere emits UTF-8 JSON; fromLocal8Bit mangled non-ASCII install
    // paths (VS previews install under "…/2026Preview/"-style names).
    return QString::fromUtf8(process.readAllStandardOutput());
}

/// The installer's own record of where 32-bit programs live. Read instead of
/// assumed because the environment variable can be missing (a stripped
/// environment, a process that inherited nothing), and because the directory
/// is relocatable: neither "C:" nor "next to Windows" is a given.
QString programFilesDirX86FromRegistry()
{
    const QStringList keys = {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion"),
    };
    for (const QString& key : keys) {
        const QSettings settings(key, QSettings::NativeFormat);
        const QString value =
            settings.value(QStringLiteral("ProgramFilesDir (x86)")).toString();
        if (!value.isEmpty()) {
            return value;
        }
    }
    return {};
}

/// Visual Studio 2017 and later, straight from the installer's own locator.
///
/// vswhere is preferred over the registry because it also reports preview
/// builds: VS 2026 installs side by side with an earlier release and only shows
/// up here, and only when -prerelease is passed.
QList<Candidate> rootsFromVswhere()
{
    // R2-57: this used to fall back to a hard-coded "C:/Program Files (x86)".
    // On a machine whose Windows is elsewhere — or whose 32-bit programs are —
    // that fallback cannot find vswhere at all, so an installed Visual Studio
    // was reported as "No Visual Studio installation was found".
    const QString programsX86 = LightCpp::App::MsvcNames::programFilesX86From(
        qEnvironmentVariable("ProgramFiles(x86)"),
        programFilesDirX86FromRegistry(), qEnvironmentVariable("SystemDrive"));
    QString vswhere;
    if (!programsX86.isEmpty()) {
        vswhere = QDir(programsX86).filePath(
            QStringLiteral("Microsoft Visual Studio/Installer/vswhere.exe"));
    }
    if (!QFileInfo(vswhere).isFile()) {
        return {};
    }

    const QStringList common = {QStringLiteral("-all"), QStringLiteral("-products"),
                                QStringLiteral("*"), QStringLiteral("-format"),
                                QStringLiteral("json")};
    QString json = capture(vswhere, QStringList{QStringLiteral("-prerelease")} + common);
    if (json.isEmpty()) {
        // vswhere older than 2.6 rejects -prerelease outright.
        json = capture(vswhere, common);
    }

    QList<Candidate> out;
    const QJsonArray instances = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const QJsonValue& value : instances) {
        const QJsonObject instance = value.toObject();
        const QString root = instance.value(QStringLiteral("installationPath")).toString();
        if (root.isEmpty()) {
            continue;
        }
        out.append({root,
                    scoreFromVersion(instance.value(QStringLiteral("installationVersion")).toString())});
    }
    return out;
}

/// Visual Studio 2002 through 2019 register here; the newer installer does not.
QList<Candidate> rootsFromRegistry()
{
    QList<Candidate> out;
    const QStringList keys = {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\VisualStudio\\SxS\\VS7"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\VisualStudio\\SxS\\VS7"),
    };
    for (const QString& key : keys) {
        QSettings settings(key, QSettings::NativeFormat);
        // The reading itself lives in the public installRootsIn() so a test can
        // hand it a QSettings file; see the header for the key's shape.
        for (const QPair<QString, QString>& pair : Msvc::installRootsIn(settings)) {
            out.append({pair.second, scoreFromVersion(pair.first)});
        }
    }
    return out;
}

/// Visual C++ 6.0 predates all of the above and keeps its own key.
QList<Candidate> rootsFromVc6()
{
    QList<Candidate> out;
    const QStringList keys = {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\VisualStudio\\6.0\\Setup\\VC"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\VisualStudio\\6.0\\Setup\\VC"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\VisualStudio\\6.0\\Setup\\Microsoft Visual C++"),
    };
    for (const QString& key : keys) {
        QSettings settings(key, QSettings::NativeFormat);
        const QString root = settings.value(QStringLiteral("ProductDir")).toString();
        if (!root.isEmpty()) {
            out.append({root, 6000});
        }
    }
    // Some old VC6 installations have lost their registry entry. Probe
    // only the installer-provided Program Files roots; never assume Windows
    // lives on C: or that 32-bit applications use a particular directory.
    const QStringList programFilesRoots = {
        qEnvironmentVariable("ProgramFiles(x86)"),
        qEnvironmentVariable("ProgramFiles"),
        qEnvironmentVariable("ProgramW6432")
    };
    for (const QString& base : programFilesRoots) {
        if (base.isEmpty()) {
            continue;
        }
        const QString guess =
            QDir(base).filePath(QStringLiteral("Microsoft Visual Studio/VC98"));
        if (QFileInfo(guess).isDir()) {
            out.append({guess, 6000});
        }
    }
    return out;
}

/// Relative layouts used before VS 2017 introduced the versioned
/// VC\Tools\MSVC directory. The first entries that exist win, so a 64-bit
/// toolset is preferred purely because it is listed first.
struct LegacyLayout {
    const char* cl;
    const char* include;
    const char* lib;
};

constexpr std::array<LegacyLayout, 3> kLegacyLayouts{{
    {"VC/bin/amd64", "VC/include", "VC/lib/amd64"},  // VS2005-2015, 64-bit
    {"VC/bin", "VC/include", "VC/lib"},              // VS2002-2015, 32-bit
    {"Bin", "Include", "Lib"},                       // Visual C++ 6.0
}};

/// `arch` receives "x64" or "x86" so the Windows SDK lookup can match.
Msvc::Toolchain resolve(const Candidate& candidate, QString* arch)
{
    Msvc::Toolchain toolchain;
    const QDir root(candidate.root);

    // --- VS 2017+ : VC\Tools\MSVC\<toolset version>\... -------------------
    const QString toolsRoot = root.filePath(QStringLiteral("VC/Tools/MSVC"));
    const QString toolsetName = newestVersionedSubdir(toolsRoot);
    if (!toolsetName.isEmpty()) {
        const QDir toolset(QDir(toolsRoot).filePath(toolsetName));
        // Hostx64 = the compiler itself is 64-bit; the trailing component is
        // what it emits. A 64-bit host targeting x64 is the best default.
        const std::array<std::pair<const char*, const char*>, 4> layouts{{
            {"bin/Hostx64/x64", "lib/x64"},
            {"bin/Hostx64/x86", "lib/x86"},
            {"bin/Hostx86/x64", "lib/x64"},
            {"bin/Hostx86/x86", "lib/x86"},
        }};
        for (const auto& [binRel, libRel] : layouts) {
            const QString cl = toolset.filePath(QString::fromLatin1(binRel) + QStringLiteral("/cl.exe"));
            if (!QFileInfo(cl).isFile()) {
                continue;
            }
            toolchain.clPath = cl;
            toolchain.binDir = QFileInfo(cl).absolutePath();
            toolchain.includeDirs = {toolset.filePath(QStringLiteral("include"))};
            toolchain.libDirs = {toolset.filePath(QString::fromLatin1(libRel))};
            *arch = QString::fromLatin1(libRel).section(QLatin1Char('/'), 1);
            break;
        }
        if (!toolchain.clPath.isEmpty()) {
            // The toolset version is written as "14.30.30705", where 30 is the
            // minor half of the cl.exe version (19.30). Naming the product
            // version as well matters when several are installed side by side,
            // which is the normal case once you have more than one.
            const QStringList parts = toolsetName.split(QLatin1Char('.'));
            if (parts.size() > 1) {
                toolchain.clMinor = parts.at(1).toInt();
            }
            const QString product =
                LightCpp::App::MsvcNames::productName(candidate.score);
            // `product` already carries the installer's own version in
            // parentheses, so the surroundings use a dash rather than a second
            // pair — "(Visual Studio 2022 (17.10), toolset …)" is not a label
            // anyone can parse at a glance.
            toolchain.label = toolchain.clMinor > 0
                    ? QStringLiteral("MSVC 19.%1 \u2014 %2, toolset %3")
                          .arg(toolchain.clMinor)
                          .arg(product, toolsetName)
                    : QStringLiteral("MSVC \u2014 %1, toolset %2")
                          .arg(product, toolsetName);
            return toolchain;
        }
    }

    // --- VS 2002-2015 (and Visual C++ 6.0, via its own layout) -----------
    for (const LegacyLayout& layout : kLegacyLayouts) {
        const QString cl = root.filePath(QString::fromLatin1(layout.cl) + QStringLiteral("/cl.exe"));
        if (!QFileInfo(cl).isFile()) {
            continue;
        }
        toolchain.clPath = cl;
        toolchain.binDir = QFileInfo(cl).absolutePath();
        toolchain.includeDirs = {root.filePath(QString::fromLatin1(layout.include))};
        toolchain.libDirs = {root.filePath(QString::fromLatin1(layout.lib))};
        *arch = root.filePath(QString::fromLatin1(layout.lib)).endsWith(QStringLiteral("amd64"))
                    ? QStringLiteral("x64")
                    : QStringLiteral("x86");
        toolchain.clMinor = 0;  // not derivable from the layout alone
        toolchain.label = QStringLiteral("MSVC (%1)")
                              .arg(LightCpp::App::MsvcNames::productName(
                                  candidate.score));
        return toolchain;
    }

    return toolchain;
}

/// Append the Windows SDK's include and lib directories.
///
/// The SDK installs wherever it likes — KitsRoot10 on this machine points at
/// D:\Windows Kits\10 — so the location has to be read, not assumed.
bool appendWindowsSdk(const QString& arch, Msvc::Toolchain* toolchain)
{
    QString kitsRoot;
    const QStringList keys = {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\Windows Kits\\Installed Roots"),
    };
    for (const QString& key : keys) {
        const QSettings settings(key, QSettings::NativeFormat);
        const QString value = settings.value(QStringLiteral("KitsRoot10")).toString();
        if (!value.isEmpty()) {
            kitsRoot = value;
            break;
        }
    }
    if (kitsRoot.isEmpty()) {
        toolchain->problem = msg("The Windows SDK was not found "
                                 "(the KitsRoot10 registry value is missing). "
                                 "Install the \"Desktop development with C++\" workload "
                                 "or a Windows SDK.");
        return false;
    }

    const QString version = newestVersionedSubdir(QDir(kitsRoot).filePath(QStringLiteral("Include")),
                                                  QStringLiteral("10."));
    if (version.isEmpty()) {
        toolchain->problem = msg("The Windows SDK at \"%1\" has no versioned headers.")
                                 .arg(QDir::toNativeSeparators(kitsRoot));
        return false;
    }

    const QDir include(QDir(kitsRoot).filePath(QStringLiteral("Include/") + version));
    const QDir lib(QDir(kitsRoot).filePath(QStringLiteral("Lib/") + version));
    // ucrt is the C runtime, shared holds headers used by both kernel and user
    // mode, um is user mode (windows.h). winrt is only needed for WinRT apps.
    for (const char* sub : {"ucrt", "shared", "um"}) {
        toolchain->includeDirs.append(include.filePath(QString::fromLatin1(sub)));
    }
    toolchain->libDirs.append(lib.filePath(QStringLiteral("ucrt/") + arch));
    toolchain->libDirs.append(lib.filePath(QStringLiteral("um/") + arch));
    return true;
}

#endif  // Q_OS_WIN

}  // namespace

namespace LightCpp::App::Msvc {

QList<QPair<QString, QString>> installRootsIn(QSettings& settings)
{
    QList<QPair<QString, QString>> out;

    // The documented shape: one *value* per version, the data being the
    // install root. This is what `reg query` prints and what the key has held
    // since Visual Studio 2002 — and what this code used to miss entirely,
    // because it asked for subkeys.
    const QStringList versions = settings.childKeys();
    for (const QString& version : versions) {
        const QString root = settings.value(version).toString().trimmed();
        if (!root.isEmpty()) {
            out.append({version, root});
        }
    }

    // The shape the old code assumed: a subkey per version whose *default*
    // value is the path. Kept because reading it costs one call and no
    // candidate is trusted anyway — anything that is not an install with a
    // cl.exe under it is dropped a few lines later.
    const QStringList groups = settings.childGroups();
    for (const QString& version : groups) {
        const QString root =
            settings.value(version + QStringLiteral("/.")).toString().trimmed();
        if (!root.isEmpty()) {
            out.append({version, root});
        }
    }

    return out;
}

Toolchain detect()
{
    QList<Candidate> candidates = rootsFromVswhere();
    candidates += rootsFromRegistry();
    candidates += rootsFromVc6();

    if (candidates.isEmpty()) {
        Toolchain toolchain;
        toolchain.problem = msg("No Visual Studio installation was found.");
        return toolchain;
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& lhs, const Candidate& rhs) {
                         return lhs.score > rhs.score;
                     });

    Toolchain best;
    QString bestProblem = msg("Visual Studio was found, but no C++ compiler is installed. "
                              "Add the \"Desktop development with C++\" workload.");
    for (const Candidate& candidate : candidates) {
        QString arch = QStringLiteral("x64");
        Toolchain toolchain = resolve(candidate, &arch);
        if (toolchain.clPath.isEmpty()) {
            continue;
        }
        if (!appendWindowsSdk(arch, &toolchain)) {
            // Remember why, in case no other install does better.
            if (bestProblem.isEmpty() || candidate.score >= 15000) {
                bestProblem = toolchain.problem;
            }
            continue;
        }
        // Hand clangd the same target the compiler produces code for, so it
        // parses windows.h and the STL with the matching ABI. The language
        // standard is deliberately left alone: clang defaults to C++14 on the
        // msvc targets precisely because cl.exe does, so clangd already agrees
        // with what this toolchain compiles.
        toolchain.targetTriple = arch == QStringLiteral("x64")
                                     ? QStringLiteral("x86_64-pc-windows-msvc")
                                     : QStringLiteral("i686-pc-windows-msvc");
        return toolchain;
    }

    best.problem = bestProblem;
    return best;
}

ToolchainDetector::ToolchainDetector(QObject* parent) : QObject(parent)
{
    // The result crosses threads through a queued signal, so the meta-object
    // system has to know the type by name.
    qRegisterMetaType<LightCpp::App::Msvc::Toolchain>();
}

ToolchainDetector::~ToolchainDetector()
{
    if (thread_ == nullptr) {
        return;
    }
    // detect() has no cancellation path — its blocking vswhere waits are the
    // reason it runs here in the first place — so the only safe teardown is
    // to let it finish. The worker below emits through `this`, which must
    // therefore stay alive; the wait is bounded by vswhere's own timeouts.
    thread_->wait();
    delete thread_;
    thread_ = nullptr;
}

void ToolchainDetector::start()
{
    if (thread_ != nullptr) {
        return;
    }
    thread_ = QThread::create([this] {
        result_ = LightCpp::App::Msvc::detect();
        emit detected(result_);
    });
    thread_->start();
}

bool ToolchainDetector::isRunning() const
{
    return thread_ != nullptr && !thread_->isFinished();
}

bool ToolchainDetector::waitForFinished(int msecs)
{
    if (thread_ == nullptr) {
        return true;
    }
    // Joining the thread publishes result_/finished_ to this thread, so the
    // plain (non-atomic) members need no further synchronisation.
    return thread_->wait(QDeadlineTimer(msecs)) || thread_->isFinished();
}

ToolchainHeaders clangdHeaders(const Toolchain& toolchain)
{
    ToolchainHeaders headers;
    if (!toolchain.isValid()) {
        return headers;
    }
    headers.target = toolchain.targetTriple;
    // Same include directories cl.exe sees through INCLUDE, so the editor and
    // the compiler cannot disagree about which <iostream> is meant.
    headers.includeDirs = toolchain.includeDirs;
    // MSVC's standard library picks its code paths from _MSC_VER, so clangd has
    // to be told the version this compiler actually reports; otherwise clang
    // assumes a newer MSVC than is installed and takes a branch of the same
    // headers the compiler never would.
    const QStringList msCompat{QStringLiteral("-fms-compatibility"),
                               QStringLiteral("-fms-extensions")};
    headers.extraFlags = msCompat;
    if (toolchain.clMinor > 0) {
        headers.extraFlags << QStringLiteral("-fms-compatibility-version=19.%1")
                                  .arg(toolchain.clMinor);
    }
    return headers;
}

}  // namespace LightCpp::App::Msvc
