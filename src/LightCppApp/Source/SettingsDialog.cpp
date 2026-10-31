// SettingsDialog — GUI editor for the IDE's configuration documents.
//
// One dialog, four pages (defaults / toolchains / kits / preferences), one
// layer at a time. Reading goes through SettingsJson's lenient parsers and
// writing through SettingsStore::writeDocument, so the on-disk format is
// exactly the one the starter writer produces and the store merges.

#include "LightCpp/App/SettingsDialog.hpp"

#include "LightCpp/App/KitToolchainOptions.hpp"
#include "LightCpp/Settings/SettingsJson.hpp"
#include "LightCpp/Settings/SettingsStore.hpp"
#include "LightCpp/Settings/ToolchainScanner.hpp"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

namespace LightCpp::App {

namespace Settings = LightCpp::Settings;

namespace {

// The four standard documents the dialog reads and writes. Files with
// other names in the same directory are deliberately neither read nor
// touched: the dialog must never destroy configuration it does not
// understand.
constexpr const char* kConfigFiles[] = {
    "toolchains.json", "kits.json", "defaults.json", "preferences.json"};

constexpr const char* kStylePresets[] = {
    "file", "LLVM", "Google", "Chromium", "Mozilla", "WebKit", "Microsoft", "GNU"};

constexpr const char* kCppStandards[] = {
    "", "c++14", "c++17", "c++20", "gnu++14", "gnu++17", "gnu++20", "c++23", "gnu++23"};

constexpr const char* kEncodingNames[] = {
    "UTF-8", "GB18030", "GBK", "Big5", "Shift-JIS", "EUC-KR",
    "UTF-16LE", "UTF-16BE", "Windows-1252", "ISO-8859-1"};

/// The tool paths a toolchain names, as (role, label) pairs in form order.
struct ToolchainToolSpec {
    const char* key;          ///< "" = the C/C++/debugger/... members
    const char* label;
};
const ToolchainToolSpec kToolchainToolSpecs[] = {
    {"gcc", QT_TRANSLATE_NOOP("SettingsDialog", "gcc")},
    {"g++", QT_TRANSLATE_NOOP("SettingsDialog", "g++")},
    {"clang", QT_TRANSLATE_NOOP("SettingsDialog", "clang")},
    {"clang++", QT_TRANSLATE_NOOP("SettingsDialog", "clang++")},
    {"cl", QT_TRANSLATE_NOOP("SettingsDialog", "cl (MSVC)")},
    {"gdb", QT_TRANSLATE_NOOP("SettingsDialog", "gdb")},
    {"lldb", QT_TRANSLATE_NOOP("SettingsDialog", "lldb")},
    {"clangd", QT_TRANSLATE_NOOP("SettingsDialog", "Clangd")},
    {"clangFormat", QT_TRANSLATE_NOOP("SettingsDialog", "Clang-Format")},
    {"cmake", QT_TRANSLATE_NOOP("SettingsDialog", "CMake")},
    {"ninja", QT_TRANSLATE_NOOP("SettingsDialog", "Ninja")},
    {"gnuMake", QT_TRANSLATE_NOOP("SettingsDialog", "GNU Make")},
    {"msbuild", QT_TRANSLATE_NOOP("SettingsDialog", "MSBuild")},
};

/// Non-empty, trimmed lines of a one-item-per-line editor.
QStringList linesOf(const QPlainTextEdit* edit)
{
    QStringList out;
    const QStringList lines = edit->toPlainText().split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (!line.isEmpty()) {
            out.append(line);
        }
    }
    return out;
}

/// How a kit is named in lists: the display name, else the id, else a
/// placeholder. Shared by the kit list and the commit that refreshes it.
QString kitDisplayName(const Settings::Kit& kit)
{
    if (!kit.name.isEmpty()) {
        return kit.name;
    }
    if (!kit.id.isEmpty()) {
        return kit.id;
    }
    return SettingsDialog::tr("(unnamed kit)");
}

/// How a toolchain is named in lists, mirroring kitDisplayName.
QString toolchainDisplayName(const Settings::Toolchain& toolchain)
{
    if (!toolchain.name.isEmpty()) {
        return toolchain.name;
    }
    if (!toolchain.id.isEmpty()) {
        return toolchain.id;
    }
    return SettingsDialog::tr("(unnamed toolchain)");
}

/// Read/write one of the tool paths by its spec key. Used by the form
/// load/commit so each row needs no dedicated member variable pair.
/// Compiler/debugger keys address the `compilers` map; the rest address
/// their dedicated fields.
QString toolchainPath(const Settings::Toolchain& toolchain, const QString& key)
{
    if (key == QLatin1String("gcc") || key == QLatin1String("g++")
        || key == QLatin1String("clang") || key == QLatin1String("clang++")
        || key == QLatin1String("cl") || key == QLatin1String("gdb")
        || key == QLatin1String("lldb")) {
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

void setToolchainPath(Settings::Toolchain* toolchain, const QString& key,
                      const QString& path)
{
    if (key == QLatin1String("gcc") || key == QLatin1String("g++")
        || key == QLatin1String("clang") || key == QLatin1String("clang++")
        || key == QLatin1String("cl") || key == QLatin1String("gdb")
        || key == QLatin1String("lldb")) {
        if (path.isEmpty()) {
            toolchain->compilers.remove(key);
        } else {
            toolchain->compilers.insert(key, path);
        }
    } else if (key == QLatin1String("clangd")) {
        toolchain->clangd = path;
    } else if (key == QLatin1String("clangFormat")) {
        toolchain->clangFormat = path;
    } else {
        if (path.isEmpty()) {
            toolchain->buildTools.remove(key);
        } else {
            toolchain->buildTools.insert(key, path);
        }
    }
}

void fillStyleCombo(QComboBox* combo)
{
    for (const char* style : kStylePresets) {
        combo->addItem(QLatin1String(style));
    }
}

void fillStandardCombo(QComboBox* combo)
{
    for (const char* standard : kCppStandards) {
        combo->addItem(QLatin1String(standard));
    }
}

/// Fonts are stored as a spec that must have both a family and a size; a
/// missing side means "keep the built-in default". The family combo's first
/// item is the placeholder for that.
void loadFontSpec(QFontComboBox* family, QDoubleSpinBox* size,
                  const Settings::FontSpec& spec)
{
    if (spec.family.isEmpty() || spec.size <= 0) {
        family->setCurrentIndex(0);  // "(built-in default)"
        size->setEnabled(false);
    } else {
        family->setCurrentText(spec.family);
        size->setEnabled(true);
        size->setValue(spec.size);
    }
}

Settings::FontSpec fontSpecOf(const QFontComboBox* family, const QDoubleSpinBox* size)
{
    const QString name = family->currentText().trimmed();
    if (name.isEmpty() || name == SettingsDialog::tr("(built-in default)")
        || size->value() <= 0) {
        return {};
    }
    Settings::FontSpec spec;
    spec.family = name;
    spec.size = size->value();
    return spec;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

/// RAII over loading_: while active, programmatic widget updates do not mark
/// the layer dirty and selection handlers skip their commits.
struct SettingsDialog::LoadGuard {
    explicit LoadGuard(SettingsDialog* dialog) : dialog(dialog) { ++dialog->loading_; }
    ~LoadGuard() { --dialog->loading_; }
    SettingsDialog* dialog;
};

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Settings"));
    resize(1000, 700);

    auto* rootLayout = new QVBoxLayout(this);

    // --- scope row ----------------------------------------------------------
    auto* scopeRow = new QHBoxLayout;
    scopeRow->addWidget(new QLabel(tr("Edit:"), this));
    scopeCombo_ = new QComboBox(this);
    scopeCombo_->setObjectName(QStringLiteral("settings.scope"));
    scopeCombo_->addItem(tr("Global settings"),
                         static_cast<int>(Scope::Base));
    scopeCombo_->setToolTip(
        tr("The global layer lives in config/ next to the executable; the "
           "project layer overlays it from <project>/config."));
    scopeRow->addWidget(scopeCombo_, 1);
    rootLayout->addLayout(scopeRow);

    // --- navigation + pages ---------------------------------------------------
    auto* bodyRow = new QHBoxLayout;
    nav_ = new QListWidget(this);
    nav_->setObjectName(QStringLiteral("settings.nav"));
    nav_->setFixedWidth(170);
    nav_->addItem(tr("General"));
    nav_->addItem(tr("Toolchains"));
    nav_->addItem(tr("Kits"));
    nav_->setCurrentRow(0);
    bodyRow->addWidget(nav_);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("settings.pages"));
    pages_->addWidget(wrapInScroll(buildGeneralPage()));
    pages_->addWidget(wrapInScroll(buildToolchainsPage()));
    pages_->addWidget(wrapInScroll(buildKitsPage()));
    bodyRow->addWidget(pages_, 1);
    rootLayout->addLayout(bodyRow, 1);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("settings.status"));
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rootLayout->addWidget(statusLabel_);

    // --- buttons ---------------------------------------------------------------
    auto* buttons = new QDialogButtonBox(this);
    buttons->setObjectName(QStringLiteral("settings.buttons"));
    buttons->setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel
                                | QDialogButtonBox::Apply);
    buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("settings.ok"));
    buttons->button(QDialogButtonBox::Cancel)
        ->setObjectName(QStringLiteral("settings.cancel"));
    buttons->button(QDialogButtonBox::Apply)
        ->setObjectName(QStringLiteral("settings.apply"));
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            [this] {
                if (saveModifiedScopes()) {
                    // Written is not applied: the main window re-reads and
                    // re-applies the config on this signal.
                    emit settingsApplied();
                }
            });
    rootLayout->addWidget(buttons);

    // --- wiring -----------------------------------------------------------------
    connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (loading_ > 0 || row < 0) {
            return;
        }
        commitUiToState();  // flush the page the user is leaving
        pages_->setCurrentIndex(row);
        // The kit page's suggestions and inheritance combo read the
        // committed toolchains — refresh them after the page switch.
        updateKitSuggestions();
    });
    connect(scopeCombo_, &QComboBox::currentIndexChanged, this,
            [this](int index) { handleScopeChanged(index); });
}

QWidget* SettingsDialog::wrapInScroll(QWidget* page)
{
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(page);
    return scroll;
}

// ---------------------------------------------------------------------------
// Widget factories — every factory wires markDirty, so edits made anywhere
// in the dialog flag the current layer as modified.
// ---------------------------------------------------------------------------

QLineEdit* SettingsDialog::makeLine(const QString& objectName)
{
    auto* edit = new QLineEdit(this);
    edit->setObjectName(objectName);
    connect(edit, &QLineEdit::textChanged, this, [this] { markDirty(); });
    return edit;
}

QPlainTextEdit* SettingsDialog::makeTextEdit(const QString& objectName, int fixedHeight)
{
    auto* edit = new QPlainTextEdit(this);
    edit->setObjectName(objectName);
    if (fixedHeight > 0) {
        edit->setFixedHeight(fixedHeight);
    }
    connect(edit, &QPlainTextEdit::textChanged, this, [this] { markDirty(); });
    return edit;
}

QComboBox* SettingsDialog::makeCombo(const QString& objectName, bool editable)
{
    auto* combo = new QComboBox(this);
    combo->setObjectName(objectName);
    if (editable) {
        combo->setEditable(true);
        combo->setInsertPolicy(QComboBox::NoInsert);
    }
    connect(combo, &QComboBox::currentTextChanged, this,
            [this](const QString&) { markDirty(); });
    return combo;
}

QCheckBox* SettingsDialog::makeCheck(const QString& objectName, const QString& text)
{
    auto* check = new QCheckBox(text, this);
    check->setObjectName(objectName);
    connect(check, &QCheckBox::toggled, this, [this](bool) { markDirty(); });
    return check;
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

QWidget* SettingsDialog::buildGeneralPage()
{
    auto* page = new QWidget(this);
    auto* column = new QVBoxLayout(page);
    column->addWidget(buildDefaultsSection(page));
    column->addWidget(buildPreferencesSection(page));
    column->addStretch(1);
    return page;
}

QWidget* SettingsDialog::buildDefaultsSection(QWidget* parent)
{
    auto* group = new QGroupBox(tr("Defaults"), parent);
    auto* form = new QFormLayout(group);

    defaultsKitId_ = makeCombo(QStringLiteral("defaults.kitId"), true);
    defaultsKitId_->setToolTip(
        tr("The kit selected on start-up. Kits are defined on the Kits page."));
    form->addRow(tr("Default kit"), defaultsKitId_);

    defaultsBuildConfig_ = makeCombo(QStringLiteral("defaults.buildConfig"), true);
    defaultsBuildConfig_->setToolTip(
        tr("The build configuration (Debug/Release/...) selected on start-up."));
    form->addRow(tr("Default build configuration"), defaultsBuildConfig_);

    const auto addPathRow = [this, form, group](const char* objectName,
                                               QLineEdit** editOut,
                                               const QString& label,
                                               const QString& tip) {
        auto* edit = makeLine(QLatin1String(objectName));
        edit->setToolTip(tip);
        auto* browse = new QPushButton(tr("Browse..."), group);
        connect(browse, &QPushButton::clicked, this, [this, edit] {
            const QString path = QFileDialog::getOpenFileName(
                this, tr("Select a tool"), edit->text(),
                tr("All files (*)"));
            if (!path.isEmpty()) {
                edit->setText(path);
            }
        });
        auto* row = new QHBoxLayout;
        row->addWidget(edit, 1);
        row->addWidget(browse);
        form->addRow(label, row);
        *editOut = edit;
    };
    addPathRow("defaults.gitPath", &defaultsGitPath_, tr("Git path"),
               tr("Shared git, used when the active kit does not define its own."));
    addPathRow("defaults.clangdPath", &defaultsClangdPath_, tr("Clangd path"),
               tr("Shared clangd used for syntax checks and completion."));
    addPathRow("defaults.clangFormatPath", &defaultsClangFormatPath_,
               tr("Clang-Format path"),
               tr("Shared clang-format used by the formatting commands."));
    return group;
}

QWidget* SettingsDialog::buildToolchainsPage()
{
    auto* page = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(page);

    // --- master: the toolchain list -------------------------------------------
    auto* listPane = new QVBoxLayout;
    toolchainList_ = new QListWidget(this);
    toolchainList_->setObjectName(QStringLiteral("toolchains.list"));
    toolchainList_->setMinimumWidth(180);
    toolchainList_->setToolTip(
        tr("Toolchains are stored in toolchains.json of the edited layer. "
           "Kits reference them by id."));
    listPane->addWidget(toolchainList_, 1);
    auto* scanButton = new QPushButton(tr("Find Compilers..."), this);
    scanButton->setObjectName(QStringLiteral("toolchains.identifyFolder"));
    scanButton->setToolTip(
        tr("Pick a folder; it is searched recursively and a new toolchain "
           "is created for every directory that contains a compiler."));
    connect(scanButton, &QPushButton::clicked, this,
            [this] { identifyToolchainFolder(); });
    toolchainScanButton_ = scanButton;
    listPane->addWidget(scanButton);
    auto* toolchainButtons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"), this);
    add->setObjectName(QStringLiteral("toolchains.add"));
    connect(add, &QPushButton::clicked, this, [this] { addToolchain(); });
    toolchainButtons->addWidget(add);
    auto* remove = new QPushButton(tr("Remove"), this);
    remove->setObjectName(QStringLiteral("toolchains.remove"));
    connect(remove, &QPushButton::clicked, this, [this] { removeToolchain(); });
    toolchainButtons->addWidget(remove);
    auto* check = new QPushButton(tr("Check"), this);
    check->setObjectName(QStringLiteral("toolchains.check"));
    check->setToolTip(
        tr("Verify that every named tool exists on disk and report what is "
           "missing."));
    connect(check, &QPushButton::clicked, this,
            [this] { checkCurrentToolchain(); });
    toolchainButtons->addWidget(check);
    listPane->addLayout(toolchainButtons);
    rowLayout->addLayout(listPane);

    // --- detail: the toolchain form ----------------------------------------------
    auto* formBox = new QGroupBox(tr("Toolchain"), this);
    toolchainFormContainer_ = formBox;
    auto* form = new QFormLayout(formBox);

    toolchainId_ = makeLine(QStringLiteral("toolchain.id"));
    form->addRow(tr("Id"), toolchainId_);
    toolchainName_ = makeLine(QStringLiteral("toolchain.name"));
    form->addRow(tr("Name"), toolchainName_);

    // The nine tool rows: a line edit plus a Browse button each, kept in a
    // member map keyed by the spec key.
    for (const ToolchainToolSpec& spec : kToolchainToolSpecs) {
        const QString key = QLatin1String(spec.key);
        auto* edit = makeLine(QStringLiteral("toolchain.%1").arg(key));
        edit->setToolTip(tr("Absolute path to the executable; empty = unset."));
        auto* browse = new QPushButton(tr("Browse..."), formBox);
        connect(browse, &QPushButton::clicked, this, [this, edit] {
            const QString path = QFileDialog::getOpenFileName(
                this, tr("Select a tool"), edit->text(),
                tr("Executables (*.exe);;All files (*)"));
            if (!path.isEmpty()) {
                edit->setText(path);
            }
        });
        auto* row = new QHBoxLayout;
        row->addWidget(edit, 1);
        row->addWidget(browse);
        form->addRow(tr(spec.label), row);
        toolchainToolEdits_.insert(key, edit);
    }

    rowLayout->addWidget(formBox, 1);

    connect(toolchainList_, &QListWidget::currentRowChanged, this,
            [this](int row) {
                if (loading_ > 0) {
                    return;
                }
                commitToolchainForm();
                loadToolchainForm(row);
            });
    return page;
}

QWidget* SettingsDialog::buildKitsPage()
{
    auto* page = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(page);

    // --- master: the kit list ------------------------------------------------
    auto* listPane = new QVBoxLayout;
    kitList_ = new QListWidget(this);
    kitList_->setObjectName(QStringLiteral("kits.list"));
    kitList_->setMinimumWidth(180);
    kitList_->setToolTip(tr("Kits are stored in kits.json of the edited layer."));
    listPane->addWidget(kitList_, 1);
    auto* kitButtons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"), this);
    add->setObjectName(QStringLiteral("kits.add"));
    connect(add, &QPushButton::clicked, this, [this] { addKit(); });
    kitButtons->addWidget(add);
    auto* duplicate = new QPushButton(tr("Duplicate"), this);
    duplicate->setObjectName(QStringLiteral("kits.duplicate"));
    connect(duplicate, &QPushButton::clicked, this, [this] { duplicateKit(); });
    kitButtons->addWidget(duplicate);
    auto* remove = new QPushButton(tr("Remove"), this);
    remove->setObjectName(QStringLiteral("kits.remove"));
    connect(remove, &QPushButton::clicked, this, [this] { removeKit(); });
    kitButtons->addWidget(remove);
    listPane->addLayout(kitButtons);
    rowLayout->addLayout(listPane);

    // --- detail: the kit form --------------------------------------------------
    auto* kitForm = new QGroupBox(tr("Kit"), this);
    kitFormContainer_ = kitForm;
    auto* kitLayout = new QVBoxLayout(kitForm);
    auto* form = new QFormLayout;

    kitId_ = makeLine(QStringLiteral("kit.id"));
    form->addRow(tr("Id"), kitId_);
    kitName_ = makeLine(QStringLiteral("kit.name"));
    form->addRow(tr("Name"), kitName_);

    auto* stateRow = new QHBoxLayout;
    kitEnabled_ = makeCheck(QStringLiteral("kit.enabled"), tr("Enabled"));
    stateRow->addWidget(kitEnabled_);
    kitIsDefault_ = makeCheck(QStringLiteral("kit.isDefault"), tr("Default"));
    kitIsDefault_->setToolTip(
        tr("Fallback default when defaults.defaultKitId does not name an "
           "enabled kit; the first kit marked Default wins."));
    stateRow->addWidget(kitIsDefault_);
    stateRow->addStretch(1);
    form->addRow(stateRow);

    kitInheritToolchain_ =
        makeCheck(QStringLiteral("kit.inheritToolchain"), tr("Inherit toolchain"));
    kitInheritToolchain_->setToolTip(
        tr("Fill this kit's tools from the toolchain selected below. Fields "
           "the kit names itself still win."));
    kitToolchainId_ = makeCombo(QStringLiteral("kit.toolchainId"), false);
    kitToolchainId_->setToolTip(
        tr("The toolchain this kit inherits, as defined on the Toolchains "
           "page."));
    auto* inheritRow = new QHBoxLayout;
    inheritRow->addWidget(kitInheritToolchain_);
    inheritRow->addWidget(kitToolchainId_, 1);
    form->addRow(tr("Toolchain"), inheritRow);
    connect(kitInheritToolchain_, &QCheckBox::toggled, this,
            [this](bool) { updateKitInheritanceUi(); });
    // Switching the referenced toolchain re-resolves the inherited display
    // (only while the kit actually inherits).
    connect(kitToolchainId_, &QComboBox::currentTextChanged, this, [this](const QString&) {
        if (kitInheritToolchain_->isChecked()) {
            updateKitInheritanceUi();
        }
    });

    // compilerFamily decides which toolchain compilers map onto the kit's
    // C/C++ slots when inheriting: gcc -> gcc/g++, llvm -> clang/clang++,
    // msvc -> cl. "else" inherits no compiler at all — the kit names its
    // own.
    kitCompilerFamily_ = new QComboBox(this);
    kitCompilerFamily_->setObjectName(QStringLiteral("kit.compilerFamily"));
    kitCompilerFamily_->addItem(tr("else (own compilers)"), QString());
    kitCompilerFamily_->addItem(QStringLiteral("gcc"), QStringLiteral("gcc"));
    kitCompilerFamily_->addItem(QStringLiteral("llvm"), QStringLiteral("llvm"));
    kitCompilerFamily_->addItem(QStringLiteral("msvc"), QStringLiteral("msvc"));
    kitCompilerFamily_->setToolTip(
        tr("Which compiler driver family this kit builds with. While "
           "inheriting, the matching compilers (gcc/g++, clang/clang++ or "
           "cl) are taken from the toolchain; \"else\" inherits none and "
           "the kit names its own."));
    form->addRow(tr("Compiler family"), kitCompilerFamily_);
    connect(kitCompilerFamily_, &QComboBox::currentIndexChanged, this, [this](int) {
        // This combo is built by hand (its items carry the family as data,
        // so makeCombo() cannot make it), and that meant the one connect it
        // did get was about the inheritance display only. Changing nothing
        // but the compiler family therefore left the layer "unmodified" and
        // Apply skipped writing it — the edit vanished without a word
        // (R2-33). What the user sees is what must be saved.
        markDirty();
        if (kitInheritToolchain_->isChecked()) {
            updateKitInheritanceUi();
        }
    });

    kitCCompiler_ = makeCombo(QStringLiteral("kit.cCompiler"), true);
    form->addRow(tr("C compiler"), kitCCompiler_);
    kitCppCompiler_ = makeCombo(QStringLiteral("kit.cppCompiler"), true);
    form->addRow(tr("C++ compiler"), kitCppCompiler_);
    kitDebugger_ = makeCombo(QStringLiteral("kit.debugger"), true);
    form->addRow(tr("Debugger"), kitDebugger_);
    kitClangd_ = makeCombo(QStringLiteral("kit.clangd"), true);
    form->addRow(tr("Clangd"), kitClangd_);
    kitClangFormat_ = makeCombo(QStringLiteral("kit.clangFormat"), true);
    form->addRow(tr("Clang-Format"), kitClangFormat_);

    auto* toolsBox = new QGroupBox(tr("Build tools"), this);
    kitToolsBox_ = toolsBox;
    auto* toolsForm = new QFormLayout(toolsBox);
    kitToolCmake_ = makeCombo(QStringLiteral("kit.tool.cmake"), true);
    toolsForm->addRow(tr("CMake"), kitToolCmake_);
    kitToolNinja_ = makeCombo(QStringLiteral("kit.tool.ninja"), true);
    toolsForm->addRow(tr("Ninja"), kitToolNinja_);
    kitToolGnuMake_ = makeCombo(QStringLiteral("kit.tool.gnuMake"), true);
    toolsForm->addRow(tr("GNU Make"), kitToolGnuMake_);
    kitToolMsbuild_ = makeCombo(QStringLiteral("kit.tool.msbuild"), true);
    toolsForm->addRow(tr("MSBuild"), kitToolMsbuild_);
    form->addRow(toolsBox);

    kitCppStandard_ = makeCombo(QStringLiteral("kit.cppStandard"), true);
    fillStandardCombo(kitCppStandard_);
    kitCppStandard_->setToolTip(tr("Leave empty to use the compiler's default."));
    form->addRow(tr("C++ standard"), kitCppStandard_);
    kitCompileFlags_ = makeTextEdit(QStringLiteral("kit.compileFlags"), 56);
    kitCompileFlags_->setToolTip(tr("Kit-global compile flags, one per line."));
    form->addRow(tr("Compile flags"), kitCompileFlags_);
    kitLinkFlags_ = makeTextEdit(QStringLiteral("kit.linkFlags"), 56);
    kitLinkFlags_->setToolTip(tr("Kit-global link flags, one per line."));
    form->addRow(tr("Link flags"), kitLinkFlags_);

    kitGitPath_ = makeLine(QStringLiteral("kit.gitPath"));
    kitGitPath_->setToolTip(tr("Kit-specific git; empty uses the shared default."));
    form->addRow(tr("Git path"), kitGitPath_);
    kitGitArgs_ = makeTextEdit(QStringLiteral("kit.gitArgs"), 48);
    kitGitArgs_->setToolTip(tr("Extra git arguments, one per line."));
    form->addRow(tr("Git arguments"), kitGitArgs_);

    kitClangFormatStyle_ = makeCombo(QStringLiteral("kit.clangFormatStyle"), true);
    fillStyleCombo(kitClangFormatStyle_);
    form->addRow(tr("Clang-Format style"), kitClangFormatStyle_);
    kitClangdFallbackStyle_ = makeCombo(QStringLiteral("kit.clangdFallbackStyle"), true);
    fillStyleCombo(kitClangdFallbackStyle_);
    form->addRow(tr("Clangd fallback style"), kitClangdFallbackStyle_);

    kitLayout->addLayout(form);

    // --- build configurations ------------------------------------------------
    configGroup_ = new QGroupBox(tr("Build configurations"), this);
    auto* configRowLayout = new QHBoxLayout(configGroup_);

    auto* configPane = new QVBoxLayout;
    configList_ = new QListWidget(this);
    configList_->setObjectName(QStringLiteral("kit.configs.list"));
    configList_->setMinimumWidth(140);
    configPane->addWidget(configList_, 1);
    auto* configButtons = new QVBoxLayout;
    auto* configAdd = new QPushButton(tr("Add"), this);
    configAdd->setObjectName(QStringLiteral("kit.configs.add"));
    connect(configAdd, &QPushButton::clicked, this,
            [this] { addBuildConfiguration(); });
    configButtons->addWidget(configAdd);
    auto* configRemove = new QPushButton(tr("Remove"), this);
    configRemove->setObjectName(QStringLiteral("kit.configs.remove"));
    connect(configRemove, &QPushButton::clicked, this,
            [this] { removeBuildConfiguration(); });
    configButtons->addWidget(configRemove);
    configButtons->addStretch(1);
    configPane->addLayout(configButtons);
    configRowLayout->addLayout(configPane);

    auto* configFormContainer = new QWidget(this);
    configFormContainer_ = configFormContainer;
    auto* configForm = new QFormLayout(configFormContainer);
    configName_ = makeLine(QStringLiteral("kit.config.name"));
    configForm->addRow(tr("Name"), configName_);
    configInherit_ = makeCheck(QStringLiteral("kit.config.inherit"),
                               tr("Inherit kit flags"));
    configInherit_->setToolTip(
        tr("When unchecked, this configuration replaces the kit's flags "
           "instead of extending them."));
    configForm->addRow(QStringLiteral(), configInherit_);
    configCompileFlags_ = makeTextEdit(QStringLiteral("kit.config.compileFlags"), 56);
    configCompileFlags_->setToolTip(tr("One flag per line."));
    configForm->addRow(tr("Compile flags"), configCompileFlags_);
    configLinkFlags_ = makeTextEdit(QStringLiteral("kit.config.linkFlags"), 48);
    configForm->addRow(tr("Link flags"), configLinkFlags_);
    configCppStandard_ = makeCombo(QStringLiteral("kit.config.cppStandard"), true);
    fillStandardCombo(configCppStandard_);
    configForm->addRow(tr("C++ standard"), configCppStandard_);

    auto* argsBox =
        new QGroupBox(tr("Build tool arguments (used when the project defines none)"),
                      this);
    auto* argsGrid = new QGridLayout(argsBox);
    const auto addArgsEditor = [this, argsBox, argsGrid](const char* name,
                                                         QPlainTextEdit** out,
                                                         const QString& label,
                                                         int row, int column) {
        auto* edit = makeTextEdit(QLatin1String(name), 44);
        edit->setToolTip(tr("One argument per line."));
        argsGrid->addWidget(new QLabel(label, argsBox), row * 2, column);
        argsGrid->addWidget(edit, row * 2 + 1, column);
        *out = edit;
    };
    addArgsEditor("kit.config.args.cmake", &configArgsCmake_, tr("CMake"), 0, 0);
    addArgsEditor("kit.config.args.ninja", &configArgsNinja_, tr("Ninja"), 0, 1);
    addArgsEditor("kit.config.args.gnuMake", &configArgsGnuMake_, tr("GNU Make"), 1, 0);
    addArgsEditor("kit.config.args.msbuild", &configArgsMsbuild_, tr("MSBuild"), 1, 1);
    configForm->addRow(argsBox);

    configRowLayout->addWidget(configFormContainer, 1);
    kitLayout->addWidget(configGroup_);

    rowLayout->addWidget(kitForm, 1);

    connect(kitList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (loading_ > 0) {
            return;
        }
        commitKitForm();
        loadKitForm(row);
    });
    connect(configList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (loading_ > 0) {
            return;
        }
        commitConfigForm();
        loadConfigForm(row);
    });
    return page;
}

QWidget* SettingsDialog::buildPreferencesSection(QWidget* parent)
{
    auto* container = new QWidget(parent);
    auto* column = new QVBoxLayout(container);
    column->setContentsMargins(0, 0, 0, 0);

    // --- fonts -----------------------------------------------------------------
    auto* fonts = new QGroupBox(tr("Fonts"), this);
    auto* fontsForm = new QFormLayout(fonts);
    const auto addFontRow = [this, fontsForm](const char* familyName,
                                              const char* sizeName,
                                              const QString& label,
                                              QFontComboBox** familyOut,
                                              QDoubleSpinBox** sizeOut) {
        auto* family = new QFontComboBox(this);
        family->setObjectName(QLatin1String(familyName));
        family->insertItem(0, tr("(built-in default)"));
        auto* size = new QDoubleSpinBox(this);
        size->setObjectName(QLatin1String(sizeName));
        size->setRange(1.0, 72.0);
        size->setSingleStep(0.5);
        size->setDecimals(1);
        size->setSuffix(QStringLiteral(" pt"));
        size->setEnabled(false);
        connect(family, &QComboBox::currentTextChanged, this,
                [this, size](const QString& text) {
                    size->setEnabled(!text.isEmpty()
                                     && text != tr("(built-in default)"));
                    markDirty();
                });
        connect(size, &QDoubleSpinBox::valueChanged, this,
                [this](double) { markDirty(); });
        auto* row = new QHBoxLayout;
        row->addWidget(family, 1);
        row->addWidget(size);
        fontsForm->addRow(label, row);
        *familyOut = family;
        *sizeOut = size;
    };
    addFontRow("prefs.toolbar.family", "prefs.toolbar.size", tr("Toolbar"),
               &toolbarFontFamily_, &toolbarFontSize_);
    addFontRow("prefs.editor.family", "prefs.editor.size", tr("Editor"),
               &editorFontFamily_, &editorFontSize_);
    addFontRow("prefs.output.family", "prefs.output.size", tr("Output"),
               &outputFontFamily_, &outputFontSize_);
    column->addWidget(fonts);

    // --- files -------------------------------------------------------------------
    auto* files = new QGroupBox(tr("Files"), this);
    auto* filesForm = new QFormLayout(files);
    encodingDefault_ = makeCombo(QStringLiteral("prefs.encoding.default"), true);
    for (const char* name : kEncodingNames) {
        encodingDefault_->addItem(QLatin1String(name));
    }
    filesForm->addRow(tr("New-file encoding"), encodingDefault_);
    encodingPriority_ = makeTextEdit(QStringLiteral("prefs.encoding.priority"), 64);
    encodingPriority_->setToolTip(
        tr("Detection order, one name per line (modelled; the detector's "
           "actual order is fixed).\nKnown names: UTF-8, GB18030, GBK, Big5, "
           "Shift-JIS, EUC-KR, UTF-16LE, UTF-16BE, Windows-1252, ISO-8859-1"));
    filesForm->addRow(tr("Detection priority"), encodingPriority_);
    lineEndingDefault_ = makeCombo(QStringLiteral("prefs.lineEnding.default"), false);
    lineEndingDefault_->addItems(
        {QStringLiteral("auto"), QStringLiteral("LF"), QStringLiteral("CRLF")});
    filesForm->addRow(tr("Line ending"), lineEndingDefault_);
    longLineMode_ = makeCombo(QStringLiteral("prefs.longLineMode"), false);
    longLineMode_->addItems(
        {QStringLiteral("scroll"), QStringLiteral("wrap")});
    longLineMode_->setToolTip(
        tr("What to do with a line longer than the editor is wide.\n"
           "scroll — keep one line per row and scroll it sideways\n"
           "wrap   — break it at the edge of the editing area"));
    filesForm->addRow(tr("Line longer than the editor"), longLineMode_);
    showWhitespaces_ =
        makeCheck(QStringLiteral("prefs.files.showWhitespaces"),
                  tr("Show whitespace characters"));
    showWhitespaces_->setToolTip(
        tr("Draw spaces as middle dots and tabs as arrows in the editor."));
    filesForm->addRow(QStringLiteral(), showWhitespaces_);
    showIndentGuides_ =
        makeCheck(QStringLiteral("prefs.files.showIndentGuides"),
                  tr("Show indent guides"));
    showIndentGuides_->setToolTip(
        tr("Draw a muted vertical line at every indent stop of a line's "
           "leading whitespace."));
    filesForm->addRow(QStringLiteral(), showIndentGuides_);
    column->addWidget(files);

    // --- general -----------------------------------------------------------------
    auto* general = new QGroupBox(tr("General"), this);
    auto* generalForm = new QFormLayout(general);
    checkCompilersAtStartup_ =
        makeCheck(QStringLiteral("prefs.general.checkCompilersAtStartup"),
                  tr("Check that the compilers exist at start-up"));
    checkCompilersAtStartup_->setToolTip(
        tr("When enabled, start-up verifies the default kit's compilers and "
           "offers to open the settings when one is missing."));
    generalForm->addRow(QStringLiteral(), checkCompilersAtStartup_);
    showStartupPage_ =
        makeCheck(QStringLiteral("prefs.general.showStartupPage"),
                  tr("Show the start-up page at launch"));
    showStartupPage_->setToolTip(
        tr("The start page replaces the auto-opened Untitled tab at "
           "launch: it lists recently opened files and projects and the "
           "four ways to begin. When off, an empty Untitled editor opens."));
    generalForm->addRow(QStringLiteral(), showStartupPage_);
    restoreSession_ =
        makeCheck(QStringLiteral("prefs.general.restoreSession"),
                  tr("Reopen the previous session's files at launch"));
    restoreSession_->setToolTip(
        tr("When enabled, the files open at exit are recorded and reopened "
           "the next time LightC++ starts, taking the place of the start "
           "page. Unsaved (untitled) buffers are not restored."));
    generalForm->addRow(QStringLiteral(), restoreSession_);
    checkToolchainsAtStartup_ =
        makeCheck(QStringLiteral("prefs.general.checkToolchainsAtStartup"),
                  tr("Check toolchains and kits at start-up"));
    checkToolchainsAtStartup_->setToolTip(
        tr("When enabled, the start-up page verifies every toolchain and "
           "enabled kit when it opens (compiler files exist, kit "
           "toolchain references resolve) and shows the progress inline."));
    generalForm->addRow(QStringLiteral(), checkToolchainsAtStartup_);
    autoSave_ =
        makeCheck(QStringLiteral("prefs.general.autoSave"),
                  tr("Auto-save modified files"));
    autoSave_->setToolTip(
        tr("Periodically write every modified buffer that already has a "
           "file name back to disk. Untitled buffers are not touched."));
    generalForm->addRow(QStringLiteral(), autoSave_);
    autoSaveSeconds_ = new QSpinBox(this);
    autoSaveSeconds_->setRange(5, 3600);
    autoSaveSeconds_->setSuffix(QStringLiteral(" s"));
    autoSaveSeconds_->setValue(30);
    connect(autoSaveSeconds_, &QSpinBox::valueChanged, this,
            [this](int) { markDirty(); });
    autoSave_->setToolTip(autoSave_->toolTip()
                          + tr("\nThe save period in seconds."));
    generalForm->addRow(tr("Auto-save period"), autoSaveSeconds_);
    runOutputMode_ = makeCombo(QStringLiteral("prefs.run.output"), false);
    runOutputMode_->addItem(tr("Program Output"), QStringLiteral("program"));
#ifdef Q_OS_WIN
    runOutputMode_->addItem(tr("Console Pauser"), QStringLiteral("consolePauser"));
#endif
    QString runModeTip =
        tr("How the built program runs.\n"
           "Program Output: its output is captured in the IDE's Program "
           "Output channel, with an input field for stdin.\n");
#ifdef Q_OS_WIN
    runModeTip +=
        tr("Console Pauser: it runs in its own console window via "
           "ConsolePauser.exe (copied next to the program when missing).\n");
#endif
    runModeTip +=
        tr("The other entries come from the editable terminal list below; "
           "an embedded terminal captures the output in the Program Output "
           "channel, while an external terminal is launched as a separate "
           "process.");
    runOutputMode_->setToolTip(runModeTip);
    generalForm->addRow(tr("Program output"), runOutputMode_);

    // The editable terminal list: Program Output and Console Pauser are
    // fixed, everything here can be renamed, edited, added or removed.
    auto* terminalsRow = new QHBoxLayout;
    runTerminals_ = new QTableWidget(0, 3, this);
    runTerminals_->setObjectName(QStringLiteral("prefs.run.terminals"));
    runTerminals_->setHorizontalHeaderLabels(
        {tr("Name"), tr("Command + program path"), tr("Embedded")});
    runTerminals_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    runTerminals_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    runTerminals_->verticalHeader()->setVisible(false);
    runTerminals_->setSelectionBehavior(QAbstractItemView::SelectRows);
    runTerminals_->setSelectionMode(QAbstractItemView::SingleSelection);
    runTerminals_->setMinimumHeight(96);
    runTerminals_->setToolTip(
        tr("Terminals the program can run in. The command starts the shell "
           "or terminal emulator. Embedded terminals run in the Program "
           "Output channel: the program's path is typed in as the first "
           "line of input and the input field stays live for stdin. "
           "Non-embedded terminals are launched as separate processes."));
    connect(runTerminals_, &QTableWidget::itemChanged,
            this, [this](QTableWidgetItem*) {
        if (runTerminalsLoaded_) {
            rebuildRunTerminalCombo();
            markDirty();
        }
    });
    terminalsRow->addWidget(runTerminals_, 1);
    auto* terminalButtons = new QVBoxLayout;
    auto* addTerminal = new QPushButton(tr("Add"), this);
    addTerminal->setObjectName(QStringLiteral("prefs.run.terminals.add"));
    connect(addTerminal, &QPushButton::clicked, this, [this] {
        // A new row gets a generated id (kept in the name item's data) so
        // the selection can reference it after the settings are saved.
        const QString id = QStringLiteral("terminal-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const int row = runTerminals_->rowCount();
        runTerminals_->insertRow(row);
        auto* nameItem = new QTableWidgetItem(tr("New terminal"));
        nameItem->setData(Qt::UserRole, id);
        runTerminals_->setItem(row, 0, nameItem);
        runTerminals_->setItem(row, 1, new QTableWidgetItem(QString()));
        auto* embeddedItem = new QTableWidgetItem();
        embeddedItem->setCheckState(Qt::Checked);
        embeddedItem->setFlags(embeddedItem->flags()
                               | Qt::ItemIsUserCheckable);
        runTerminals_->setItem(row, 2, embeddedItem);
        runTerminals_->editItem(nameItem);
    });
    auto* removeTerminal = new QPushButton(tr("Remove"), this);
    removeTerminal->setObjectName(QStringLiteral("prefs.run.terminals.remove"));
    removeTerminal->setToolTip(tr("Delete the selected terminal"));
    connect(removeTerminal, &QPushButton::clicked, this, [this] {
        const int row = runTerminals_->currentRow();
        if (row < 0) {
            return;
        }
        // removeRow() emits no itemChanged, so the one handler that knows
        // about user edits never runs: the layer stayed "unmodified", Apply
        // skipped it, and the deleted terminal came back — while the "run
        // in" list kept offering it too (R2-33). Deleting a row is an edit
        // like any other.
        runTerminals_->removeRow(row);
        if (runTerminalsLoaded_) {
            runTerminals_->setCurrentCell(-1, -1);
            rebuildRunTerminalCombo();
            markDirty();
        }
    });
    terminalButtons->addWidget(addTerminal);
    terminalButtons->addWidget(removeTerminal);
    terminalButtons->addStretch(1);
    terminalsRow->addLayout(terminalButtons);
    generalForm->addRow(tr("Terminals"), terminalsRow);
    column->addWidget(general);

    // --- shortcuts ----------------------------------------------------------------
    auto* shortcuts = new QGroupBox(tr("Shortcuts"), this);
    auto* shortcutsForm = new QFormLayout(shortcuts);
    shortcutClangd_ = new QKeySequenceEdit(this);
    shortcutClangd_->setObjectName(QStringLiteral("prefs.shortcut.clangd"));
    shortcutClangd_->setToolTip(
        tr("Toggles Tools > Syntax Check. Clear to disable the shortcut."));
    connect(shortcutClangd_, &QKeySequenceEdit::keySequenceChanged, this,
            [this](const QKeySequence&) { markDirty(); });
    shortcutsForm->addRow(tr("Syntax Check (clangd)"), shortcutClangd_);
    shortcutClangFormat_ = new QKeySequenceEdit(this);
    shortcutClangFormat_->setObjectName(QStringLiteral("prefs.shortcut.clangFormat"));
    shortcutClangFormat_->setToolTip(
        tr("Format Document. Clear to keep the built-in shortcuts."));
    connect(shortcutClangFormat_, &QKeySequenceEdit::keySequenceChanged, this,
            [this](const QKeySequence&) { markDirty(); });
    shortcutsForm->addRow(tr("Format Document (clang-format)"), shortcutClangFormat_);
    column->addWidget(shortcuts);

    // --- performance -----------------------------------------------------------------
    auto* performance = new QGroupBox(tr("Large-file limits"), this);
    auto* perfForm = new QFormLayout(performance);
    const auto addMbSpin = [this, perfForm](const char* name, const QString& label,
                                            const QString& tip) {
        auto* spin = new QSpinBox(this);
        spin->setObjectName(QLatin1String(name));
        spin->setRange(0, 4096);
        spin->setSuffix(QStringLiteral(" MB"));
        spin->setSpecialValueText(tr("no limit"));
        spin->setToolTip(tip);
        connect(spin, &QSpinBox::valueChanged, this, [this](int) { markDirty(); });
        perfForm->addRow(label, spin);
        return spin;
    };
    largeFileHighlight_ = addMbSpin(
        "prefs.largeFile.highlight", tr("Disable syntax highlighting above"),
        tr("Files larger than this get no syntax highlighting. 0 = no limit."));
    largeFileIndex_ = addMbSpin(
        "prefs.largeFile.index", tr("Disable clangd indexing above"),
        tr("Files larger than this are not pushed to clangd's index. 0 = no limit."));
    largeFileParse_ = addMbSpin(
        "prefs.largeFile.parse", tr("Disable clangd parsing above"),
        tr("Files larger than this are not parsed by clangd. 0 = no limit."));
    column->addWidget(performance);

    // --- language services --------------------------------------------------------------
    auto* service = new QGroupBox(tr("Language services"), this);
    auto* serviceLayout = new QVBoxLayout(service);
    clangdEnabledByDefault_ = makeCheck(QStringLiteral("prefs.clangd.enabledByDefault"),
                                        tr("Enable clangd by default for C/C++ files"));
    serviceLayout->addWidget(clangdEnabledByDefault_);
    clangdCompletion_ = makeCheck(QStringLiteral("prefs.clangd.completion"),
                                  tr("Completion"));
    serviceLayout->addWidget(clangdCompletion_);
    clangdDiagnostics_ = makeCheck(QStringLiteral("prefs.clangd.diagnostics"),
                                   tr("Diagnostics"));
    serviceLayout->addWidget(clangdDiagnostics_);
    clangdIndexing_ = makeCheck(QStringLiteral("prefs.clangd.indexing"),
                                tr("Navigation (go to definition)"));
    serviceLayout->addWidget(clangdIndexing_);
    auto* clangdStyleForm = new QFormLayout;
    clangdFallbackStyle_ = makeCombo(QStringLiteral("prefs.clangd.fallbackStyle"), true);
    fillStyleCombo(clangdFallbackStyle_);
    clangdStyleForm->addRow(tr("Clangd fallback style"), clangdFallbackStyle_);
    serviceLayout->addLayout(clangdStyleForm);

    // --- code-completion engine -------------------------------------------------
    auto* completionForm = new QFormLayout;
    completionBackend_ = makeCombo(QStringLiteral("prefs.completion.backend"), false);
    completionBackend_->addItem(QStringLiteral("clangd"));
    completionBackend_->addItem(QStringLiteral("libclang"));
    completionBackend_->addItem(QStringLiteral("ccls"));
    completionBackend_->setToolTip(
        tr("Engine that answers code-completion requests.\n"
           "clangd / ccls are language servers; libclang parses the file\n"
           "in-process. When the chosen engine cannot answer, the request\n"
           "is retried the number of times set below."));
    completionForm->addRow(tr("Default completion backend"), completionBackend_);
    completionRetry_ = new QSpinBox(this);
    completionRetry_->setObjectName(QStringLiteral("prefs.completion.retryCount"));
    completionRetry_->setRange(0, 20);
    completionRetry_->setSpecialValueText(tr("no retry"));
    completionRetry_->setToolTip(
        tr("How many times a failed completion request is retried\n"
           "(server missing, crashed, or returned nothing)."));
    connect(completionRetry_, &QSpinBox::valueChanged, this,
            [this](int) { markDirty(); });
    completionForm->addRow(tr("Completion retry count"), completionRetry_);
    serviceLayout->addLayout(completionForm);

    clangFormatEnabled_ = makeCheck(QStringLiteral("prefs.clangFormat.enabled"),
                                    tr("Enable clang-format"));
    serviceLayout->addWidget(clangFormatEnabled_);
    clangFormatOnSave_ = makeCheck(QStringLiteral("prefs.clangFormat.formatOnSave"),
                                   tr("Format on save"));
    serviceLayout->addWidget(clangFormatOnSave_);
    auto* formatStyleForm = new QFormLayout;
    clangFormatStyle_ = makeCombo(QStringLiteral("prefs.clangFormat.style"), true);
    fillStyleCombo(clangFormatStyle_);
    formatStyleForm->addRow(tr("Clang-Format style"), clangFormatStyle_);
    serviceLayout->addLayout(formatStyleForm);
    column->addWidget(service);

    // --- formatting -----------------------------------------------------------------------
    auto* formatting = new QGroupBox(tr("Formatting"), this);
    auto* formattingForm = new QFormLayout(formatting);
    formattingGlobalStyle_ = makeCombo(QStringLiteral("prefs.formatting.globalStyle"), true);
    fillStyleCombo(formattingGlobalStyle_);
    formattingForm->addRow(tr("Global style"), formattingGlobalStyle_);
    formattingDefaultStyleContent_ =
        makeTextEdit(QStringLiteral("prefs.formatting.defaultStyleContent"), 72);
    formattingDefaultStyleContent_->setToolTip(
        tr("Content for a generated .clang-format (reserved; not consumed yet)."));
    formattingForm->addRow(tr("Default style content"), formattingDefaultStyleContent_);
    column->addWidget(formatting);

    return container;
}

// ---------------------------------------------------------------------------
// Scope / layers
// ---------------------------------------------------------------------------

const QString& SettingsDialog::directoryFor(Scope scope) const
{
    return scope == Scope::Base ? baseDir_ : overlayDir_;
}

void SettingsDialog::setBaseDirectory(const QString& dir)
{
    baseDir_ = dir;
    state(Scope::Base) = ScopeState{};
    scopeCombo_->setItemData(0, QDir::toNativeSeparators(dir), Qt::ToolTipRole);
    if (currentScope_ == Scope::Base) {
        loadScopeToUi(Scope::Base);
    }
}

void SettingsDialog::setOverlayDirectory(const QString& dir)
{
    overlayDir_ = dir;
    state(Scope::Overlay) = ScopeState{};
    const int overlayIndex =
        scopeCombo_->findData(static_cast<int>(Scope::Overlay));
    if (dir.isEmpty()) {
        if (overlayIndex >= 0) {
            QSignalBlocker blocker(scopeCombo_);
            scopeCombo_->removeItem(overlayIndex);
        }
        if (currentScope_ == Scope::Overlay) {
            currentScope_ = Scope::Base;
            loadScopeToUi(Scope::Base);
        }
        return;
    }
    if (overlayIndex < 0) {
        scopeCombo_->addItem(tr("Project settings"), static_cast<int>(Scope::Overlay));
        scopeCombo_->setItemData(scopeCombo_->count() - 1,
                                 QDir::toNativeSeparators(dir), Qt::ToolTipRole);
    } else {
        scopeCombo_->setItemData(overlayIndex, QDir::toNativeSeparators(dir),
                                 Qt::ToolTipRole);
    }
    if (currentScope_ == Scope::Overlay) {
        loadScopeToUi(Scope::Overlay);
    }
}

void SettingsDialog::setInitialScope(Scope scope)
{
    const int index = scopeCombo_->findData(static_cast<int>(scope));
    if (index < 0) {
        return;
    }
    if (scopeCombo_->currentIndex() == index) {
        loadScopeToUi(scope);
        return;
    }
    scopeCombo_->setCurrentIndex(index);  // triggers handleScopeChanged
}

void SettingsDialog::loadScopeStateFromDisk(Scope scope)
{
    ScopeState& target = state(scope);
    target = ScopeState{};
    const QString dir = directoryFor(scope);
    if (dir.isEmpty()) {
        return;
    }
    QStringList warnings;
    for (const char* name : kConfigFiles) {
        QFile file(QDir(dir).filePath(QLatin1String(name)));
        if (!file.open(QIODevice::ReadOnly)) {
            continue;  // a missing file is the default configuration
        }
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || document.isNull()) {
            warnings << tr("%1: %2").arg(QLatin1String(name), parseError.errorString());
            continue;
        }
        const auto absorb = [&target](const QJsonObject& object) {
            const QString type = object.value(QLatin1String("type")).toString();
            if (type == QLatin1String("toolchain")) {
                Settings::Toolchain toolchain;
                Settings::fromJson(object, toolchain);
                target.toolchains.append(toolchain);
            } else if (type == QLatin1String("defaults")) {
                Settings::fromJson(object, target.defaults);
            } else if (type == QLatin1String("preferences")) {
                Settings::fromJson(object, target.preferences);
            } else if (type == QLatin1String("kit")) {
                Settings::Kit kit;
                Settings::fromJson(object, kit);
                target.kits.append(kit);
            }
        };
        if (document.isArray()) {
            const QJsonArray array = document.array();
            for (const QJsonValue& value : array) {
                if (value.isObject()) {
                    absorb(value.toObject());
                }
            }
        } else if (document.isObject()) {
            absorb(document.object());
        }
    }
    // Remember what this layer's own documents said, before the user has
    // touched anything: saving compares against it to tell an edited field
    // from one that merely still holds its struct default.
    target.loadedDefaults = LightCpp::Settings::toJson(target.defaults);
    target.loadedPreferences = LightCpp::Settings::toJson(target.preferences);
    if (!warnings.isEmpty()) {
        setStatus(tr("Config warnings: %1").arg(warnings.join(QLatin1String("; "))));
    }
}

void SettingsDialog::loadScopeToUi(Scope scope)
{
    LoadGuard guard(this);
    ScopeState& target = state(scope);
    if (!target.loaded) {
        loadScopeStateFromDisk(scope);
        target.loaded = true;
    }
    currentScope_ = scope;
    currentKitIndex_ = -1;
    currentConfigIndex_ = -1;
    currentToolchainIndex_ = -1;

    updateKitSuggestions();
    loadDefaultsPage(target.defaults);
    loadToolchainsPage(target.toolchains);
    loadPreferencesPage(target.preferences);

    kitList_->blockSignals(true);
    kitList_->clear();
    for (const Settings::Kit& kit : target.kits) {
        kitList_->addItem(kitDisplayName(kit));
    }
    kitList_->blockSignals(false);
    loadKitForm(target.kits.isEmpty() ? -1 : 0);

    // Show the first page; the nav handler is skipped via blockSignals
    // because a programmatic row change must not commit.
    nav_->blockSignals(true);
    nav_->setCurrentRow(0);
    nav_->blockSignals(false);
    pages_->setCurrentIndex(0);
}

void SettingsDialog::handleScopeChanged(int index)
{
    if (loading_ > 0 || index < 0) {
        return;
    }
    const auto target = static_cast<Scope>(scopeCombo_->itemData(index).toInt());
    if (target == currentScope_) {
        return;
    }
    commitUiToState();
    ScopeState& current = state(currentScope_);
    if (current.modified) {
        const QMessageBox::StandardButton answer = QMessageBox::question(
            this, tr("Unsaved changes"),
            tr("Save changes in %1 before switching?")
                .arg(scopeDisplayName(currentScope_)),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
            QMessageBox::Save);
        if (answer == QMessageBox::Cancel) {
            restoreScopeCombo();
            return;
        }
        if (answer == QMessageBox::Save) {
            QString error;
            if (!saveScope(currentScope_, &error)) {
                QMessageBox::critical(this, tr("Settings"), error);
                restoreScopeCombo();
                return;
            }
        } else {
            current.loaded = false;  // discard edits: re-read from disk later
            // The edits are being thrown away: without clearing the flag,
            // OK/Apply would later write the in-memory state — the very
            // edits the user just declined to save — back to disk.
            current.modified = false;
        }
    }
    loadScopeToUi(target);
}

void SettingsDialog::restoreScopeCombo()
{
    QSignalBlocker blocker(scopeCombo_);
    const int index = currentScope_ == Scope::Base
        ? 0
        : scopeCombo_->findData(static_cast<int>(Scope::Overlay));
    if (index >= 0) {
        scopeCombo_->setCurrentIndex(index);
    }
}

// ---------------------------------------------------------------------------
// Defaults page
// ---------------------------------------------------------------------------

void SettingsDialog::loadDefaultsPage(const Settings::GlobalDefaults& defaults)
{
    LoadGuard guard(this);
    updateDefaultsKitItems();
    defaultsKitId_->setCurrentText(defaults.defaultKitId);
    defaultsBuildConfig_->clear();
    defaultsBuildConfig_->addItem(QStringLiteral("Debug"));
    defaultsBuildConfig_->addItem(QStringLiteral("Release"));
    for (const Settings::Kit& kit : kits()) {
        if (kit.id != defaults.defaultKitId) {
            continue;
        }
        for (const Settings::KitBuildConfiguration& config : kit.buildConfigurations) {
            if (!config.name.isEmpty() && defaultsBuildConfig_->findText(config.name) < 0) {
                defaultsBuildConfig_->addItem(config.name);
            }
        }
    }
    defaultsBuildConfig_->setCurrentText(defaults.defaultBuildConfiguration);
    defaultsGitPath_->setText(defaults.gitPath);
    defaultsClangdPath_->setText(defaults.clangdPath);
    defaultsClangFormatPath_->setText(defaults.clangFormatPath);
}

void SettingsDialog::commitDefaultsPage(Settings::GlobalDefaults* defaults)
{
    defaults->defaultKitId = defaultsKitId_->currentText().trimmed();
    defaults->defaultBuildConfiguration = defaultsBuildConfig_->currentText().trimmed();
    defaults->gitPath = defaultsGitPath_->text().trimmed();
    defaults->clangdPath = defaultsClangdPath_->text().trimmed();
    defaults->clangFormatPath = defaultsClangFormatPath_->text().trimmed();
}

void SettingsDialog::updateDefaultsKitItems()
{
    const QString current = defaultsKitId_->currentText();
    QSignalBlocker blocker(defaultsKitId_);
    defaultsKitId_->clear();
    for (const Settings::Kit& kit : kits()) {
        if (!kit.id.isEmpty()) {
            defaultsKitId_->addItem(kit.id);
        }
    }
    defaultsKitId_->setCurrentText(current);
}

// ---------------------------------------------------------------------------
// Toolchains page — one entry per toolchain, master-detail like the kits
// ---------------------------------------------------------------------------

void SettingsDialog::loadToolchainsPage(
    const QList<Settings::Toolchain>& toolchains)
{
    LoadGuard guard(this);
    currentToolchainIndex_ = -1;
    toolchainList_->blockSignals(true);
    toolchainList_->clear();
    for (const Settings::Toolchain& toolchain : toolchains) {
        toolchainList_->addItem(toolchainDisplayName(toolchain));
    }
    toolchainList_->blockSignals(false);
    loadToolchainForm(toolchains.isEmpty() ? -1 : 0);
}

void SettingsDialog::loadToolchainForm(int toolchainIndex)
{
    LoadGuard guard(this);
    currentToolchainIndex_ = toolchainIndex;
    const QList<Settings::Toolchain>& list =
        state(currentScope_).toolchains;
    const bool have =
        toolchainIndex >= 0 && toolchainIndex < list.size();
    toolchainFormContainer_->setEnabled(have);
    if (!have) {
        toolchainId_->clear();
        toolchainName_->clear();
        for (QLineEdit* edit : toolchainToolEdits_) {
            edit->clear();
        }
        return;
    }
    const Settings::Toolchain& toolchain = list[toolchainIndex];
    toolchainId_->setText(toolchain.id);
    toolchainName_->setText(toolchain.name);
    for (auto it = toolchainToolEdits_.constBegin();
         it != toolchainToolEdits_.constEnd(); ++it) {
        it.value()->setText(toolchainPath(toolchain, it.key()));
    }
}

void SettingsDialog::commitToolchainForm()
{
    QList<Settings::Toolchain>& list = state(currentScope_).toolchains;
    if (currentToolchainIndex_ < 0
        || currentToolchainIndex_ >= list.size()) {
        return;
    }
    Settings::Toolchain& toolchain = list[currentToolchainIndex_];
    toolchain.id = toolchainId_->text().trimmed();
    toolchain.name = toolchainName_->text().trimmed();
    for (auto it = toolchainToolEdits_.constBegin();
         it != toolchainToolEdits_.constEnd(); ++it) {
        setToolchainPath(&toolchain, it.key(),
                         it.value()->text().trimmed());
    }
    if (currentToolchainIndex_ < toolchainList_->count()) {
        toolchainList_->item(currentToolchainIndex_)
            ->setText(toolchainDisplayName(toolchain));
    }
}

void SettingsDialog::commitToolchainsPage(
    QList<Settings::Toolchain>* toolchains)
{
    // The form commits straight into the working state the pointer refers
    // to; the parameter exists so callers can name the target explicitly.
    Q_UNUSED(toolchains);
    commitToolchainForm();
}

void SettingsDialog::rebuildToolchainList(int selectRow)
{
    toolchainList_->blockSignals(true);
    toolchainList_->clear();
    for (const Settings::Toolchain& toolchain :
         state(currentScope_).toolchains) {
        toolchainList_->addItem(toolchainDisplayName(toolchain));
    }
    if (selectRow >= 0 && selectRow < toolchainList_->count()) {
        toolchainList_->setCurrentRow(selectRow);
    }
    toolchainList_->blockSignals(false);
}

void SettingsDialog::addToolchain()
{
    commitToolchainForm();
    Settings::Toolchain toolchain;
    toolchain.id = uniqueToolchainId(QStringLiteral("new-toolchain"));
    toolchain.name = tr("New Toolchain");
    state(currentScope_).toolchains.append(toolchain);
    markDirty();
    const int row = state(currentScope_).toolchains.size() - 1;
    rebuildToolchainList(row);
    loadToolchainForm(row);
    updateKitSuggestions();
    toolchainId_->setFocus();
    toolchainId_->selectAll();
}

void SettingsDialog::removeToolchain()
{
    QList<Settings::Toolchain>& list = state(currentScope_).toolchains;
    if (currentToolchainIndex_ < 0 || currentToolchainIndex_ >= list.size()) {
        return;
    }
    const Settings::Toolchain& toolchain = list[currentToolchainIndex_];
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("Remove toolchain"),
        tr("Remove toolchain \"%1\"?").arg(toolchainDisplayName(toolchain)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    list.removeAt(currentToolchainIndex_);
    markDirty();
    const bool any = !list.isEmpty();
    const int row =
        any ? qBound(0, currentToolchainIndex_, static_cast<int>(list.size()) - 1)
            : -1;
    currentToolchainIndex_ = -1;
    rebuildToolchainList(any ? row : -1);
    loadToolchainForm(row);
    updateKitSuggestions();
}

void SettingsDialog::checkCurrentToolchain()
{
    commitToolchainForm();
    const QList<Settings::Toolchain>& list = state(currentScope_).toolchains;
    if (currentToolchainIndex_ < 0 || currentToolchainIndex_ >= list.size()) {
        return;
    }
    const Settings::Toolchain& toolchain = list[currentToolchainIndex_];
    QStringList missing;
    for (const ToolchainToolSpec& spec : kToolchainToolSpecs) {
        const QString path =
            toolchainPath(toolchain, QLatin1String(spec.key));
        if (!path.isEmpty() && !QFileInfo::exists(path)) {
            missing.append(tr("%1: %2")
                               .arg(tr(spec.label),
                                    QDir::toNativeSeparators(path)));
        }
    }
    if (missing.isEmpty()) {
        QMessageBox::information(
            this, tr("Check toolchain"),
            tr("Every tool named by \"%1\" exists on disk.")
                .arg(toolchainDisplayName(toolchain)));
        return;
    }
    QMessageBox::warning(this, tr("Check toolchain"),
                         tr("Missing tools in \"%1\":\n%2")
                             .arg(toolchainDisplayName(toolchain),
                                  missing.join(QLatin1Char('\n'))));
}

void SettingsDialog::identifyToolchainFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Find compilers in a folder"), QString());
    if (dir.isEmpty()) {
        return;
    }

    // Scan options first: how deep to look and which folder names never
    // to enter. Both defaults are read from ToolchainScanOptions itself
    // rather than copied: the depth copy is what made the dialog offer 5
    // while the scan used something else (R2-34).
    QDialog optionsDialog(this);
    optionsDialog.setObjectName(QStringLiteral("toolchains.scanOptions"));
    optionsDialog.setWindowTitle(tr("Scan options"));
    auto* depthLabel = new QLabel(tr("Recursion depth:"), &optionsDialog);
    auto* depth = new QSpinBox(&optionsDialog);
    depth->setObjectName(QStringLiteral("toolchains.scanDepth"));
    depth->setRange(1, 32);
    // The scan's own default, not a second copy of the number: the two used
    // to disagree (5 here, and the Visual Studio layout needs more).
    depth->setValue(Settings::ToolchainScanOptions{}.maxDepth);
    auto* excludedLabel =
        new QLabel(tr("Excluded folder names (comma separated):"),
                   &optionsDialog);
    auto* excluded = new QLineEdit(&optionsDialog);
    excluded->setObjectName(QStringLiteral("toolchains.scanExcluded"));
    excluded->setText(
        Settings::ToolchainScanOptions{}.excludedDirectories.join(
            QStringLiteral(", ")));
    excluded->setToolTip(
        tr("The scan never enters folders with these names, at any depth."));
    auto* buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                             &optionsDialog);
    connect(buttons, &QDialogButtonBox::accepted, &optionsDialog,
            &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &optionsDialog,
            &QDialog::reject);
    auto* form = new QFormLayout;
    form->addRow(depthLabel, depth);
    form->addRow(excludedLabel, excluded);
    auto* layout = new QVBoxLayout(&optionsDialog);
    layout->addLayout(form);
    layout->addWidget(buttons);

    if (optionsDialog.exec() != QDialog::Accepted) {
        return;
    }

    Settings::ToolchainScanOptions scanOptions;
    scanOptions.maxDepth = depth->value();
    scanOptions.excludedDirectories.clear();
    const QStringList tokens =
        excluded->text().split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        const QString name = token.trimmed();
        if (!name.isEmpty()) {
            scanOptions.excludedDirectories.append(name);
        }
    }

    commitToolchainForm();
    // The walk can take minutes on a big drive (the default depth over C:\ is
    // a lot of directories), so it runs on its own thread: the dialog stays
    // usable and the button that started it says so until the answer arrives.
    // Edits are committed first so the result is added to what the widgets
    // show.
    if (toolchainScanThread_ != nullptr) {
        return;  // one scan at a time (the button is disabled meanwhile)
    }
    setStatus(tr("Searching for compilers in %1…")
                  .arg(QDir::toNativeSeparators(dir)));
    if (toolchainScanButton_ != nullptr) {
        toolchainScanButton_->setEnabled(false);
    }
    toolchainScanAbort_ = false;
    toolchainScanRoot_ = dir;
    toolchainScanDepth_ = scanOptions.maxDepth;
    scanOptions.abort = &toolchainScanAbort_;
    const QString scanRoot = dir;
    toolchainScanThread_ = QThread::create(
        [this, scanRoot, scanOptions] {
            const QList<Settings::Toolchain> found =
                Settings::scanForToolchains(scanRoot, scanOptions);
            // Deliberately not a signal on this dialog: a queued invocation
            // keeps the payload off the header and needs no metatype. It is
            // safe because the destructor waits for this thread.
            QMetaObject::invokeMethod(
                this, [this, found] { onToolchainScanFinished(found); },
                Qt::QueuedConnection);
        });
    toolchainScanThread_->start();
}

void SettingsDialog::onToolchainScanFinished(
    const QList<LightCpp::Settings::Toolchain>& found)
{
    if (toolchainScanThread_ != nullptr) {
        toolchainScanThread_->wait();
        delete toolchainScanThread_;
        toolchainScanThread_ = nullptr;
    }
    if (toolchainScanButton_ != nullptr) {
        toolchainScanButton_->setEnabled(true);
    }
    const int depth = toolchainScanDepth_;
    if (found.isEmpty()) {
        setStatus(tr("No compilers found (depth %1).").arg(depth));
        QMessageBox::information(
            this, tr("Identify tools"),
            tr("No compilers were found under\n%1 (depth %2)")
                .arg(QDir::toNativeSeparators(toolchainScanRoot_),
                     QString::number(depth)));
        return;
    }

    // Re-scanning the same root must not append a second copy of a toolchain
    // the user already has: the list doubles on every press of Identify
    // tools… otherwise, and a freshly scanned toolchain carries the same tool
    // paths as the stored one (only the id and the display name differ).
    const QList<Settings::Toolchain> existing = state(currentScope_).toolchains;
    const auto alreadyThere = [&existing](const Settings::Toolchain& scanned) {
        return std::any_of(
            existing.cbegin(), existing.cend(),
            [&scanned](const Settings::Toolchain& present) {
                return Settings::sameTools(present, scanned);
            });
    };

    int added = 0;
    int known = 0;
    for (const Settings::Toolchain& scanned : found) {
        if (alreadyThere(scanned)) {
            ++known;
            continue;
        }
        Settings::Toolchain toolchain = scanned;
        toolchain.id = uniqueToolchainId(QStringLiteral("found-toolchain"));
        state(currentScope_).toolchains.append(toolchain);
        ++added;
    }
    if (added == 0) {
        setStatus(tr("All %1 toolchain(s) found are already in the list.")
                      .arg(known));
        return;
    }
    markDirty();
    const int firstRow =
        static_cast<int>(state(currentScope_).toolchains.size()) - added;
    rebuildToolchainList(firstRow);
    loadToolchainForm(firstRow);
    updateKitSuggestions();
    if (known > 0) {
        setStatus(tr("Created %1 toolchain(s) under %2 (%3 already known)")
                      .arg(added)
                      .arg(QDir::toNativeSeparators(toolchainScanRoot_))
                      .arg(known));
    } else {
        setStatus(tr("Created %1 toolchain(s) under %2")
                      .arg(added)
                      .arg(QDir::toNativeSeparators(toolchainScanRoot_)));
    }
}

SettingsDialog::~SettingsDialog()
{
    if (toolchainScanThread_ == nullptr) {
        return;
    }
    // The walk checks the flag between directories, so this wait is bounded
    // by one directory listing — not by the depth the user asked for.
    toolchainScanAbort_ = true;
    toolchainScanThread_->wait();
    delete toolchainScanThread_;
    toolchainScanThread_ = nullptr;
}

// ---------------------------------------------------------------------------
// Kit master-detail
// ---------------------------------------------------------------------------

void SettingsDialog::loadKitForm(int kitIndex)
{
    LoadGuard guard(this);
    // The inheritance display snapshot belongs to the kit being shown; drop
    // any leftover from the previously selected kit first.
    kitToolFieldsBackup_.clear();
    currentKitIndex_ = kitIndex;
    currentConfigIndex_ = -1;
    const QList<Settings::Kit>& list = kits();
    const bool have = kitIndex >= 0 && kitIndex < list.size();
    kitFormContainer_->setEnabled(have);
    configGroup_->setEnabled(have);
    if (!have) {
        kitId_->clear();
        kitName_->clear();
        kitEnabled_->setChecked(true);
        kitIsDefault_->setChecked(false);
        kitInheritToolchain_->setChecked(false);
        populateKitToolchainCombo(QString());
        kitCompilerFamily_->setCurrentIndex(0);
        for (QComboBox* combo : {kitCCompiler_, kitCppCompiler_, kitDebugger_,
                                 kitClangd_, kitClangFormat_, kitToolCmake_,
                                 kitToolNinja_, kitToolGnuMake_, kitToolMsbuild_,
                                 kitCppStandard_, kitClangFormatStyle_,
                                 kitClangdFallbackStyle_}) {
            combo->setCurrentText(QString());
        }
        kitCompileFlags_->clear();
        kitLinkFlags_->clear();
        kitGitPath_->clear();
        kitGitArgs_->clear();
        configList_->clear();
        clearConfigWidgets(false);
        updateKitInheritanceUi();
        return;
    }

    const Settings::Kit& kit = list[kitIndex];
    kitId_->setText(kit.id);
    kitName_->setText(kit.name);
    kitEnabled_->setChecked(kit.enabled);
    kitIsDefault_->setChecked(kit.isDefault);
    kitInheritToolchain_->setChecked(kit.inheritToolchain);
    populateKitToolchainCombo(kit.toolchainId);
    const int familyIndex = kitCompilerFamily_->findData(kit.compilerFamily);
    kitCompilerFamily_->setCurrentIndex(familyIndex < 0 ? 0 : familyIndex);
    kitCCompiler_->setCurrentText(kit.cCompiler);
    kitCppCompiler_->setCurrentText(kit.cppCompiler);
    kitDebugger_->setCurrentText(kit.debugger);
    kitClangd_->setCurrentText(kit.clangd);
    kitClangFormat_->setCurrentText(kit.clangFormat);
    kitToolCmake_->setCurrentText(kit.buildTools.value(QStringLiteral("cmake")));
    kitToolNinja_->setCurrentText(kit.buildTools.value(QStringLiteral("ninja")));
    kitToolGnuMake_->setCurrentText(kit.buildTools.value(QStringLiteral("gnuMake")));
    kitToolMsbuild_->setCurrentText(kit.buildTools.value(QStringLiteral("msbuild")));
    kitCppStandard_->setCurrentText(kit.cppStandard);
    kitCompileFlags_->setPlainText(kit.compileFlags.join(QLatin1Char('\n')));
    kitLinkFlags_->setPlainText(kit.linkFlags.join(QLatin1Char('\n')));
    kitGitPath_->setText(kit.gitPath);
    kitGitArgs_->setPlainText(kit.gitArgs.join(QLatin1Char('\n')));
    kitClangFormatStyle_->setCurrentText(kit.clangFormatStyle);
    kitClangdFallbackStyle_->setCurrentText(kit.clangdFallbackStyle);
    // The fields now show the kit's own values: that is the snapshot the
    // inheritance display restores from and treats as "the kit's own".
    saveKitToolFieldsSnapshot();
    updateKitInheritanceUi();

    rebuildConfigList(-1);
    if (!kit.buildConfigurations.isEmpty()) {
        configList_->setCurrentRow(0);
        currentConfigIndex_ = 0;
        loadConfigForm(0);
    } else {
        clearConfigWidgets(false);
    }
}

void SettingsDialog::commitKitForm()
{
    commitConfigForm();
    if (currentKitIndex_ < 0 || currentKitIndex_ >= kits().size()) {
        return;
    }
    Settings::Kit& kit = kits()[currentKitIndex_];
    kit.id = kitId_->text().trimmed();
    kit.name = kitName_->text().trimmed();
    kit.enabled = kitEnabled_->isChecked();
    kit.isDefault = kitIsDefault_->isChecked();
    kit.inheritToolchain = kitInheritToolchain_->isChecked();
    // currentData(), not currentText(): the row may say "gcc (missing)" or
    // "(none)" while the value it carries is the id the kit stores.
    kit.toolchainId = kitToolchainId_->currentData().toString().trimmed();
    kit.compilerFamily = kitCompilerFamily_->currentData().toString();
    // While inheriting, the tool widgets display the resolved toolchain
    // paths; committing them would freeze today's toolchain into the kit's
    // own fields and silently break the inheritance (non-empty kit fields
    // win over the toolchain). Leave the stored fallbacks untouched.
    if (!kit.inheritToolchain) {
        kit.cCompiler = kitCCompiler_->currentText().trimmed();
        kit.cppCompiler = kitCppCompiler_->currentText().trimmed();
        kit.debugger = kitDebugger_->currentText().trimmed();
        kit.clangd = kitClangd_->currentText().trimmed();
        kit.clangFormat = kitClangFormat_->currentText().trimmed();
        const QHash<QComboBox*, const char*> toolFields = {
            {kitToolCmake_, "cmake"}, {kitToolNinja_, "ninja"},
            {kitToolGnuMake_, "gnuMake"}, {kitToolMsbuild_, "msbuild"}};
        for (auto it = toolFields.constBegin(); it != toolFields.constEnd(); ++it) {
            const QString path = it.key()->currentText().trimmed();
            if (path.isEmpty()) {
                kit.buildTools.remove(QLatin1String(it.value()));
            } else {
                kit.buildTools.insert(QLatin1String(it.value()), path);
            }
        }
    }
    kit.cppStandard = kitCppStandard_->currentText().trimmed();
    kit.compileFlags = linesOf(kitCompileFlags_);
    kit.linkFlags = linesOf(kitLinkFlags_);
    kit.gitPath = kitGitPath_->text().trimmed();
    kit.gitArgs = linesOf(kitGitArgs_);
    kit.clangFormatStyle = kitClangFormatStyle_->currentText().trimmed();
    kit.clangdFallbackStyle = kitClangdFallbackStyle_->currentText().trimmed();
    if (currentKitIndex_ < kitList_->count()) {
        kitList_->item(currentKitIndex_)->setText(kitDisplayName(kit));
    }
}

void SettingsDialog::loadConfigForm(int configIndex)
{
    LoadGuard guard(this);
    currentConfigIndex_ = configIndex;
    const bool kitValid =
        currentKitIndex_ >= 0 && currentKitIndex_ < kits().size();
    const bool have = kitValid && configIndex >= 0
        && configIndex < kits()[currentKitIndex_].buildConfigurations.size();
    if (!have) {
        clearConfigWidgets(false);
        return;
    }
    const Settings::KitBuildConfiguration& config =
        kits()[currentKitIndex_].buildConfigurations[configIndex];
    configFormContainer_->setEnabled(true);
    configName_->setText(config.name);
    configInherit_->setChecked(config.inheritFromKit);
    configCompileFlags_->setPlainText(config.compileFlags.join(QLatin1Char('\n')));
    configLinkFlags_->setPlainText(config.linkFlags.join(QLatin1Char('\n')));
    configCppStandard_->setCurrentText(config.cppStandard);
    configArgsCmake_->setPlainText(
        config.buildToolArgs.value(QStringLiteral("cmake")).join(QLatin1Char('\n')));
    configArgsNinja_->setPlainText(
        config.buildToolArgs.value(QStringLiteral("ninja")).join(QLatin1Char('\n')));
    configArgsGnuMake_->setPlainText(
        config.buildToolArgs.value(QStringLiteral("gnuMake")).join(QLatin1Char('\n')));
    configArgsMsbuild_->setPlainText(
        config.buildToolArgs.value(QStringLiteral("msbuild")).join(QLatin1Char('\n')));
}

void SettingsDialog::commitConfigForm()
{
    if (currentKitIndex_ < 0 || currentKitIndex_ >= kits().size()) {
        return;
    }
    QList<Settings::KitBuildConfiguration>& configs =
        kits()[currentKitIndex_].buildConfigurations;
    if (currentConfigIndex_ < 0 || currentConfigIndex_ >= configs.size()) {
        return;
    }
    Settings::KitBuildConfiguration& config = configs[currentConfigIndex_];
    config.name = configName_->text().trimmed();
    config.inheritFromKit = configInherit_->isChecked();
    config.compileFlags = linesOf(configCompileFlags_);
    config.linkFlags = linesOf(configLinkFlags_);
    config.cppStandard = configCppStandard_->currentText().trimmed();
    config.buildToolArgs.clear();
    config.buildToolArgs.insert(QStringLiteral("cmake"), linesOf(configArgsCmake_));
    config.buildToolArgs.insert(QStringLiteral("ninja"), linesOf(configArgsNinja_));
    config.buildToolArgs.insert(QStringLiteral("gnuMake"), linesOf(configArgsGnuMake_));
    config.buildToolArgs.insert(QStringLiteral("msbuild"), linesOf(configArgsMsbuild_));
    if (currentConfigIndex_ < configList_->count()) {
        configList_->item(currentConfigIndex_)
            ->setText(config.name.isEmpty() ? tr("(unnamed)") : config.name);
    }
}

void SettingsDialog::clearConfigWidgets(bool enabled)
{
    configFormContainer_->setEnabled(enabled);
    configName_->clear();
    configInherit_->setChecked(true);
    configCompileFlags_->clear();
    configLinkFlags_->clear();
    configCppStandard_->setCurrentText(QString());
    configArgsCmake_->clear();
    configArgsNinja_->clear();
    configArgsGnuMake_->clear();
    configArgsMsbuild_->clear();
}

void SettingsDialog::rebuildKitList(int selectRow)
{
    kitList_->blockSignals(true);
    kitList_->clear();
    for (const Settings::Kit& kit : kits()) {
        kitList_->addItem(kitDisplayName(kit));
    }
    if (selectRow >= 0 && selectRow < kitList_->count()) {
        kitList_->setCurrentRow(selectRow);
    }
    kitList_->blockSignals(false);
}

void SettingsDialog::rebuildConfigList(int selectRow)
{
    configList_->blockSignals(true);
    configList_->clear();
    if (currentKitIndex_ >= 0 && currentKitIndex_ < kits().size()) {
        for (const Settings::KitBuildConfiguration& config :
             kits()[currentKitIndex_].buildConfigurations) {
            configList_->addItem(config.name.isEmpty() ? tr("(unnamed)") : config.name);
        }
    }
    if (selectRow >= 0 && selectRow < configList_->count()) {
        configList_->setCurrentRow(selectRow);
    }
    configList_->blockSignals(false);
}

void SettingsDialog::addKit()
{
    commitKitForm();
    Settings::Kit kit;
    kit.enabled = true;
    kit.id = uniqueKitId(QStringLiteral("new-kit"));
    kits().append(kit);
    markDirty();
    const int row = kits().size() - 1;
    rebuildKitList(row);
    loadKitForm(row);
    updateDefaultsKitItems();
    kitId_->setFocus();
    kitId_->selectAll();
}

void SettingsDialog::duplicateKit()
{
    commitKitForm();
    if (currentKitIndex_ < 0 || currentKitIndex_ >= kits().size()) {
        return;
    }
    Settings::Kit copy = kits()[currentKitIndex_];
    copy.id = uniqueKitId(copy.id.isEmpty() ? QStringLiteral("kit-copy")
                                            : copy.id + QStringLiteral("-copy"));
    copy.isDefault = false;
    const int row = currentKitIndex_ + 1;
    kits().insert(row, copy);
    markDirty();
    rebuildKitList(row);
    loadKitForm(row);
    updateDefaultsKitItems();
}

void SettingsDialog::removeKit()
{
    if (currentKitIndex_ < 0 || currentKitIndex_ >= kits().size()) {
        return;
    }
    const Settings::Kit& kit = kits()[currentKitIndex_];
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("Remove kit"),
        tr("Remove kit \"%1\"?").arg(kitDisplayName(kit)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    kits().removeAt(currentKitIndex_);
    markDirty();
    const int row = qBound(0, currentKitIndex_, static_cast<int>(kits().size()) - 1);
    const bool any = !kits().isEmpty();
    rebuildKitList(any ? row : -1);
    loadKitForm(any ? row : -1);
    updateDefaultsKitItems();
}

void SettingsDialog::addBuildConfiguration()
{
    commitConfigForm();
    if (currentKitIndex_ < 0 || currentKitIndex_ >= kits().size()) {
        return;
    }
    Settings::KitBuildConfiguration config;
    config.name = uniqueConfigName(QStringLiteral("Debug"));
    config.inheritFromKit = true;
    kits()[currentKitIndex_].buildConfigurations.append(config);
    markDirty();
    const int row = kits()[currentKitIndex_].buildConfigurations.size() - 1;
    rebuildConfigList(row);
    loadConfigForm(row);
    configName_->setFocus();
    configName_->selectAll();
}

void SettingsDialog::removeBuildConfiguration()
{
    if (currentKitIndex_ < 0 || currentKitIndex_ >= kits().size()) {
        return;
    }
    QList<Settings::KitBuildConfiguration>& configs =
        kits()[currentKitIndex_].buildConfigurations;
    if (currentConfigIndex_ < 0 || currentConfigIndex_ >= configs.size()) {
        return;
    }
    configs.removeAt(currentConfigIndex_);
    markDirty();
    const bool any = !configs.isEmpty();
    const int row = any ? qBound(0, currentConfigIndex_, static_cast<int>(configs.size()) - 1) : -1;
    currentConfigIndex_ = -1;
    rebuildConfigList(any ? row : -1);
    loadConfigForm(row);
}

void SettingsDialog::updateKitSuggestions()
{
    // Tool suggestions come from the toolchains the user actually has —
    // including the base layer's while editing the project layer, or the
    // fields of a kit that inherits a global toolchain would look empty.
    const QList<Settings::Toolchain> toolchains = visibleToolchains(currentScope_);
    QHash<QString, QStringList> byKey;
    for (const Settings::Toolchain& toolchain : toolchains) {
        for (const ToolchainToolSpec& spec : kToolchainToolSpecs) {
            const QString path =
                toolchainPath(toolchain, QLatin1String(spec.key));
            if (!path.isEmpty()) {
                byKey[QLatin1String(spec.key)].append(path);
            }
        }
    }
    // The kit's C/C++/debugger slots accept every compiler of a matching
    // kind the toolchains carry: C slots list gcc/clang/cl, C++ slots
    // g++/clang++/cl, debugger slots gdb/lldb.
    QStringList cCompilers, cppCompilers, debuggers;
    for (const Settings::Toolchain& toolchain : toolchains) {
        for (const QString& key :
             {QStringLiteral("gcc"), QStringLiteral("g++"),
              QStringLiteral("clang"), QStringLiteral("clang++"),
              QStringLiteral("cl"), QStringLiteral("gdb"),
              QStringLiteral("lldb")}) {
            const QString path = toolchain.compilers.value(key);
            if (path.isEmpty()) {
                continue;
            }
            if (key == QLatin1String("gcc") || key == QLatin1String("clang")
                || key == QLatin1String("cl")) {
                cCompilers.append(path);
            }
            if (key == QLatin1String("g++") || key == QLatin1String("clang++")
                || key == QLatin1String("cl")) {
                cppCompilers.append(path);
            }
            if (key == QLatin1String("gdb") || key == QLatin1String("lldb")) {
                debuggers.append(path);
            }
        }
    }
    const auto suggest = [](QComboBox* combo, const QStringList& items) {
        const QString current = combo->currentText();
        QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItems(items);
        combo->setCurrentText(current);
    };
    suggest(kitCCompiler_, cCompilers);
    suggest(kitCppCompiler_, cppCompilers);
    suggest(kitDebugger_, debuggers);
    suggest(kitClangd_, byKey.value(QStringLiteral("clangd")));
    suggest(kitClangFormat_, byKey.value(QStringLiteral("clangFormat")));
    suggest(kitToolCmake_, byKey.value(QStringLiteral("cmake")));
    suggest(kitToolNinja_, byKey.value(QStringLiteral("ninja")));
    suggest(kitToolGnuMake_, byKey.value(QStringLiteral("gnuMake")));
    suggest(kitToolMsbuild_, byKey.value(QStringLiteral("msbuild")));

    // The inheritance combo names the toolchains by id — every id the user
    // can refer to, and the selected one even when nothing defines it.
    populateKitToolchainCombo(kitToolchainId_->currentData().toString());
}

QList<Settings::Toolchain> SettingsDialog::visibleToolchains(Scope scope) const
{
    // The base layer is written and read on its own, so a base kit may only
    // lean on a base toolchain; a project layer legally inherits a global
    // one, and calling that id missing is what blanked it (R2-32) and then
    // refused to save it (R2-36).
    const QList<Settings::Toolchain>& base = state(Scope::Base).toolchains;
    if (scope == Scope::Overlay) {
        return Settings::mergedToolchains(base, state(Scope::Overlay).toolchains);
    }
    return base;
}

void SettingsDialog::populateKitToolchainCombo(const QString& storedId)
{
    QStringList ids;
    for (const Settings::Toolchain& toolchain : visibleToolchains(currentScope_)) {
        ids.append(toolchain.id);
    }
    const KitToolchainOptions options = kitToolchainOptionsFor(ids, storedId);
    const QSignalBlocker blocker(kitToolchainId_);
    kitToolchainId_->clear();
    for (const KitToolchainOption& option : options.items) {
        kitToolchainId_->addItem(option.text, option.value);
    }
    kitToolchainId_->setCurrentIndex(options.current);
}

void SettingsDialog::updateKitInheritanceUi()
{
    // Inheriting a toolchain means the kit's tool fields show what actually
    // takes effect — the referenced toolchain's paths wherever the kit does
    // not name its own — and the self-selection rows are disabled so the
    // tools actually used cannot diverge silently from the toolchain.
    const bool inherit = kitInheritToolchain_->isChecked();
    kitToolchainId_->setEnabled(inherit);
    const QList<QComboBox*> toolCombos = {
        kitCCompiler_, kitCppCompiler_, kitDebugger_, kitClangd_,
        kitClangFormat_, kitToolCmake_, kitToolNinja_, kitToolGnuMake_,
        kitToolMsbuild_};
    for (QComboBox* combo : toolCombos) {
        combo->setEnabled(!inherit);
    }
    if (kitToolsBox_ != nullptr) {
        kitToolsBox_->setEnabled(!inherit);
    }
    if (inherit) {
        if (kitToolFieldsBackup_.isEmpty()) {
            // Manual tick: capture what the user sees right now (the kit's
            // own values, including uncommitted edits) before display takes
            // over. loadKitForm re-snapshots after refilling the fields.
            saveKitToolFieldsSnapshot();
        }
        applyKitInheritanceDisplay();
    } else {
        for (auto it = kitToolFieldsBackup_.constBegin();
             it != kitToolFieldsBackup_.constEnd(); ++it) {
            QSignalBlocker blocker(it.key());
            it.key()->setCurrentText(it.value());
        }
        kitToolFieldsBackup_.clear();
    }
}

void SettingsDialog::saveKitToolFieldsSnapshot()
{
    const QList<QComboBox*> toolCombos = {
        kitCCompiler_, kitCppCompiler_, kitDebugger_, kitClangd_,
        kitClangFormat_, kitToolCmake_, kitToolNinja_, kitToolGnuMake_,
        kitToolMsbuild_};
    kitToolFieldsBackup_.clear();
    for (QComboBox* combo : toolCombos) {
        kitToolFieldsBackup_.insert(combo, combo->currentText());
    }
}

void SettingsDialog::applyKitInheritanceDisplay()
{
    const QString toolchainId = kitToolchainId_->currentData().toString().trimmed();
    // Take a **copy** of the inherited toolchain, never a pointer into
    // visibleToolchains(): that getter returns its list by value, so a pointer
    // aimed at one of its entries dangles the moment the loop that found it
    // ends. Reading the freed entry's `compilers` hash is what crashed the
    // program the instant Settings opened — a use-after-free, not a null deref
    // (the dump showed the heap's 0xFEEEFEEE fill pattern).
    const std::optional<Settings::Toolchain> inherited =
        Settings::toolchainById(visibleToolchains(currentScope_), toolchainId);
    if (!inherited.has_value()) {
        // Nothing to inherit from: keep showing the kit's own values.
        return;
    }
    const Settings::Toolchain& toolchain = *inherited;
    // Mirror SettingsStore::applyToolchainInheritance — the kit's compiler
    // family picks which toolchain compilers land in the C/C++ slots, and
    // fields the kit names itself still win, so show the kit's snapshot
    // value when non-empty.
    const auto show = [this](QComboBox* combo, const QString& inherited) {
        const QString own = kitToolFieldsBackup_.value(combo);
        QSignalBlocker blocker(combo);
        combo->setCurrentText(own.isEmpty() ? inherited : own);
    };
    const QString family = kitCompilerFamily_->currentData().toString();
    const QString empty;
    const QString cCompiler =
        family == QLatin1String("gcc")
            ? toolchain.compilers.value(QStringLiteral("gcc"))
        : family == QLatin1String("llvm")
            ? toolchain.compilers.value(QStringLiteral("clang"))
        : family == QLatin1String("msvc")
            ? toolchain.compilers.value(QStringLiteral("cl"))
            : empty;
    const QString cppCompiler =
        family == QLatin1String("gcc")
            ? toolchain.compilers.value(QStringLiteral("g++"))
        : family == QLatin1String("llvm")
            ? toolchain.compilers.value(QStringLiteral("clang++"))
        : family == QLatin1String("msvc")
            ? toolchain.compilers.value(QStringLiteral("cl"))
            : empty;
    QString debugger;
    if (family == QLatin1String("msvc")) {
        debugger = toolchain.compilers.value(QStringLiteral("cdb"));
    } else if (family == QLatin1String("gcc")
               || family == QLatin1String("llvm")) {
        debugger = toolchain.compilers.value(QStringLiteral("gdb"));
        if (debugger.isEmpty()) {
            debugger = toolchain.compilers.value(QStringLiteral("lldb"));
        }
    }
    show(kitCCompiler_, cCompiler);
    show(kitCppCompiler_, cppCompiler);
    show(kitDebugger_, debugger);
    show(kitClangd_, toolchain.clangd);
    show(kitClangFormat_, toolchain.clangFormat);
    show(kitToolCmake_, toolchain.buildTools.value(QStringLiteral("cmake")));
    show(kitToolNinja_, toolchain.buildTools.value(QStringLiteral("ninja")));
    show(kitToolGnuMake_, toolchain.buildTools.value(QStringLiteral("gnuMake")));
    show(kitToolMsbuild_, toolchain.buildTools.value(QStringLiteral("msbuild")));
}

QString SettingsDialog::uniqueKitId(const QString& base)
{
    QSet<QString> taken;
    for (const Settings::Kit& kit : kits()) {
        taken.insert(kit.id);
    }
    if (!taken.contains(base)) {
        return base;
    }
    for (int n = 2;; ++n) {
        const QString candidate =
            base + QLatin1Char('-') + QString::number(n);
        if (!taken.contains(candidate)) {
            return candidate;
        }
    }
}

QString SettingsDialog::uniqueToolchainId(const QString& base)
{
    QSet<QString> taken;
    for (const Settings::Toolchain& toolchain : state(currentScope_).toolchains) {
        taken.insert(toolchain.id);
    }
    if (!taken.contains(base)) {
        return base;
    }
    for (int n = 2;; ++n) {
        const QString candidate =
            base + QLatin1Char('-') + QString::number(n);
        if (!taken.contains(candidate)) {
            return candidate;
        }
    }
}

QString SettingsDialog::uniqueConfigName(const QString& base)
{
    QSet<QString> taken;
    if (currentKitIndex_ >= 0 && currentKitIndex_ < kits().size()) {
        for (const Settings::KitBuildConfiguration& config :
             kits()[currentKitIndex_].buildConfigurations) {
            taken.insert(config.name);
        }
    }
    if (!taken.contains(base)) {
        return base;
    }
    for (int n = 2;; ++n) {
        const QString candidate =
            base + QLatin1Char(' ') + QString::number(n);
        if (!taken.contains(candidate)) {
            return candidate;
        }
    }
}

// ---------------------------------------------------------------------------
// Preferences page
// ---------------------------------------------------------------------------

void SettingsDialog::loadPreferencesPage(const Settings::Preferences& prefs)
{
    LoadGuard guard(this);
    loadFontSpec(toolbarFontFamily_, toolbarFontSize_, prefs.toolbarFont);
    loadFontSpec(editorFontFamily_, editorFontSize_, prefs.editorFont);
    loadFontSpec(outputFontFamily_, outputFontSize_, prefs.outputFont);
    encodingDefault_->setCurrentText(prefs.defaultEncoding);
    encodingPriority_->setPlainText(prefs.detectionPriority.join(QLatin1Char('\n')));
    lineEndingDefault_->setCurrentText(prefs.lineEndingDefault.isEmpty()
                                           ? QStringLiteral("auto")
                                           : prefs.lineEndingDefault);
    longLineMode_->setCurrentText(prefs.longLineMode.isEmpty()
                                      ? QStringLiteral("scroll")
                                      : prefs.longLineMode);
    showWhitespaces_->setChecked(prefs.showWhitespaces);
    showIndentGuides_->setChecked(prefs.showIndentGuides);
    checkCompilersAtStartup_->setChecked(prefs.checkCompilersAtStartup);
    showStartupPage_->setChecked(prefs.showStartupPage);
    restoreSession_->setChecked(prefs.restoreSession);
    checkToolchainsAtStartup_->setChecked(prefs.checkToolchainsAtStartup);
    autoSave_->setChecked(prefs.autoSave);
    autoSaveSeconds_->setValue(qBound(5, prefs.autoSaveSeconds, 3600));
    // Fill the editable terminal table from the settings. The guard keeps
    // itemChanged from treating this as a user edit; ids ride in the name
    // item's UserRole so the selection survives a reload.
    runTerminalsLoaded_ = false;
    runTerminals_->setRowCount(0);
    for (const auto& terminal : prefs.terminals) {
        const int row = runTerminals_->rowCount();
        runTerminals_->insertRow(row);
        auto* nameItem = new QTableWidgetItem(terminal.name);
        nameItem->setData(Qt::UserRole, terminal.id);
        runTerminals_->setItem(row, 0, nameItem);
        runTerminals_->setItem(row, 1, new QTableWidgetItem(terminal.command));
        auto* embeddedItem = new QTableWidgetItem();
        embeddedItem->setCheckState(terminal.embedded ? Qt::Checked
                                                      : Qt::Unchecked);
        embeddedItem->setFlags(embeddedItem->flags() | Qt::ItemIsUserCheckable);
        runTerminals_->setItem(row, 2, embeddedItem);
    }
    runTerminalsLoaded_ = true;
    // Mirror the table into the combo, then apply the saved selection —
    // which may point at a custom terminal id, not a fixed entry.
    runOutputMode_->setCurrentIndex(0);
    rebuildRunTerminalCombo();
    {
        const int runIndex = runOutputMode_->findData(prefs.programOutput);
        runOutputMode_->setCurrentIndex(runIndex < 0 ? 0 : runIndex);
    }
    shortcutClangd_->setKeySequence(
        QKeySequence(prefs.shortcutClangd, QKeySequence::PortableText));
    shortcutClangFormat_->setKeySequence(
        QKeySequence(prefs.shortcutClangFormat, QKeySequence::PortableText));
    largeFileHighlight_->setValue(prefs.largeFile.syntaxHighlightDisabledAboveMB);
    largeFileIndex_->setValue(prefs.largeFile.indexDisabledAboveMB);
    largeFileParse_->setValue(prefs.largeFile.parseDisabledAboveMB);
    clangdEnabledByDefault_->setChecked(prefs.clangd.enabledByDefault);
    clangdCompletion_->setChecked(prefs.clangd.completion);
    clangdDiagnostics_->setChecked(prefs.clangd.diagnostics);
    clangdIndexing_->setChecked(prefs.clangd.indexing);
    clangdFallbackStyle_->setCurrentText(prefs.clangd.fallbackStyle);
    completionBackend_->setCurrentText(
        prefs.completion.backend.isEmpty() ? QStringLiteral("clangd")
                                           : prefs.completion.backend);
    completionRetry_->setValue(prefs.completion.retryCount);
    clangFormatEnabled_->setChecked(prefs.clangFormat.enabled);
    clangFormatOnSave_->setChecked(prefs.clangFormat.formatOnSave);
    clangFormatStyle_->setCurrentText(prefs.clangFormat.style);
    formattingGlobalStyle_->setCurrentText(
        prefs.globalStyle.isEmpty() ? QStringLiteral("file") : prefs.globalStyle);
    formattingDefaultStyleContent_->setPlainText(prefs.defaultStyleContent);
}

void SettingsDialog::commitPreferencesPage(Settings::Preferences* prefs)
{
    prefs->toolbarFont = fontSpecOf(toolbarFontFamily_, toolbarFontSize_);
    prefs->editorFont = fontSpecOf(editorFontFamily_, editorFontSize_);
    prefs->outputFont = fontSpecOf(outputFontFamily_, outputFontSize_);
    prefs->defaultEncoding = encodingDefault_->currentText().trimmed();
    prefs->detectionPriority = linesOf(encodingPriority_);
    prefs->lineEndingDefault = lineEndingDefault_->currentText();
    prefs->longLineMode = longLineMode_->currentText().trimmed();
    prefs->showWhitespaces = showWhitespaces_->isChecked();
    prefs->showIndentGuides = showIndentGuides_->isChecked();
    prefs->checkCompilersAtStartup = checkCompilersAtStartup_->isChecked();
    prefs->showStartupPage = showStartupPage_->isChecked();
    prefs->restoreSession = restoreSession_->isChecked();
    prefs->checkToolchainsAtStartup = checkToolchainsAtStartup_->isChecked();
    prefs->autoSave = autoSave_->isChecked();
    prefs->autoSaveSeconds = autoSaveSeconds_->value();
    // recentFiles is not edited here: it is carried through from the loaded
    // state, so saving the dialog never wipes the list *within this layer*.
    // Across layers it is still an array, and arrays do not merge — an
    // overlay that has no list of its own must not write an empty one, which
    // is what saveScope()'s layerDelta() takes care of (R2-05).
    prefs->programOutput = runOutputMode_->currentData().toString();
    // Read the terminal table back. Rows keep the id generated for them
    // (or the one loaded from disk); a row without an id gets a fresh one.
    prefs->terminals.clear();
    for (int row = 0; row < runTerminals_->rowCount(); ++row) {
        Settings::RunTerminal terminal;
        if (auto* nameItem = runTerminals_->item(row, 0)) {
            terminal.id = nameItem->data(Qt::UserRole).toString();
            terminal.name = nameItem->text();
        }
        if (terminal.id.isEmpty()) {
            terminal.id = QStringLiteral("terminal-")
                + QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        if (auto* commandItem = runTerminals_->item(row, 1)) {
            terminal.command = commandItem->text().trimmed();
        }
        if (auto* embeddedItem = runTerminals_->item(row, 2)) {
            terminal.embedded = embeddedItem->checkState() == Qt::Checked;
        }
        if (terminal.name.trimmed().isEmpty()) {
            terminal.name = tr("Terminal %1").arg(row + 1);
        }
        prefs->terminals.push_back(terminal);
    }
    prefs->shortcutClangd =
        shortcutClangd_->keySequence().toString(QKeySequence::PortableText);
    prefs->shortcutClangFormat =
        shortcutClangFormat_->keySequence().toString(QKeySequence::PortableText);
    prefs->largeFile.syntaxHighlightDisabledAboveMB = largeFileHighlight_->value();
    prefs->largeFile.indexDisabledAboveMB = largeFileIndex_->value();
    prefs->largeFile.parseDisabledAboveMB = largeFileParse_->value();
    prefs->clangd.enabledByDefault = clangdEnabledByDefault_->isChecked();
    prefs->clangd.completion = clangdCompletion_->isChecked();
    prefs->clangd.diagnostics = clangdDiagnostics_->isChecked();
    prefs->clangd.indexing = clangdIndexing_->isChecked();
    prefs->clangd.fallbackStyle = clangdFallbackStyle_->currentText().trimmed();
    prefs->completion.backend = completionBackend_->currentText().trimmed();
    prefs->completion.retryCount = completionRetry_->value();
    prefs->clangFormat.enabled = clangFormatEnabled_->isChecked();
    prefs->clangFormat.formatOnSave = clangFormatOnSave_->isChecked();
    prefs->clangFormat.style = clangFormatStyle_->currentText().trimmed();
    prefs->globalStyle = formattingGlobalStyle_->currentText().trimmed();
    prefs->defaultStyleContent = formattingDefaultStyleContent_->toPlainText();
}

void SettingsDialog::rebuildRunTerminalCombo()
{
    // Keep the two fixed entries, drop the rest, then mirror the table.
    const QString selected = runOutputMode_->currentData().toString();
    while (runOutputMode_->count() > 2) {
        runOutputMode_->removeItem(runOutputMode_->count() - 1);
    }
    for (int row = 0; row < runTerminals_->rowCount(); ++row) {
        auto* nameItem = runTerminals_->item(row, 0);
        if (nameItem == nullptr || nameItem->text().trimmed().isEmpty()) {
            continue;
        }
        const QString id = nameItem->data(Qt::UserRole).toString();
        runOutputMode_->addItem(nameItem->text(), id);
    }
    // Preserve the selection when its terminal still exists.
    const int index = runOutputMode_->findData(selected);
    runOutputMode_->setCurrentIndex(index < 0 ? 0 : index);
}

// ---------------------------------------------------------------------------
// Commit / save
// ---------------------------------------------------------------------------

void SettingsDialog::commitUiToState()
{
    commitKitForm();
    commitToolchainForm();
    ScopeState& current = state(currentScope_);
    commitDefaultsPage(&current.defaults);
    commitPreferencesPage(&current.preferences);
}

bool SettingsDialog::validateScope(Scope scope, QString* errorMessage) const
{
    const ScopeState& current = state(scope);
    QSet<QString> ids;
    for (int i = 0; i < current.kits.size(); ++i) {
        const Settings::Kit& kit = current.kits[i];
        if (kit.id.trimmed().isEmpty()) {
            if (errorMessage != nullptr) {
                *errorMessage = tr("Kit #%1 has an empty id.").arg(i + 1);
            }
            return false;
        }
        if (ids.contains(kit.id)) {
            if (errorMessage != nullptr) {
                *errorMessage = tr("Duplicate kit id \"%1\".").arg(kit.id);
            }
            return false;
        }
        ids.insert(kit.id);
    }
    QSet<QString> toolchainIds;
    for (int i = 0; i < current.toolchains.size(); ++i) {
        const Settings::Toolchain& toolchain = current.toolchains[i];
        if (toolchain.id.trimmed().isEmpty()) {
            if (errorMessage != nullptr) {
                *errorMessage =
                    tr("Toolchain #%1 has an empty id.").arg(i + 1);
            }
            return false;
        }
        if (toolchainIds.contains(toolchain.id)) {
            if (errorMessage != nullptr) {
                *errorMessage =
                    tr("Duplicate toolchain id \"%1\".").arg(toolchain.id);
            }
            return false;
        }
        toolchainIds.insert(toolchain.id);
    }
    // A kit must name a toolchain the user's configuration will have. That is
    // not the same as "a toolchain this layer defines": when the project layer
    // is being saved, every global toolchain counts too (R2-36). Validating
    // against this layer alone turned a normal setup into a red box that
    // blocked saving, after the id itself had been blanked (R2-32).
    QSet<QString> visibleIds;
    for (const Settings::Toolchain& toolchain : visibleToolchains(scope)) {
        if (!toolchain.id.isEmpty()) {
            visibleIds.insert(toolchain.id);
        }
    }
    for (int i = 0; i < current.kits.size(); ++i) {
        const Settings::Kit& kit = current.kits[i];
        if (!kit.inheritToolchain) {
            continue;
        }
        if (kit.toolchainId.isEmpty()) {
            if (errorMessage != nullptr) {
                *errorMessage = tr("Kit \"%1\" inherits a toolchain but names "
                                   "none.")
                                    .arg(kitDisplayName(kit));
            }
            return false;
        }
        if (!visibleIds.contains(kit.toolchainId)) {
            if (errorMessage != nullptr) {
                *errorMessage = tr("Kit \"%1\" inherits from toolchain "
                                   "\"%2\", which does not exist.")
                                    .arg(kitDisplayName(kit), kit.toolchainId);
            }
            return false;
        }
    }
    return true;
}

bool SettingsDialog::saveScope(Scope scope, QString* errorMessage)
{
    // The widgets are the source of truth: flush them into the working
    // state first — but only for the layer they actually show.
    if (scope == currentScope_) {
        commitUiToState();
    }
    const QString dir = directoryFor(scope);
    if (dir.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("The %1 layer has no directory.")
                                .arg(scopeDisplayName(scope));
        }
        return false;
    }
    if (!QDir().mkpath(dir)) {
        if (errorMessage != nullptr) {
            *errorMessage = tr("Cannot create directory %1.")
                                .arg(QDir::toNativeSeparators(dir));
        }
        return false;
    }
    const auto write = [&](const char* fileName, const QJsonDocument& document) {
        const QString path = QDir(dir).filePath(QLatin1String(fileName));
        if (Settings::SettingsStore::writeDocument(path, document)) {
            return true;
        }
        if (errorMessage != nullptr) {
            *errorMessage = tr("Cannot write %1.").arg(QDir::toNativeSeparators(path));
        }
        return false;
    };
    ScopeState& current = state(scope);

    // Only what this layer actually says gets written. A field the user
    // never touched still holds its struct default (an empty string, an
    // empty list), and writing those out would not read back as "unset":
    // the layers are merged later-wins, so a project layer would blank
    // every global setting it does not mention — the default kit, the
    // clangd/git/clang-format paths, the encoding and line-ending choices,
    // the recents list, the disabled plugins (R2-05).
    //
    // layerDelta() keeps a key when the layer's own document already had it
    // or when its value differs from what this layer held when it was read,
    // and carries over keys the model does not produce — the same courtesy
    // mergeUnknownKeys() extends. The toolchain/kit arrays are whole
    // documents matched by id, so they are written as they are.
    const auto layerDocument = [&](const char* fileName, const QJsonObject& fresh,
                                   const QJsonObject& loaded, bool* hasContent) {
        QJsonObject previous;
        QFile file(QDir(dir).filePath(QLatin1String(fileName)));
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonDocument parsed =
                QJsonDocument::fromJson(file.readAll());
            file.close();
            if (parsed.isObject()) {
                previous = parsed.object();
            }
        }
        const QJsonObject delta =
            Settings::SettingsStore::layerDelta(fresh, loaded, previous);
        *hasContent = Settings::SettingsStore::layerDeltaHasContent(delta);
        return QJsonDocument(delta);
    };

    QJsonArray toolchainArray;
    for (const Settings::Toolchain& toolchain : current.toolchains) {
        toolchainArray.append(Settings::toJson(toolchain));
    }
    if (!write("toolchains.json", QJsonDocument(toolchainArray))) {
        return false;
    }
    QJsonArray kitArray;
    for (const Settings::Kit& kit : current.kits) {
        kitArray.append(Settings::toJson(kit));
    }
    if (!write("kits.json", QJsonDocument(kitArray))) {
        return false;
    }
    bool hasContent = false;
    const QJsonDocument defaults =
        layerDocument("defaults.json", Settings::toJson(current.defaults),
                      current.loadedDefaults, &hasContent);
    if (hasContent && !write("defaults.json", defaults)) {
        return false;
    }
    const QJsonDocument preferences =
        layerDocument("preferences.json", Settings::toJson(current.preferences),
                      current.loadedPreferences, &hasContent);
    if (hasContent && !write("preferences.json", preferences)) {
        return false;
    }
    current.modified = false;
    setStatus(tr("Saved to %1").arg(QDir::toNativeSeparators(dir)));
    return true;
}

bool SettingsDialog::saveModifiedScopes()
{
    commitUiToState();
    const Scope scopes[] = {Scope::Base, Scope::Overlay};
    for (Scope scope : scopes) {
        if (!state(scope).modified) {
            continue;
        }
        QString error;
        if (!validateScope(scope, &error)) {
            QMessageBox::critical(this, tr("Settings"), error);
            return false;
        }
    }
    for (Scope scope : scopes) {
        if (!state(scope).modified) {
            continue;
        }
        QString error;
        if (!saveScope(scope, &error)) {
            QMessageBox::critical(this, tr("Settings"), error);
            return false;
        }
    }
    return true;
}

void SettingsDialog::accept()
{
    if (!saveModifiedScopes()) {
        return;  // the error box names the problem; stay open
    }
    QDialog::accept();
}

void SettingsDialog::reject()
{
    if ((state(Scope::Base).modified || state(Scope::Overlay).modified)
        && QMessageBox::question(this, tr("Discard changes?"),
                                 tr("Discard unsaved changes?"),
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::Yes)
            != QMessageBox::Yes) {
        return;
    }
    QDialog::reject();
}

void SettingsDialog::markDirty()
{
    if (loading_ > 0) {
        return;
    }
    state(currentScope_).modified = true;
}

void SettingsDialog::setStatus(const QString& text)
{
    statusLabel_->setText(text);
}

QString SettingsDialog::scopeDisplayName(Scope scope) const
{
    if (scope == Scope::Overlay) {
        return tr("Project settings");
    }
    return tr("Global settings");
}

}  // namespace LightCpp::App
