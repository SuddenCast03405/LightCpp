// SettingsDialog — GUI editor for the IDE's configuration documents.
//
// One dialog, four pages (defaults / toolchains / kits / preferences), one
// layer at a time. Reading goes through SettingsJson's lenient parsers and
// writing through SettingsStore::writeDocument, so the on-disk format is
// exactly the one the starter writer produces and the store merges.

#include "LightCpp/App/SettingsDialog.hpp"

#include "LightCpp/Settings/SettingsJson.hpp"
#include "LightCpp/Settings/SettingsStore.hpp"

#include <QCheckBox>
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
#include <QInputDialog>
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
#include <QVBoxLayout>

#include <functional>

namespace LightCpp::App {

namespace Settings = LightCpp::Settings;

namespace {

// The four standard documents the dialog reads and writes. Files with
// other names in the same directory are deliberately neither read nor
// touched: the dialog must never destroy configuration it does not
// understand.
constexpr const char* kConfigFiles[] = {
    "toolchains.json", "kits.json", "defaults.json", "preferences.json"};

constexpr const char* kBuildToolKeys[] = {"cmake", "ninja", "gnuMake", "msbuild"};

constexpr const char* kStylePresets[] = {
    "file", "LLVM", "Google", "Chromium", "Mozilla", "WebKit", "Microsoft", "GNU"};

constexpr const char* kCppStandards[] = {
    "", "c++14", "c++17", "c++20", "gnu++14", "gnu++17", "gnu++20", "c++23", "gnu++23"};

constexpr const char* kEncodingNames[] = {
    "UTF-8", "GB18030", "GBK", "Big5", "Shift-JIS", "EUC-KR",
    "UTF-16LE", "UTF-16BE", "Windows-1252", "ISO-8859-1"};

/// One entry of the toolchains page: registry key + human label.
struct RegistrySpec {
    const char* key;
    const char* label;
};
constexpr RegistrySpec kRegistrySpecs[] = {
    {"cCompilers", QT_TRANSLATE_NOOP("SettingsDialog", "C compilers")},
    {"cppCompilers", QT_TRANSLATE_NOOP("SettingsDialog", "C++ compilers")},
    {"debuggers", QT_TRANSLATE_NOOP("SettingsDialog", "Debuggers")},
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

/// A labelled list of paths with Browse/Remove buttons. Plain QGroupBox
/// subclass without Q_OBJECT: everything is wired with lambdas.
class PathListEditor final : public QGroupBox {
public:
    PathListEditor(const QString& title, QWidget* parent)
        : QGroupBox(title, parent)
    {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 6, 6, 6);
        list_ = new QListWidget(this);
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        list_->setToolTip(
            tr("Add a path with Browse; double-click an entry to edit it."));
        layout->addWidget(list_, 1);

        auto* buttons = new QVBoxLayout;
        auto* add = new QPushButton(tr("Browse..."), this);
        connect(add, &QPushButton::clicked, this, [this] { browse(); });
        buttons->addWidget(add);
        auto* remove = new QPushButton(tr("Remove"), this);
        remove->setEnabled(false);
        connect(remove, &QPushButton::clicked, this, [this, remove] {
            const int row = list_->currentRow();
            if (row >= 0) {
                delete list_->takeItem(row);
                remove->setEnabled(false);
                if (onChange_) {
                    onChange_();
                }
            }
        });
        buttons->addWidget(remove);
        buttons->addStretch(1);
        connect(list_, &QListWidget::itemSelectionChanged, this,
                [remove, this] { remove->setEnabled(list_->currentRow() >= 0); });
        connect(list_, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem* item) {
                    bool ok = false;
                    const QString value = QInputDialog::getText(
                        this, tr("Edit entry"), tr("Path:"), QLineEdit::Normal,
                        item->text(), &ok);
                    if (ok) {
                        const QString trimmed = value.trimmed();
                        if (!trimmed.isEmpty()) {
                            item->setText(trimmed);
                            if (onChange_) {
                                onChange_();
                            }
                        }
                    }
                });
        layout->addLayout(buttons);
    }

    QListWidget* list() const { return list_; }

    void setItems(const QStringList& items)
    {
        list_->clear();
        list_->addItems(items);
    }

    QStringList items() const
    {
        QStringList out;
        for (int i = 0; i < list_->count(); ++i) {
            out.append(list_->item(i)->text());
        }
        return out;
    }

    /// Appends one path (used by Browse and by tests driving the widget).
    void addPath(const QString& path)
    {
        const QString trimmed = path.trimmed();
        if (trimmed.isEmpty()) {
            return;
        }
        list_->addItem(trimmed);
        list_->setCurrentRow(list_->count() - 1);
    }

    /// Called after user-driven changes only; programmatic setItems/addPath
    /// stay silent (loads run under the dialog's loading guard anyway).
    void setOnChange(std::function<void()> callback) { onChange_ = std::move(callback); }

private:
    void browse()
    {
        const QString start = list_->currentItem()
            ? QFileInfo(list_->currentItem()->text()).absolutePath()
            : QString();
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Select a tool"), start, tr("Executables (*.exe);;All files (*)"));
        if (path.isEmpty()) {
            return;
        }
        addPath(path);
        if (onChange_) {
            onChange_();
        }
    }

    QListWidget* list_ = nullptr;
    std::function<void()> onChange_;
};

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
    scopeCombo_->addItem(tr("Global settings (exe directory)"),
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
    nav_->addItem(tr("Defaults"));
    nav_->addItem(tr("Toolchains"));
    nav_->addItem(tr("Kits"));
    nav_->addItem(tr("Preferences"));
    nav_->setCurrentRow(0);
    bodyRow->addWidget(nav_);

    pages_ = new QStackedWidget(this);
    pages_->setObjectName(QStringLiteral("settings.pages"));
    pages_->addWidget(wrapInScroll(buildDefaultsPage()));
    pages_->addWidget(wrapInScroll(buildToolchainsPage()));
    pages_->addWidget(wrapInScroll(buildKitsPage()));
    pages_->addWidget(wrapInScroll(buildPreferencesPage()));
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
            [this] { saveModifiedScopes(); });
    rootLayout->addWidget(buttons);

    // --- wiring -----------------------------------------------------------------
    connect(nav_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (loading_ > 0 || row < 0) {
            return;
        }
        commitUiToState();  // flush the page the user is leaving
        pages_->setCurrentIndex(row);
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

QWidget* SettingsDialog::buildDefaultsPage()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);

    defaultsKitId_ = makeCombo(QStringLiteral("defaults.kitId"), true);
    defaultsKitId_->setToolTip(
        tr("The kit selected on start-up. Kits are defined on the Kits page."));
    form->addRow(tr("Default kit"), defaultsKitId_);

    defaultsBuildConfig_ = makeCombo(QStringLiteral("defaults.buildConfig"), true);
    defaultsBuildConfig_->setToolTip(
        tr("The build configuration (Debug/Release/...) selected on start-up."));
    form->addRow(tr("Default build configuration"), defaultsBuildConfig_);

    const auto addPathRow = [this, form, page](const char* objectName,
                                               QLineEdit** editOut,
                                               const QString& label,
                                               const QString& tip) {
        auto* edit = makeLine(QLatin1String(objectName));
        edit->setToolTip(tip);
        auto* browse = new QPushButton(tr("Browse..."), page);
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
    return page;
}

QWidget* SettingsDialog::buildToolchainsPage()
{
    auto* page = new QWidget(this);
    auto* grid = new QGridLayout(page);
    int row = 0;
    int column = 0;
    for (const RegistrySpec& spec : kRegistrySpecs) {
        const QString key = QLatin1String(spec.key);
        auto* editor = new PathListEditor(tr(spec.label), page);
        editor->setOnChange([this] { markDirty(); });
        editor->list()->setObjectName(QStringLiteral("toolchains.%1.list").arg(key));
        registryLists_.insert(key, editor->list());
        grid->addWidget(editor, row, column);
        if (++column == 2) {
            column = 0;
            ++row;
        }
    }
    grid->setRowStretch(row + 1, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
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

QWidget* SettingsDialog::buildPreferencesPage()
{
    auto* page = new QWidget(this);
    auto* column = new QVBoxLayout(page);

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
    auto* platformRow = new QHBoxLayout;
    const auto addPlatformRow = [this](QHBoxLayout* row, const char* name,
                                       QComboBox** out, const QString& label) {
        auto* combo = makeCombo(QLatin1String(name), false);
        combo->addItems({QStringLiteral("LF"), QStringLiteral("CRLF")});
        row->addWidget(new QLabel(label, combo));
        row->addWidget(combo, 1);
        *out = combo;
    };
    addPlatformRow(platformRow, "prefs.lineEnding.windows", &lineEndingWindows_,
                   tr("Windows"));
    addPlatformRow(platformRow, "prefs.lineEnding.linux", &lineEndingLinux_,
                   tr("Linux"));
    addPlatformRow(platformRow, "prefs.lineEnding.macos", &lineEndingMacos_,
                   tr("macOS"));
    filesForm->addRow(tr("Line ending overrides"), platformRow);
    column->addWidget(files);

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

    column->addStretch(1);
    return page;
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
            if (type == QLatin1String("toolchains")) {
                Settings::fromJson(object, target.toolchains);
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
// Toolchains page
// ---------------------------------------------------------------------------

void SettingsDialog::loadToolchainsPage(const Settings::ToolchainLists& lists)
{
    LoadGuard guard(this);
    for (const RegistrySpec& spec : kRegistrySpecs) {
        const QString key = QLatin1String(spec.key);
        QListWidget* list = registryLists_.value(key);
        if (list == nullptr) {
            continue;
        }
        const QStringList items = key == QLatin1String("cCompilers") ? lists.cCompilers
            : key == QLatin1String("cppCompilers")                   ? lists.cppCompilers
            : key == QLatin1String("debuggers")                      ? lists.debuggers
            : key == QLatin1String("clangd")                         ? lists.clangd
            : key == QLatin1String("clangFormat")                    ? lists.clangFormat
                                                                     : lists.buildTools.value(key);
        // Same refill as PathListEditor::setItems, without a cast back to
        // the editor wrapper (it has no Q_OBJECT, so qobject_cast refuses).
        list->clear();
        for (const QString& item : items) {
            list->addItem(item);
        }
    }
}

void SettingsDialog::commitToolchainsPage(Settings::ToolchainLists* lists)
{
    for (const RegistrySpec& spec : kRegistrySpecs) {
        const QString key = QLatin1String(spec.key);
        const QListWidget* list = registryLists_.value(key);
        if (list == nullptr) {
            continue;
        }
        QStringList items;
        for (int i = 0; i < list->count(); ++i) {
            items.append(list->item(i)->text());
        }
        if (key == QLatin1String("cCompilers")) {
            lists->cCompilers = items;
        } else if (key == QLatin1String("cppCompilers")) {
            lists->cppCompilers = items;
        } else if (key == QLatin1String("debuggers")) {
            lists->debuggers = items;
        } else if (key == QLatin1String("clangd")) {
            lists->clangd = items;
        } else if (key == QLatin1String("clangFormat")) {
            lists->clangFormat = items;
        } else {
            lists->buildTools.insert(key, items);
        }
    }
}

// ---------------------------------------------------------------------------
// Kit master-detail
// ---------------------------------------------------------------------------

void SettingsDialog::loadKitForm(int kitIndex)
{
    LoadGuard guard(this);
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
        return;
    }

    const Settings::Kit& kit = list[kitIndex];
    kitId_->setText(kit.id);
    kitName_->setText(kit.name);
    kitEnabled_->setChecked(kit.enabled);
    kitIsDefault_->setChecked(kit.isDefault);
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
    const Settings::ToolchainLists& lists = state(currentScope_).toolchains;
    const auto suggest = [](QComboBox* combo, const QStringList& items) {
        const QString current = combo->currentText();
        QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItems(items);
        combo->setCurrentText(current);
    };
    suggest(kitCCompiler_, lists.cCompilers);
    suggest(kitCppCompiler_, lists.cppCompilers);
    suggest(kitDebugger_, lists.debuggers);
    suggest(kitClangd_, lists.clangd);
    suggest(kitClangFormat_, lists.clangFormat);
    suggest(kitToolCmake_, lists.buildTools.value(QStringLiteral("cmake")));
    suggest(kitToolNinja_, lists.buildTools.value(QStringLiteral("ninja")));
    suggest(kitToolGnuMake_, lists.buildTools.value(QStringLiteral("gnuMake")));
    suggest(kitToolMsbuild_, lists.buildTools.value(QStringLiteral("msbuild")));
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
    lineEndingWindows_->setCurrentText(
        prefs.lineEndingPlatform.value(QStringLiteral("windows"), QStringLiteral("CRLF")));
    lineEndingLinux_->setCurrentText(
        prefs.lineEndingPlatform.value(QStringLiteral("linux"), QStringLiteral("LF")));
    lineEndingMacos_->setCurrentText(
        prefs.lineEndingPlatform.value(QStringLiteral("macos"), QStringLiteral("LF")));
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
    prefs->lineEndingPlatform.insert(QStringLiteral("windows"),
                                      lineEndingWindows_->currentText());
    prefs->lineEndingPlatform.insert(QStringLiteral("linux"),
                                      lineEndingLinux_->currentText());
    prefs->lineEndingPlatform.insert(QStringLiteral("macos"),
                                      lineEndingMacos_->currentText());
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
    prefs->clangFormat.enabled = clangFormatEnabled_->isChecked();
    prefs->clangFormat.formatOnSave = clangFormatOnSave_->isChecked();
    prefs->clangFormat.style = clangFormatStyle_->currentText().trimmed();
    prefs->globalStyle = formattingGlobalStyle_->currentText().trimmed();
    prefs->defaultStyleContent = formattingDefaultStyleContent_->toPlainText();
}

// ---------------------------------------------------------------------------
// Commit / save
// ---------------------------------------------------------------------------

void SettingsDialog::commitUiToState()
{
    commitKitForm();
    ScopeState& current = state(currentScope_);
    commitDefaultsPage(&current.defaults);
    commitToolchainsPage(&current.toolchains);
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
    if (!write("toolchains.json", QJsonDocument(Settings::toJson(current.toolchains)))) {
        return false;
    }
    QJsonArray kitArray;
    for (const Settings::Kit& kit : current.kits) {
        kitArray.append(Settings::toJson(kit));
    }
    if (!write("kits.json", QJsonDocument(kitArray))) {
        return false;
    }
    if (!write("defaults.json", QJsonDocument(Settings::toJson(current.defaults)))) {
        return false;
    }
    if (!write("preferences.json", QJsonDocument(Settings::toJson(current.preferences)))) {
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
