// PackageManager — the dependency managers lightcpp.1.md §九 asks for
// (vcpkg, conan, Hunter, apt, brew, winget).
//
// A package manager in an IDE is three things: knowing whether the tool is
// installed, asking it what it has, and turning "add fmt" into the command
// plus the CMake flag that makes the build find it. The third part is the one
// that decides whether the feature is useful: installing vcpkg packages and
// then not passing `-DCMAKE_TOOLCHAIN_FILE` leaves the project unable to find
// them, which reads as "the IDE installed the wrong thing".
//
// Every command list here is pure data, so the spellings can be pinned by
// tests — the difference between `vcpkg install fmt:x64-windows` and
// `vcpkg install fmt --triplet x64-windows` is the difference between a working
// button and a silently ignored one.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench::Pkg {

enum class Manager {
    Unknown = 0,
    Vcpkg,
    Conan,
    Hunter,
    Apt,
    Brew,
    Winget,
};

[[nodiscard]] QString managerName(Manager manager);   ///< "vcpkg", "conan", …
[[nodiscard]] QString managerId(Manager manager);     ///< lowercase id
[[nodiscard]] Manager managerFromId(const QString& id);

/// One package, as a list/search command reported it.
struct Package {
    QString name;
    QString version;
    QString description;
    QString triplet;      ///< vcpkg only
    bool installed = false;
};

/// What the probe found.
struct ManagerStatus {
    Manager manager = Manager::Unknown;
    bool installed = false;
    QString executable;
    QString version;
    QString root;         ///< VCPKG_ROOT / conan home / brew prefix
    QString defaultTriplet;
};

/// The command that reports the tool's version (`vcpkg version`,
/// `conan --version`, …). An empty list means "this manager has no CLI"
/// (Hunter is a CMake module), and the probe must not be attempted.
[[nodiscard]] QStringList versionArguments(Manager manager);

/// Parse that version output. A missing number yields an empty string, which
/// the caller reads as "not installed" rather than "version 0".
[[nodiscard]] QString parseVersion(Manager manager, const QString& output);

/// `vcpkg install fmt:x64-windows` / `conan install --requires=fmt/10.1.1` /
/// `brew install fmt` / `apt-get install -y libfmt-dev` / `winget install fmt`.
///
/// Conan gets its v2 form (`conan install --requires=`), because the v1
/// `conan install fmt/10.1.1@` spelling is rejected by every conan 2.x and
/// conan 2 is what a new install gets.
[[nodiscard]] QStringList installArguments(Manager manager, const QString& name,
                                           const QString& version,
                                           const QString& triplet);

[[nodiscard]] QStringList removeArguments(Manager manager, const QString& name);

[[nodiscard]] QStringList searchArguments(Manager manager, const QString& query);

[[nodiscard]] QStringList listArguments(Manager manager);

/// Parse the installed list. Each manager prints a different table; a line
/// that does not fit is skipped rather than guessed at.
[[nodiscard]] QList<Package> parseList(Manager manager, const QString& output);

/// Parse a search result. Installed packages are marked when the output says
/// so; otherwise the flag stays false.
[[nodiscard]] QList<Package> parseSearch(Manager manager, const QString& output);

/// The CMake flag that makes the build find the packages this manager
/// installed — `-DCMAKE_TOOLCHAIN_FILE=…/vcpkg.cmake` for vcpkg, a
/// `CMAKE_PREFIX_PATH` entry for brew, a prepare step for conan, nothing for
/// the system package managers (their headers are already on the default
/// path).
[[nodiscard]] QStringList cmakeIntegrationArguments(Manager manager,
                                                    const QString& root,
                                                    const QString& triplet);

/// A one-line explanation of the integration above, for the "Build" page.
/// Never empty.
[[nodiscard]] QString integrationHint(Manager manager, const QString& root,
                                      const QString& triplet);

/// vcpkg requires the triplet on the install command *and* exports it through
/// the toolchain file; a mismatch silently installs a package the build cannot
/// see. Normalising here keeps both halves in step.
[[nodiscard]] QString normalizeTriplet(const QString& triplet);

/// The default triplet for the host platform, as vcpkg names it.
[[nodiscard]] QString hostTriplet();

/// Managers whose packages the IDE can add to a CMake build without a
/// separate prepare step (conan needs `conan install` first, so it is false).
[[nodiscard]] bool integratesWithCMakeDirectly(Manager manager);

/// The managers a project manifest names, for "this project uses …" in the
/// build page. Detects `vcpkg.json`, `conanfile.txt/.py`, `HunterGate.cmake`,
/// `CMakeLists.txt` with `find_package` on a system package.
struct ManifestInfo {
    QList<Manager> managers;
    QString vcpkgManifestPath;
    QString conanfilePath;
    QString hunterGatePath;
};
[[nodiscard]] ManifestInfo detectManifests(const QStringList& projectFileNames);

}  // namespace LightCpp::Workbench::Pkg
