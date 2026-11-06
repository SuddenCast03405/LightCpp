// DocsCatalog — the documentation half of §十一.
//
// The IDE already renders Markdown and RST and parses Doxygen comments. What
// was missing is the *catalogue*: which document answers "what does this
// keyword do", "where is the standard", "which page documents this plugin".
//
// Three design rules, because a documentation browser that guesses is worse
// than a link:
//
//   * A symbol that is not in the offline table still gets a link — to the
//     cppreference search page for it — rather than a fabricated summary.
//   * The offline summaries cover the C++ keywords and the most-used standard
//     library names. They are written as one sentence each and say what the
//     thing *is*, not what a particular implementation does with it.
//   * Third-party documentation is a registry of URLs, not scraped content:
//     opening the project's own site is correct, caching it here would go
//     stale silently.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench::Docs {

/// One entry of the documentation browser.
struct DocEntry {
    QString title;
    QString url;
    QString summary;
    /// "keyword", "standard", "library", "plugin", "project".
    QString kind;

    [[nodiscard]] bool isValid() const { return !title.isEmpty() || !url.isEmpty(); }
};

/// Quick help for a word: the symbol under the caret, as the editor sees it.
/// An unknown symbol yields an entry whose `url` is the cppreference search
/// page and whose `summary` is empty — never a made-up description.
[[nodiscard]] DocEntry quickHelp(const QString& symbol);

/// The URLs for the C++ standard documents, newest first, plus the C standard
/// and the core guidelines.
[[nodiscard]] QList<DocEntry> standardDocuments();

/// The libraries a C++ project most often needs to look up, with their own
/// documentation sites.
[[nodiscard]] QList<DocEntry> libraryDocuments();

/// One entry per plugin id, pointing into the repository's plugin reference.
/// `pluginIds` comes from the plugin manager; the URL anchor is the id, which
/// is what docs/PLUGINS.md uses as a heading anchor.
[[nodiscard]] QList<DocEntry> pluginDocuments(const QStringList& pluginIds);

/// The cppreference URL for `symbol`: an exact page for the names in the
/// table, a search URL otherwise.
[[nodiscard]] QString cppreferenceUrl(const QString& symbol);

/// True when `word` is a C++ keyword the offline table knows.
[[nodiscard]] bool isKnownKeyword(const QString& word);

/// The offline summary for `word`, or an empty string.
[[nodiscard]] QString keywordSummary(const QString& word);

/// Every keyword the table covers — the "C++ keywords" index page.
[[nodiscard]] QStringList keywords();

/// Search the whole catalogue (keywords, standards, libraries) for `query`.
/// Case-insensitive substring over the title and the summary; an empty query
/// returns the index (standards and libraries).
[[nodiscard]] QList<DocEntry> search(const QList<DocEntry>& libraries,
                                     const QString& query);

}  // namespace LightCpp::Workbench::Docs
