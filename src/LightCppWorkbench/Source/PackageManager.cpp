// PackageManager — implementation. See the header for why the spellings matter.

#include "LightCpp/Workbench/PackageManager.hpp"

#include <QRegularExpression>

namespace LightCpp::Workbench::Pkg {
namespace {

QString firstLine(const QString& text)
{
    return text.section(QLatin1Char('\n'), 0, 0).trimmed();
}

/// vcpkg's list output is fixed-column-ish but space-separated; the name may
/// carry a `:triplet` suffix, which is the only reliable way to tell the
/// package from its triplet on an old install.
Package parseVcpkgLine(const QString& line)
{
    Package package;
    QString rest = line.trimmed();
    const QStringList parts = rest.split(QRegularExpression(QStringLiteral("\\s+")),
                                         Qt::SkipEmptyParts);
    if (parts.size() < 2) {
        return package;
    }
    package.name = parts.at(0);
    const qsizetype colon = package.name.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        package.triplet = package.name.mid(colon + 1);
        package.name = package.name.left(colon);
    }
    package.version = parts.at(1);
    if (parts.size() > 2) {
        package.description = parts.mid(2).join(QLatin1Char(' '));
    }
    package.installed = true;
    return package;
}

}  // namespace

QString managerName(Manager manager)
{
    switch (manager) {
    case Manager::Vcpkg:  return QStringLiteral("vcpkg");
    case Manager::Conan:  return QStringLiteral("Conan");
    case Manager::Hunter: return QStringLiteral("Hunter");
    case Manager::Apt:    return QStringLiteral("apt");
    case Manager::Brew:   return QStringLiteral("Homebrew");
    case Manager::Winget: return QStringLiteral("winget");
    case Manager::Unknown: break;
    }
    return QStringLiteral("Unknown");
}

QString managerId(Manager manager)
{
    switch (manager) {
    case Manager::Vcpkg:  return QStringLiteral("vcpkg");
    case Manager::Conan:  return QStringLiteral("conan");
    case Manager::Hunter: return QStringLiteral("hunter");
    case Manager::Apt:    return QStringLiteral("apt");
    case Manager::Brew:   return QStringLiteral("brew");
    case Manager::Winget: return QStringLiteral("winget");
    case Manager::Unknown: break;
    }
    return QStringLiteral("unknown");
}

Manager managerFromId(const QString& id)
{
    const QString lower = id.trimmed().toLower();
    if (lower == QLatin1String("vcpkg")) return Manager::Vcpkg;
    if (lower == QLatin1String("conan")) return Manager::Conan;
    if (lower == QLatin1String("hunter")) return Manager::Hunter;
    if (lower == QLatin1String("apt") || lower == QLatin1String("apt-get")) return Manager::Apt;
    if (lower == QLatin1String("brew") || lower == QLatin1String("homebrew")) return Manager::Brew;
    if (lower == QLatin1String("winget")) return Manager::Winget;
    return Manager::Unknown;
}

QStringList versionArguments(Manager manager)
{
    switch (manager) {
    case Manager::Vcpkg:  return {QStringLiteral("version")};
    case Manager::Conan:  return {QStringLiteral("--version")};
    case Manager::Apt:    return {QStringLiteral("--version")};
    case Manager::Brew:   return {QStringLiteral("--version")};
    case Manager::Winget: return {QStringLiteral("--version")};
    case Manager::Hunter:
        // Hunter is a CMake module fetched by HunterGate.cmake; there is no
        // executable to ask. An empty list is the signal not to probe.
        return {};
    case Manager::Unknown:
        break;
    }
    return {};
}

QString parseVersion(Manager manager, const QString& output)
{
    static const QRegularExpression number(QStringLiteral("(\\d+\\.\\d+(?:\\.\\d+)*)"));
    const QString line = firstLine(output);
    if (line.isEmpty()) {
        return {};
    }
    switch (manager) {
    case Manager::Vcpkg: {
        // "vcpkg package management program version 2023-12-12-…"
        const auto match = number.match(line);
        return match.hasMatch() ? match.captured(1) : QString();
    }
    case Manager::Conan: {
        // "Conan version 2.0.17"
        const auto match = number.match(line);
        return match.hasMatch() ? match.captured(1) : QString();
    }
    case Manager::Brew: {
        // "Homebrew 4.2.0"
        const auto match = number.match(line);
        return match.hasMatch() ? match.captured(1) : QString();
    }
    case Manager::Winget: {
        // "v1.6.3482" — the first token is the version.
        const QString token = line.section(QLatin1Char(' '), 0, 0);
        return token.startsWith(QLatin1Char('v')) ? token.mid(1) : token;
    }
    case Manager::Apt: {
        // "apt 2.4.5 (amd64)"
        const auto match = number.match(line);
        return match.hasMatch() ? match.captured(1) : QString();
    }
    case Manager::Hunter:
    case Manager::Unknown:
        break;
    }
    return {};
}

QString normalizeTriplet(const QString& triplet)
{
    QString normalized = triplet.trimmed().toLower();
    if (normalized.isEmpty()) {
        return hostTriplet();
    }
    return normalized;
}

QString hostTriplet()
{
#if defined(Q_OS_WIN)
#if defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("arm64-windows");
#else
    return QStringLiteral("x64-windows");
#endif
#elif defined(Q_OS_MACOS)
#if defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("arm64-osx");
#else
        return QStringLiteral("x64-osx");
#endif
#elif defined(Q_OS_LINUX)
#if defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("arm64-linux");
#else
    return QStringLiteral("x64-linux");
#endif
#else
    return QStringLiteral("x64-linux");
#endif
}

QStringList installArguments(Manager manager, const QString& name,
                             const QString& version, const QString& triplet)
{
    const QString package = name.trimmed();
    if (package.isEmpty()) {
        return {};
    }
    switch (manager) {
    case Manager::Vcpkg:
        // The triplet is part of the *package spec*, not a flag: `fmt` alone
        // installs the default triplet, and a project built for another one
        // then cannot see it.
        return {QStringLiteral("install"),
                package + QLatin1Char(':') + normalizeTriplet(triplet)};
    case Manager::Conan: {
        QString requirement = package;
        if (!version.trimmed().isEmpty()) {
            requirement += QLatin1Char('/') + version.trimmed();
        }
        return {QStringLiteral("install"), QStringLiteral("--requires=") + requirement,
                QStringLiteral("--build=missing")};
    }
    case Manager::Hunter:
        // Hunter has no install verb: the dependency is declared in
        // CMakeLists.txt and fetched at configure time.
        return {};
    case Manager::Apt:
        return {QStringLiteral("install"), QStringLiteral("-y"), package};
    case Manager::Brew:
        return {QStringLiteral("install"), package};
    case Manager::Winget:
        return {QStringLiteral("install"), QStringLiteral("--id"), package,
                QStringLiteral("--accept-package-agreements"),
                QStringLiteral("--accept-source-agreements")};
    case Manager::Unknown:
        break;
    }
    return {};
}

QStringList removeArguments(Manager manager, const QString& name)
{
    const QString package = name.trimmed();
    if (package.isEmpty()) {
        return {};
    }
    switch (manager) {
    case Manager::Vcpkg:
        return {QStringLiteral("remove"), package};
    case Manager::Conan:
        return {QStringLiteral("remove"), package};
    case Manager::Hunter:
        return {};
    case Manager::Apt:
        // `remove` would leave the config files behind; a dependency the user
        // asked to delete should actually be gone.
        return {QStringLiteral("purge"), QStringLiteral("-y"), package};
    case Manager::Brew:
        return {QStringLiteral("uninstall"), package};
    case Manager::Winget:
        return {QStringLiteral("uninstall"), QStringLiteral("--id"), package};
    case Manager::Unknown:
        break;
    }
    return {};
}

QStringList searchArguments(Manager manager, const QString& query)
{
    const QString text = query.trimmed();
    switch (manager) {
    case Manager::Vcpkg:
        return text.isEmpty() ? QStringList{QStringLiteral("search")}
                              : QStringList{QStringLiteral("search"), text};
    case Manager::Conan:
        return text.isEmpty() ? QStringList{QStringLiteral("search"), QStringLiteral("*")}
                              : QStringList{QStringLiteral("search"), text};
    case Manager::Hunter:
        return {};
    case Manager::Apt:
        return {QStringLiteral("search"), text.isEmpty() ? QStringLiteral("*") : text};
    case Manager::Brew:
        return text.isEmpty() ? QStringList{QStringLiteral("search")}
                              : QStringList{QStringLiteral("search"), text};
    case Manager::Winget:
        return text.isEmpty() ? QStringList{QStringLiteral("search")}
                              : QStringList{QStringLiteral("search"), text};
    case Manager::Unknown:
        break;
    }
    return {};
}

QStringList listArguments(Manager manager)
{
    switch (manager) {
    case Manager::Vcpkg:  return {QStringLiteral("list")};
    case Manager::Conan:  return {QStringLiteral("list"), QStringLiteral("*")};
    case Manager::Hunter: return {};
    case Manager::Apt:    return {QStringLiteral("list"), QStringLiteral("--installed")};
    case Manager::Brew:   return {QStringLiteral("list"), QStringLiteral("--versions")};
    case Manager::Winget: return {QStringLiteral("list")};
    case Manager::Unknown: break;
    }
    return {};
}

QList<Package> parseList(Manager manager, const QString& output)
{
    QList<Package> packages;
    const QStringList lines = output.split(QLatin1Char('\n'));
    switch (manager) {
    case Manager::Vcpkg: {
        // "fmt:x64-windows      10.1.1       A modern formatting library"
        for (const QString& raw : lines) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1String("vcpkg"))
                || !line.contains(QLatin1Char(' '))) {
                continue;
            }
            Package package = parseVcpkgLine(line);
            if (!package.name.isEmpty()) {
                packages.append(package);
            }
        }
        break;
    }
    case Manager::Conan: {
        // v2:  "    zlib/1.2.13#hash"  (indented, under a cache heading)
        // v1:  "zlib/1.2.13@user/stable"
        for (const QString& raw : lines) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || !line.contains(QLatin1Char('/'))) {
                continue;
            }
            const QStringList parts = line.split(QLatin1Char('/'));
            if (parts.size() < 2) {
                continue;
            }
            Package package;
            package.name = parts.at(0);
            QString version = parts.at(1);
            version = version.section(QLatin1Char('#'), 0, 0);
            package.version = version;
            package.installed = true;
            if (!package.name.isEmpty()) {
                packages.append(package);
            }
        }
        break;
    }
    case Manager::Brew: {
        // "fmt 10.1.1" or "python@3.12 3.12.2_1"
        for (const QString& raw : lines) {
            const QStringList parts =
                raw.trimmed().split(QRegularExpression(QStringLiteral("\\s+")),
                                    Qt::SkipEmptyParts);
            if (parts.size() < 2) {
                continue;
            }
            Package package;
            package.name = parts.at(0);
            package.version = parts.at(1);
            package.installed = true;
            packages.append(package);
        }
        break;
    }
    case Manager::Apt: {
        // "libfmt-dev/jammy,now 9.1.0+ds1-2 amd64 [installed]"
        static const QRegularExpression pattern(
            QStringLiteral("^([^/\\s]+)/\\S*\\s+(\\S+)\\s+(\\S+)(.*)$"));
        for (const QString& raw : lines) {
            const auto match = pattern.match(raw.trimmed());
            if (!match.hasMatch()) {
                continue;
            }
            Package package;
            package.name = match.captured(1);
            package.version = match.captured(2);
            package.description = match.captured(3);
            package.installed = true;
            packages.append(package);
        }
        break;
    }
    case Manager::Winget: {
        // A fixed-width table whose separator row is a run of dashes. The
        // columns are Name/Id/Version/Available/Source; ids are the stable
        // key, so a row is parsed only when it has at least four columns and
        // the second looks like a package id (contains a dot).
        static const QRegularExpression separator(QStringLiteral("^-{3,}"));
        for (const QString& raw : lines) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || separator.match(line).hasMatch()
                || line.startsWith(QLatin1String("Name "))) {
                continue;
            }
            const QStringList parts =
                line.split(QRegularExpression(QStringLiteral("\\s{2,}")),
                           Qt::SkipEmptyParts);
            if (parts.size() < 3 || !parts.at(1).contains(QLatin1Char('.'))) {
                continue;
            }
            Package package;
            package.name = parts.at(1);
            package.version = parts.at(2);
            package.description = parts.at(0);
            package.installed = true;
            packages.append(package);
        }
        break;
    }
    case Manager::Hunter:
    case Manager::Unknown:
        break;
    }
    return packages;
}

QList<Package> parseSearch(Manager manager, const QString& output)
{
    QList<Package> packages;
    for (const QString& raw : output.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        switch (manager) {
        case Manager::Vcpkg: {
            // "fmt                 10.1.1         A modern formatting library"
            // "fmt:x64-windows     10.1.1         A modern formatting library"
            const QStringList parts =
                line.split(QRegularExpression(QStringLiteral("\\s{2,}|\\s")),
                           Qt::SkipEmptyParts);
            if (parts.size() < 2 || !parts.at(1).contains(QLatin1Char('.'))) {
                continue;
            }
            Package package;
            package.name = parts.at(0).section(QLatin1Char(':'), 0, 0);
            package.triplet = parts.at(0).section(QLatin1Char(':'), 1, 1);
            package.version = parts.at(1);
            if (parts.size() > 2) {
                package.description = parts.mid(2).join(QLatin1Char(' '));
            }
            packages.append(package);
            break;
        }
        case Manager::Conan: {
            // "Existing recipes in ConanCenter:" then "fmt/10.1.1"
            if (!line.contains(QLatin1Char('/'))) {
                continue;
            }
            const QStringList parts = line.split(QLatin1Char('/'));
            if (parts.size() < 2) {
                continue;
            }
            Package package;
            package.name = parts.at(0);
            package.version = parts.at(1).section(QLatin1Char('#'), 0, 0);
            packages.append(package);
            break;
        }
        case Manager::Brew: {
            // "==> Formulae\nfmt (10.1.1)\n==> Casks"
            static const QRegularExpression formula(
                QStringLiteral("^([A-Za-z0-9@._+-]+) \\(([^)]+)\\)$"));
            const auto match = formula.match(line);
            if (!match.hasMatch()) {
                continue;
            }
            Package package;
            package.name = match.captured(1);
            package.version = match.captured(2);
            packages.append(package);
            break;
        }
        case Manager::Apt: {
            // "libfmt-dev - Formatting library for C++ (development files)"
            const qsizetype dash = line.indexOf(QLatin1String(" - "));
            if (dash <= 0) {
                continue;
            }
            Package package;
            package.name = line.left(dash);
            package.description = line.mid(dash + 3);
            packages.append(package);
            break;
        }
        case Manager::Winget:
        case Manager::Hunter:
        case Manager::Unknown:
            break;
        }
    }
    return packages;
}

QStringList cmakeIntegrationArguments(Manager manager, const QString& root,
                                      const QString& triplet)
{
    switch (manager) {
    case Manager::Vcpkg: {
        const QString toolchain = root + QStringLiteral("/scripts/buildsystems/vcpkg.cmake");
        QStringList arguments{
            QStringLiteral("-DCMAKE_TOOLCHAIN_FILE=") + toolchain,
        };
        const QString normalized = normalizeTriplet(triplet);
        if (!normalized.isEmpty()) {
            arguments << QStringLiteral("-DVCPKG_TARGET_TRIPLET=") + normalized;
        }
        return arguments;
    }
    case Manager::Conan:
        // conan 2 generates the toolchain next to conanfile; the build needs
        // its preset or its toolchain file, and both are per-config.
        return {QStringLiteral("-DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake")};
    case Manager::Brew:
    case Manager::Apt:
    case Manager::Winget: {
        const QString prefix = root.trimmed();
        if (prefix.isEmpty()) {
            return {};
        }
        return {QStringLiteral("-DCMAKE_PREFIX_PATH=") + prefix};
    }
    case Manager::Hunter:
    case Manager::Unknown:
        break;
    }
    return {};
}

QString integrationHint(Manager manager, const QString& root, const QString& triplet)
{
    switch (manager) {
    case Manager::Vcpkg:
        return QStringLiteral("Pass -DCMAKE_TOOLCHAIN_FILE=%1/scripts/buildsystems/vcpkg.cmake "
                              "and -DVCPKG_TARGET_TRIPLET=%2 so the build sees the installed ports.")
            .arg(root, normalizeTriplet(triplet));
    case Manager::Conan:
        return QStringLiteral("Run `conan install . --build=missing` before configuring; the "
                              "generated conan_toolchain.cmake carries the paths.");
    case Manager::Brew:
        return QStringLiteral("Homebrew installs headers under its own prefix; on macOS nothing "
                              "is needed, on Linux pass -DCMAKE_PREFIX_PATH=%1.").arg(root);
    case Manager::Apt:
        return QStringLiteral("System packages are already on the default include path; no "
                              "CMake flag is required.");
    case Manager::Winget:
        return QStringLiteral("winget packages install system-wide; the CMake prefix is only "
                              "needed for packages that ship a CMake config under %1.").arg(root);
    case Manager::Hunter:
        return QStringLiteral("Hunter fetches dependencies during configure through "
                              "HunterGate.cmake; no external command is needed.");
    case Manager::Unknown:
        break;
    }
    return QStringLiteral("No integration available.");
}

bool integratesWithCMakeDirectly(Manager manager)
{
    return manager == Manager::Vcpkg || manager == Manager::Brew
           || manager == Manager::Apt || manager == Manager::Winget;
}

ManifestInfo detectManifests(const QStringList& projectFileNames)
{
    ManifestInfo info;
    for (const QString& name : projectFileNames) {
        const QString lower = name.toLower();
        if (lower == QLatin1String("vcpkg.json")) {
            info.vcpkgManifestPath = name;
        } else if (lower == QLatin1String("conanfile.txt")
                   || lower == QLatin1String("conanfile.py")) {
            info.conanfilePath = name;
        } else if (lower == QLatin1String("huntergate.cmake")) {
            info.hunterGatePath = name;
        }
    }
    if (!info.vcpkgManifestPath.isEmpty()) {
        info.managers.append(Manager::Vcpkg);
    }
    if (!info.conanfilePath.isEmpty()) {
        info.managers.append(Manager::Conan);
    }
    if (!info.hunterGatePath.isEmpty()) {
        info.managers.append(Manager::Hunter);
    }
    return info;
}

}  // namespace LightCpp::Workbench::Pkg
