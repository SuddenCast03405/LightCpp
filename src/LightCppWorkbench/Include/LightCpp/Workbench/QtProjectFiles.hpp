// QtProjectFiles — `.pro` / `.pri`, `.ui` and `.qrc`, read.
//
// lightcpp.1.md §十二 asks for Qt project recognition with `.pro`/`.pri`
// parsing, `.ui` preview, `.qrc` resource preview, QML highlighting, and
// qmake/CMake build support. The repository already had a 421-line
// `QtCppBackend` that parsed `.pro` — but it was compiled as a *plugin* and
// never reached the IDE's project model, so opening a `.pro` did nothing.
//
// This is the same information in the core, where the IDE can use it:
//
//   qmake    variables (SOURCES/HEADERS/FORMS/RESOURCES/TARGET/QT/CONFIG/
//            INCLUDEPATH/DEFINES/LIBS/SUBDIRS), `$$VAR`, `+=`, `-=`, `*=`,
//            `#` comments, and `include(other.pri)` — the parts of the
//            language a project actually uses. Not a full qmake evaluator:
//            it does not run functions, and it says so by exposing `unknown`
//            expressions rather than pretending.
//   .ui      QDomDocument: the widget tree with class, name, geometry and
//            signal/slot connections — enough to draw a preview outline.
//   .qrc     QDomDocument: prefix/alias/file entries with their resolved
//            absolute paths, which is what a resource browser needs.
//
// QML is handled as a syntax definition (keywords + built-in types) because
// the highlighter is a QSyntaxHighlighter rule set, not a parser.

#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench {

struct QmakeProject {
    QString path;                 ///< the .pro this came from
    QString target;               ///< TARGET, else the file's base name
    QString templateType;         ///< "app" / "lib" / "subdirs"
    QStringList sources;
    QStringList headers;
    QStringList forms;            ///< .ui files
    QStringList resources;        ///< .qrc files
    QStringList includePaths;
    QStringList defines;
    QStringList libraries;
    QStringList qtModules;        ///< "core" "gui" "widgets" ...
    QStringList config;           ///< "debug" "release" "c++17" ...
    QStringList subdirs;
    QStringList includes;         ///< included .pri files, resolved
    /// Expressions this reader could not evaluate (`$$system(...)`, `$$files()`).
    QStringList unresolvedExpressions;
    bool valid = false;

    /// True when the project needs Qt Widgets (drives the build flags).
    [[nodiscard]] bool usesWidgets() const;
    /// True when CONFIG names a C++ standard (c++11..c++23).
    [[nodiscard]] QString cxxStandard() const;
};

/// Parse a `.pro` or `.pri`. `path` resolves `include()` relative to it.
[[nodiscard]] QmakeProject parseQmakeProject(const QString& path);
/// Parse text, resolving includes against `baseDirectory`.
[[nodiscard]] QmakeProject parseQmakeText(const QString& text,
                                          const QString& baseDirectory,
                                          int depth = 0);

/// One widget of a `.ui` form.
struct UiWidget {
    QString className;   ///< "QPushButton"
    QString name;        ///< "okButton"
    QString parent;      ///< the parent widget's name
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    QString text;        ///< the `text` property, when set
};

struct UiForm {
    QString className;    ///< the form's own class ("MainWindow")
    QList<UiWidget> widgets;
    QStringList connections;  ///< "clicked() -> onOk()"
    bool valid = false;

    /// Widgets of `className`, in document order.
    [[nodiscard]] QList<UiWidget> ofClass(const QString& className) const;
};

/// Parse a `.ui` file's XML.
[[nodiscard]] UiForm parseUiForm(const QString& xml);

/// One entry of a `.qrc`.
struct QrcResource {
    QString prefix;    ///< "/icons"
    QString alias;     ///< the file name as Qt sees it (default: base name)
    QString path;      ///< the file path as written in the .qrc
    QString absolutePath;  ///< resolved against the .qrc's directory
    bool exists = false;
};

struct QrcFile {
    QList<QrcResource> resources;
    bool valid = false;

    /// The path Qt uses at run time: ":/icons/open.png".
    [[nodiscard]] QString resourcePath(const QrcResource& resource) const;
};

/// Parse a `.qrc` file's XML; `qrcPath` resolves the entries.
[[nodiscard]] QrcFile parseQrc(const QString& xml, const QString& qrcPath);

/// The QML keyword set the highlighter needs (language keywords, built-in
/// types, and the ids of the QtQuick modules' most-used attached types).
[[nodiscard]] QStringList qmlKeywords();
[[nodiscard]] QStringList qmlBuiltinTypes();

/// True when `path` is a Qt project file this module understands.
[[nodiscard]] bool isQtProjectFile(const QString& path);

/// The qmake invocation that configures `projectFile` into `buildDirectory`.
[[nodiscard]] QStringList qmakeArguments(const QString& projectFile,
                                         const QString& buildDirectory,
                                         const QString& configuration);

}  // namespace LightCpp::Workbench
