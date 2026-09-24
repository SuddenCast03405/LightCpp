#pragma once

#include "LightCpp/Settings/SettingsTypes.hpp"

#include <QDialog>
#include <QHash>
#include <QString>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QGroupBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QStackedWidget;

namespace LightCpp::App {

/// Modal editor for the IDE's configuration documents.
///
/// The four document kinds the settings system understands are edited on
/// four pages: defaults, the toolchain registries, the kits and the
/// preferences. The dialog reads and writes one *layer* at a time — the
/// base layer (<exe dir>/config) or the open project's own config/
/// overlay — using the same four standard file names the starter writer
/// produces (toolchains.json, kits.json, defaults.json, preferences.json).
/// Other files in the directory are neither read nor touched, so the dialog
/// can never destroy configuration it does not understand.
///
/// Round-tripping goes through SettingsJson's typed (de)serialisation: a
/// document is parsed into the structs, the structs fill the widgets, and
/// saving converts the widgets back through toJson(). That means unknown
/// JSON keys inside the four standard files are dropped on save — the same
/// trade-off the default-config writer already makes.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    /// Which config layer the dialog edits. The base layer lives next to
    /// the executable; the overlay is the open project's config/ directory.
    enum class Scope { Base, Overlay };

    explicit SettingsDialog(QWidget* parent = nullptr);

    /// Required before exec(): the base layer directory (…/config). Resets
    /// anything loaded for the base layer and re-reads it from the new
    /// directory.
    void setBaseDirectory(const QString& dir);

    /// Optional: the project overlay directory (…/config). Passing an empty
    /// string (or never calling this) hides the overlay scope entirely.
    void setOverlayDirectory(const QString& dir);

    /// Open the dialog editing `scope` (default: the base layer).
    void setInitialScope(Scope scope);

    /// Write `scope`'s working state to its four standard files, creating
    /// the directory when needed. Returns false and fills `errorMessage`
    /// when the directory is unknown or a file could not be written.
    bool saveScope(Scope scope, QString* errorMessage = nullptr);

    void accept() override;
    void reject() override;

private:
    /// Working state of one layer. `loaded` is set once the layer has been
    /// read from disk (lazily, on first switch); `modified` marks layers
    /// with edits that have not been written yet.
    struct ScopeState {
        LightCpp::Settings::GlobalDefaults defaults;
        LightCpp::Settings::ToolchainLists toolchains;
        LightCpp::Settings::Preferences preferences;
        QList<LightCpp::Settings::Kit> kits;
        bool loaded = false;
        bool modified = false;
    };

    /// RAII over loading_: while >0, programmatic widget updates do not
    /// mark the layer dirty and selection handlers skip their commits.
    struct LoadGuard;

    // --- page builders ------------------------------------------------------
    QWidget* buildDefaultsPage();
    QWidget* buildToolchainsPage();
    QWidget* buildKitsPage();
    QWidget* buildPreferencesPage();
    /// Wrap a page in a frameless, resizable scroll area.
    QWidget* wrapInScroll(QWidget* page);
    /// The scope combo changed: offer to save the layer being left, then
    /// load the target layer into the widgets.
    void handleScopeChanged(int index);

    // --- widget factories (wire markDirty) ----------------------------------
    QLineEdit* makeLine(const QString& objectName);
    QPlainTextEdit* makeTextEdit(const QString& objectName, int fixedHeight);
    QComboBox* makeCombo(const QString& objectName, bool editable);
    QCheckBox* makeCheck(const QString& objectName, const QString& text);

    // --- state <-> ui -------------------------------------------------------
    ScopeState& state(Scope scope) { return states_[static_cast<int>(scope)]; }
    [[nodiscard]] const ScopeState& state(Scope scope) const
    {
        return states_[static_cast<int>(scope)];
    }
    QList<LightCpp::Settings::Kit>& kits() { return state(currentScope_).kits; }
    [[nodiscard]] const QString& directoryFor(Scope scope) const;
    void loadScopeStateFromDisk(Scope scope);
    void loadScopeToUi(Scope scope);
    void commitUiToState();

    void loadDefaultsPage(const LightCpp::Settings::GlobalDefaults& defaults);
    void commitDefaultsPage(LightCpp::Settings::GlobalDefaults* defaults);
    void loadToolchainsPage(const LightCpp::Settings::ToolchainLists& lists);
    void commitToolchainsPage(LightCpp::Settings::ToolchainLists* lists);
    void loadPreferencesPage(const LightCpp::Settings::Preferences& prefs);
    void commitPreferencesPage(LightCpp::Settings::Preferences* prefs);

    // --- kit master-detail --------------------------------------------------
    void loadKitForm(int kitIndex);
    void commitKitForm();
    void loadConfigForm(int configIndex);
    void commitConfigForm();
    void rebuildKitList(int selectRow);
    void rebuildConfigList(int selectRow);
    void addKit();
    void duplicateKit();
    void removeKit();
    void addBuildConfiguration();
    void removeBuildConfiguration();
    void updateKitSuggestions();
    void updateDefaultsKitItems();
    void clearConfigWidgets(bool enabled);

    // --- save ----------------------------------------------------------------
    [[nodiscard]] bool validateScope(Scope scope, QString* errorMessage) const;
    /// Commit the widgets into the working state, then validate and save
    /// every layer the user edited. Shows an error box and returns false on
    /// the first failure.
    bool saveModifiedScopes();
    void markDirty();
    void setStatus(const QString& text);
    [[nodiscard]] QString scopeDisplayName(Scope scope) const;
    void restoreScopeCombo();
    [[nodiscard]] QString uniqueKitId(const QString& base);
    [[nodiscard]] QString uniqueConfigName(const QString& base);

    // --- scope / working state ----------------------------------------------
    ScopeState states_[2];
    Scope currentScope_ = Scope::Base;
    QString baseDir_;
    QString overlayDir_;
    int loading_ = 0;  ///< >0 while widgets are being filled programmatically

    // --- shell ---------------------------------------------------------------
    QComboBox* scopeCombo_ = nullptr;
    QListWidget* nav_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    // --- defaults page -------------------------------------------------------
    QComboBox* defaultsKitId_ = nullptr;
    QComboBox* defaultsBuildConfig_ = nullptr;
    QLineEdit* defaultsGitPath_ = nullptr;
    QLineEdit* defaultsClangdPath_ = nullptr;
    QLineEdit* defaultsClangFormatPath_ = nullptr;

    // --- toolchains page -----------------------------------------------------
    /// Registry key ("cCompilers", ..., "cmake", ...) -> its path list.
    QHash<QString, QListWidget*> registryLists_;

    // --- kits page: kit form -------------------------------------------------
    QListWidget* kitList_ = nullptr;
    QWidget* kitFormContainer_ = nullptr;
    QLineEdit* kitId_ = nullptr;
    QLineEdit* kitName_ = nullptr;
    QCheckBox* kitEnabled_ = nullptr;
    QCheckBox* kitIsDefault_ = nullptr;
    QComboBox* kitCCompiler_ = nullptr;
    QComboBox* kitCppCompiler_ = nullptr;
    QComboBox* kitDebugger_ = nullptr;
    QComboBox* kitClangd_ = nullptr;
    QComboBox* kitClangFormat_ = nullptr;
    QComboBox* kitToolCmake_ = nullptr;
    QComboBox* kitToolNinja_ = nullptr;
    QComboBox* kitToolGnuMake_ = nullptr;
    QComboBox* kitToolMsbuild_ = nullptr;
    QComboBox* kitCppStandard_ = nullptr;
    QPlainTextEdit* kitCompileFlags_ = nullptr;
    QPlainTextEdit* kitLinkFlags_ = nullptr;
    QLineEdit* kitGitPath_ = nullptr;
    QPlainTextEdit* kitGitArgs_ = nullptr;
    QComboBox* kitClangFormatStyle_ = nullptr;
    QComboBox* kitClangdFallbackStyle_ = nullptr;
    int currentKitIndex_ = -1;

    // --- kits page: build-configuration form ---------------------------------
    QGroupBox* configGroup_ = nullptr;  // needed by QGroupBox forward decl? no — include
    QWidget* configFormContainer_ = nullptr;
    QListWidget* configList_ = nullptr;
    QLineEdit* configName_ = nullptr;
    QCheckBox* configInherit_ = nullptr;
    QPlainTextEdit* configCompileFlags_ = nullptr;
    QPlainTextEdit* configLinkFlags_ = nullptr;
    QComboBox* configCppStandard_ = nullptr;
    QPlainTextEdit* configArgsCmake_ = nullptr;
    QPlainTextEdit* configArgsNinja_ = nullptr;
    QPlainTextEdit* configArgsGnuMake_ = nullptr;
    QPlainTextEdit* configArgsMsbuild_ = nullptr;
    int currentConfigIndex_ = -1;

    // --- preferences page ----------------------------------------------------
    QFontComboBox* toolbarFontFamily_ = nullptr;
    QDoubleSpinBox* toolbarFontSize_ = nullptr;
    QFontComboBox* editorFontFamily_ = nullptr;
    QDoubleSpinBox* editorFontSize_ = nullptr;
    QFontComboBox* outputFontFamily_ = nullptr;
    QDoubleSpinBox* outputFontSize_ = nullptr;
    QComboBox* encodingDefault_ = nullptr;
    QPlainTextEdit* encodingPriority_ = nullptr;
    QComboBox* lineEndingDefault_ = nullptr;
    QComboBox* lineEndingWindows_ = nullptr;
    QComboBox* lineEndingLinux_ = nullptr;
    QComboBox* lineEndingMacos_ = nullptr;
    QKeySequenceEdit* shortcutClangd_ = nullptr;
    QKeySequenceEdit* shortcutClangFormat_ = nullptr;
    QSpinBox* largeFileHighlight_ = nullptr;
    QSpinBox* largeFileIndex_ = nullptr;
    QSpinBox* largeFileParse_ = nullptr;
    QCheckBox* clangdEnabledByDefault_ = nullptr;
    QCheckBox* clangdCompletion_ = nullptr;
    QCheckBox* clangdDiagnostics_ = nullptr;
    QCheckBox* clangdIndexing_ = nullptr;
    QComboBox* clangdFallbackStyle_ = nullptr;
    QCheckBox* clangFormatEnabled_ = nullptr;
    QCheckBox* clangFormatOnSave_ = nullptr;
    QComboBox* clangFormatStyle_ = nullptr;
    QComboBox* formattingGlobalStyle_ = nullptr;
    QPlainTextEdit* formattingDefaultStyleContent_ = nullptr;
};

}  // namespace LightCpp::App
