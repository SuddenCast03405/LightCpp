// MarkupRender — Markdown and a reStructuredText subset, rendered to HTML.
//
// lightcpp.1.md §十一 asks for Markdown and RST preview and an "inline browser".
// Qt WebEngine is not a dependency of this project (and adding it would
// multiply the build), so the preview uses QTextBrowser, which understands a
// large but *not* complete HTML 4 subset. That constraint shapes the
// renderer: no CSS grid, no `<details>`, no flexbox — tables, headings,
// lists, `<pre>`, `<code>`, `<b>`, `<i>`, `<a>`, `<hr>` and `<blockquote>`,
// all of which QTextBrowser renders correctly.
//
// The renderer is deliberately strict about escaping: source text is HTML-
// escaped *first*, and only the constructs the parser recognises get tags
// back. That is what stops a README containing `<script>` (or a snippet of
// HTML the author wanted to show) from becoming markup — and it is also what
// makes a code fence behave.
//
// Doxygen parsing is separate: it reads `///` and `/** */` comments into the
// pieces a hover/quick-help panel shows (\brief, \param, \return, \note,
// \deprecated, \see).

#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench {

/// Render Markdown to an HTML fragment (no <html>/<body> wrapper).
[[nodiscard]] QString renderMarkdown(const QString& markdown);

/// Render a reStructuredText subset. Recognised: section titles with
/// over/underline, bullet and enumerated lists, literal blocks after `::`,
/// `.. code-block::`/`.. code::` directives, inline ``literal``, **bold**,
/// *italic*, ```interpreted text``` style roles, `field lists`, and simple
/// tables. Everything else stays a paragraph.
[[nodiscard]] QString renderRestructuredText(const QString& rst);

/// Pick a renderer by file extension: .md/.markdown → Markdown,
/// .rst/.rest → RST, anything else → the text in a <pre> block.
[[nodiscard]] QString renderForFile(const QString& path, const QString& text);

/// The stylesheet a QTextBrowser needs for the rendered fragments to look
/// like a document rather than a dump of tags.
[[nodiscard]] QString previewStyleSheet(bool darkBackground = false);

// ---------------------------------------------------------------------------
// Doxygen
// ---------------------------------------------------------------------------

/// A parsed `///` or `/** */` comment.
struct DoxygenComment {
    QString brief;
    QString details;
    QList<QPair<QString, QString>> params;   ///< (name, description)
    QString returns;
    QStringList notes;
    QStringList warnings;
    QStringList seeAlso;
    QString deprecated;
    QStringList authors;
    bool valid = false;
};

/// Parse the comment block that ends on `lineNumber` (0-based) of `lines`,
/// or the first block when `lineNumber` is -1. Empty/`valid == false` when
/// the lines above the position are not a Doxygen comment.
[[nodiscard]] DoxygenComment parseDoxygenAbove(const QStringList& lines,
                                               int lineNumber);

/// True when `text` looks like a Doxygen comment body (`\brief`, `@param`,
/// or a `///`/`/**` opener).
[[nodiscard]] bool looksLikeDoxygen(const QString& text);

/// The HTML a hover panel shows for a parsed comment.
[[nodiscard]] QString doxygenToHtml(const DoxygenComment& comment);

}  // namespace LightCpp::Workbench
