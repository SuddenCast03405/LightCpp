// ExternalTools — lightcpp.1.md §九 `tool.external`, the part that is logic.
//
// An external tool is a command the IDE did not ship, described by the user
// and pointed at whatever they are looking at. The interesting part is the
// template expansion: `${file}` has to become a path that survives being
// handed to a shell, `${selection}` has to become the selection, and an
// unknown `${...}` has to be left alone rather than silently eaten.
//
// The rules here are deliberately the conservative ones:
//
//   * Only the placeholders listed in `placeholderNames()` are substituted.
//     Anything else is passed through verbatim, because a template is often
//     a shell fragment (`$HOME`, `$?`) and eating those would break it.
//   * A substituted path is quoted for the shell that will run it. On POSIX
//     that is single quotes with `'\''` escaping; on Windows that is double
//     quotes with `""` escaping. The caller says which.
//   * A placeholder with no value expands to the empty string, and the
//     caller is told, so it can refuse to run rather than run `grep "" file`.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench::ExternalTools {

/// Which shell will see the expanded command. Quoting differs, and getting
/// it wrong is the difference between "works" and "mangled on any path with
/// a space".
enum class Shell {
    Posix,   ///< /bin/sh -c
    Windows, ///< cmd.exe /c
};

/// The values a template can refer to.
struct Context {
    QString file;           ///< the active file's path, may be empty
    QString projectRoot;    ///< the project root, may be empty
    QString buildDir;       ///< the build directory, may be empty
    QString selection;      ///< the selected text, may be empty
};

/// The placeholders this module substitutes, in the order the help text
/// should list them. Anything not here is left untouched.
[[nodiscard]] QStringList placeholderNames();

/// Quote one value for `shell`. A value that needs no quoting is returned
/// unchanged, which keeps the common case readable in the Output panel.
[[nodiscard]] QString quoteForShell(const QString& value, Shell shell);

/// Expand `templateText` against `context`. Placeholder values are quoted
/// for `shell`; `${selection}` is *not* quoted, because a selection is
/// usually meant as several arguments (a list of files, say) and quoting it
/// would turn it into one argument containing newlines.
///
/// `missing` collects the names of placeholders that had no value, so the
/// caller can decide whether to run anyway.
[[nodiscard]] QString expand(const QString& templateText, const Context& context,
                             Shell shell, QStringList* missing = nullptr);

/// The file's directory, for `${fileDir}`; empty when there is no file.
[[nodiscard]] QString directoryOf(const QString& filePath);

/// The file's name without a directory, for `${fileName}`.
[[nodiscard]] QString fileNameOf(const QString& filePath);

/// A stable id for a tool that has none: the name lowercased with spaces
/// turned into dashes, made unique against `existing` by appending a number.
/// Stable matters: the id is how a remembered "last run" is looked up.
[[nodiscard]] QString makeId(const QString& name, const QStringList& existing);

/// A one-line description of a tool for a menu entry: "clang-format --dry-run
/// ${file}" trimmed to fit. Never empty for a tool with a command; an empty
/// string means the tool cannot run (no command).
[[nodiscard]] QString describe(const QString& command, const QString& arguments,
                               int maxLength = 60);

/// The working directory a tool should run in: its own template when set,
/// else the project root, else the file's directory, else empty (the
/// process inherits the IDE's).
[[nodiscard]] QString workingDirectoryFor(const QString& templateText,
                                          const Context& context, Shell shell);

}  // namespace LightCpp::Workbench::ExternalTools
