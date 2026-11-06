// QtPreview — the preview half of §十二.
//
// 0.2.0 could parse `.ui` and `.qrc` but had nothing to show: the note in the
// status document was "只有解析器没有预览器" (a parser and no previewer). This
// module is the previewer, and it deliberately does not try to be Qt Designer:
//
//   * A `.ui` form is rendered as an absolutely-positioned box per widget,
//     using the geometry stored in the file. That is the same information
//     Designer draws, and it is honest about what it does not know — a widget
//     with no geometry gets a labelled row instead of a zero-sized box.
//   * A `.qrc` is rendered as the prefix tree Qt builds at run time, with the
//     `:/prefix/alias` path next to each file and a marker for entries that do
//     not exist on disk (the most common reason "the icon is missing").
//   * QML has no renderer here (that needs a QML engine and a scene graph),
//     so it gets a structure view: the object tree with its ids and the
//     properties that are set. Claiming a preview would be worse than
//     showing the tree.
//
// All three produce HTML for a QTextBrowser, which the IDE already links.

#pragma once

#include <QString>

#include "LightCpp/Workbench/QtProjectFiles.hpp"

namespace LightCpp::Workbench::Qt {

/// Render a `.ui` form as HTML. `formName` is shown in the header.
[[nodiscard]] QString uiPreviewHtml(const UiForm& form, const QString& formName = QString());

/// Render a `.qrc` as HTML: the prefix tree, each entry's `:/` path, and a
/// marker for files that are missing.
[[nodiscard]] QString qrcPreviewHtml(const QrcFile& qrc, const QString& qrcName = QString());

/// Render the object tree of a QML document as HTML.
[[nodiscard]] QString qmlStructureHtml(const QString& qmlText,
                                       const QString& fileName = QString());

/// The QML document's object tree, as text ("Item  (id: root)" per line with
/// two spaces of indent per level). Exposed separately so it can be tested
/// without HTML.
[[nodiscard]] QStringList qmlObjectTree(const QString& qmlText);

/// True when the HTML preview can be produced for `path` (`.ui` / `.qrc` /
/// `.qml` / `.qmltypes`).
[[nodiscard]] bool hasPreview(const QString& path);

}  // namespace LightCpp::Workbench::Qt
