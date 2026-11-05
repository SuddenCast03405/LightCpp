#pragma once

#include <QByteArray>
#include <QChar>
#include <QList>
#include <QString>

/// Parsing for `git status --porcelain=v1`. Only the reading is here; running
/// git is the window's business.
///
/// Porcelain output is explicitly the stable, script-facing format — unlike
/// the human-readable `git status`, it is not translated and its layout does
/// not change between versions, which is exactly why it is parsed rather than
/// the pretty form.
namespace LightCpp::Project {

/// One changed path.
struct GitEntry {
    /// Staged (index) status — the X of `XY path`. ' ' means nothing staged.
    QChar indexStatus = u' ';
    /// Working-tree status — the Y.
    QChar workTreeStatus = u' ';
    QString path;          // repository-relative, '/' separated
    QString originalPath;  // the "from" side of a rename or copy, else empty

    [[nodiscard]] bool isUntracked() const noexcept
    {
        return indexStatus == u'?' && workTreeStatus == u'?';
    }
    [[nodiscard]] bool isIgnored() const noexcept
    {
        return indexStatus == u'!' && workTreeStatus == u'!';
    }
    [[nodiscard]] bool hasStagedChanges() const noexcept
    {
        return !isUntracked() && !isIgnored() && indexStatus != u' ';
    }
    [[nodiscard]] bool hasUnstagedChanges() const noexcept
    {
        return !isUntracked() && !isIgnored() && workTreeStatus != u' ';
    }
    /// The two status columns as git prints them, e.g. "M ", " M", "??".
    [[nodiscard]] QString code() const
    {
        return QString(QChar(indexStatus)) + QChar(workTreeStatus);
    }
};

/// The `## ` header line plus every entry.
struct GitStatusSummary {
    QString branch;    // "main"; empty for a detached HEAD
    QString upstream;  // "origin/main"; empty when the branch has none
    int ahead = 0;
    int behind = 0;
    bool detached = false;
    QList<GitEntry> entries;

    [[nodiscard]] bool isEmpty() const noexcept { return entries.isEmpty(); }
    [[nodiscard]] int stagedCount() const;
    [[nodiscard]] int unstagedCount() const;
    [[nodiscard]] int untrackedCount() const;

    /// Compact label for the status bar: the branch, then how far it has
    /// drifted from its upstream, then a dot when the tree is dirty.
    /// e.g. "main ↑1 ●"
    [[nodiscard]] QString label() const;
};

/// Parses `git status --porcelain=v1 -b --untracked-files=all` output.
[[nodiscard]] GitStatusSummary parseGitStatus(const QByteArray& porcelainOutput);

/// One-line English summary for the log pane, or "working tree clean".
[[nodiscard]] QString describeGitStatus(const GitStatusSummary& summary);

}  // namespace LightCpp::Project
