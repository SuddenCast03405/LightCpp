#pragma once

#include "LightCpp/Settings/SettingsTypes.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <optional>

namespace LightCpp::Settings {

/// Loads, merges and resolves the IDE's configuration.
///
/// Layout, in the spirit of VSCode but next to the executable:
///
///   <exe dir>/config/*.json        — the base layer (machine scope)
///   <workspace>/config/*.json      — overlay layers, merged in order on top
///
/// Each file holds one JSON document with a "type" field, or an array of
/// such documents. Documents of type "toolchains" / "defaults" /
/// "preferences" are singletons; "kit" documents are identified by their
/// "id". When two layers define the same document, they are merged field by
/// field with the later layer winning (objects merge recursively, arrays
/// and scalars replace).
///
/// The store never writes; it only reads what is there and resolves
/// defaults. Missing directories and unreadable files are recorded in
/// lastErrors() and otherwise ignored — a broken config must not take the
/// IDE down.
class SettingsStore {
public:
    SettingsStore() = default;

    /// <directory of the running executable>/config — the base layer.
    /// Deliberately the executable's directory, not the working directory:
    /// the working directory is wherever the user happened to start from,
    /// and settings must not depend on that.
    [[nodiscard]] static QString baseConfigDirectory();

    /// Write one JSON document (object or array) to `filePath`, indentéd.
    /// Used by the default-config writer; the store itself never writes.
    static bool writeDocument(const QString& filePath, const QJsonDocument& document);

    /// Directories whose config documents are merged over the base, in the
    /// given order (later wins). Each entry is the config directory itself
    /// (…/config), not the workspace root.
    void setOverlayDirectories(const QStringList& dirs);
    [[nodiscard]] const QStringList& overlayDirectories() const noexcept
    {
        return overlayDirs_;
    }

    /// Test/alternative-root hook: use `dir` as the base layer instead of
    /// <exe dir>/config. Empty (the default) restores the executable path.
    void setBaseDirectory(const QString& dir);

    /// Compiled-in tool locations used when the settings do not name one.
    /// The IDE passes its known MSYS2 paths; resolution falls back to these
    /// so a fresh install with no config files still works exactly as
    /// before.
    void setFallbackToolPaths(const QString& clangdPath,
                              const QString& clangFormatPath,
                              const QString& gitPath);

    /// Re-read base + overlays. Always safe to call again; returns false
    /// only when the base directory exists but nothing could be read from
    /// it (a missing directory is not an error — the built-in defaults
    /// apply).
    bool reload();

    /// Human-readable problems from the last reload (bad JSON, bad types).
    [[nodiscard]] const QStringList& lastErrors() const noexcept
    {
        return errors_;
    }

    // --- typed views over the merged documents ------------------------------

    [[nodiscard]] GlobalDefaults defaults() const;
    [[nodiscard]] ToolchainLists toolchains() const;
    [[nodiscard]] Preferences preferences() const;

    /// Kits in document order (base first, then overlays), enabled or not.
    [[nodiscard]] QList<Kit> kits() const;

    [[nodiscard]] std::optional<Kit> kit(const QString& id) const;

    /// The kit to start with: the one named by defaults.defaultKitId when it
    /// exists and is enabled, else the first enabled kit marked isDefault,
    /// else the first enabled kit. nullopt when nothing is usable.
    [[nodiscard]] std::optional<Kit> defaultKit() const;

    // --- resolution ---------------------------------------------------------

    /// Tool paths: the setting when present, else the compiled-in fallback.
    [[nodiscard]] QString clangdPath() const;
    [[nodiscard]] QString clangFormatPath() const;
    [[nodiscard]] QString gitPath() const;

    /// The line ending files should be written with: the explicit setting
    /// ("LF"/"CRLF"), else the platform override for the running OS, else
    /// the platform's own convention (CRLF on Windows, LF elsewhere).
    [[nodiscard]] QString lineEndingForSaving() const;

    /// Effective flags for the build configuration called `configName` on
    /// `kit`. When the kit defines a configuration of that name, its flags
    /// are combined with the kit's according to inheritFromKit; any other
    /// name yields the kit's own flags under that name.
    [[nodiscard]] EffectiveBuildConfig effectiveBuildConfig(
        const Kit& kit, const QString& configName) const;

    /// The configuration names a kit contributes to the toolbar: the names
    /// of its buildConfigurations when it defines any, else Debug/Release.
    [[nodiscard]] QStringList buildConfigurationNames(const Kit& kit) const;

private:
    /// Recursive field merge: `overlay` values win; nested objects merge.
    [[nodiscard]] static QJsonObject mergeObjects(const QJsonObject& base,
                                                  const QJsonObject& overlay);

    /// Parse one file and merge its documents into the current state.
    void mergeFile(const QString& filePath);

    /// Merge one classified document into the state. Returns false when the
    /// "type" is unknown.
    bool mergeDocument(const QJsonObject& document, const QString& source);

    QList<QPair<QString, QJsonObject>> kitDocs_;  ///< id -> merged doc, in order
    QJsonObject mergedToolchains_;
    QJsonObject mergedDefaults_;
    QJsonObject mergedPreferences_;
    QString baseDir_;  ///< empty = the executable-relative default
    QStringList overlayDirs_;
    QString fallbackClangd_;
    QString fallbackClangFormat_;
    QString fallbackGit_;
    QStringList errors_;
};

}  // namespace LightCpp::Settings
