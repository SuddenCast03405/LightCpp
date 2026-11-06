// GitExtra — the Git features that are a *reading* of git's output or a
// *rewriting* of the working tree, and that GitModel does not cover
// (lightcpp.1.md §八: blame view, conflict resolution, commit template, GPG
// signing, authentication, submodules, worktrees, remote management).
//
// The three pieces here are pure:
//
//   * `groupBlame` folds git's per-line blame into the contiguous blocks a
//     blame view actually shows. A view that iterates lines instead of blocks
//     repaints the author column for every line of the same commit, and looks
//     like it is broken on a 4000-line file.
//   * `parseConflictFile` reads the `<<<<<<< / ||||||| / ======= / >>>>>>>`
//     markers into a hunk model, and `resolve` writes the file back with the
//     user's per-hunk choice. Parsing must handle the base section being
//     absent (`merge.conflictStyle=merge` omits `|||||||`), nested markers in
//     a conflicted *string*, and CRLF files.
//   * The commit/signing/auth helpers build the exact argument lists, because
//     `git commit -S` with no key and `git -c user.signingkey=` are different
//     operations and a GUI must not guess.

#pragma once

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include "LightCpp/Workbench/GitModel.hpp"

namespace LightCpp::Workbench::Git {

// ---------------------------------------------------------------------------
// Blame view
// ---------------------------------------------------------------------------

/// A run of consecutive lines attributed to one commit — one row of the blame
/// gutter and one row of the blame table.
struct BlameBlock {
    QString sha;
    QString shortSha;
    QString author;
    QString date;
    QString summary;
    int firstLine = 1;   ///< 1-based, inclusive
    int lineCount = 0;
    bool uncommitted = false;
    QStringList content;  ///< the lines themselves, for a hover preview
};

/// Fold a per-line blame into blocks. A block ends when the commit changes.
/// The same commit appearing twice (git shows it per line) yields two blocks
/// only when they are not adjacent — the caller renders them in order.
[[nodiscard]] QList<BlameBlock> groupBlame(const QList<BlameLine>& lines);

/// The colour index for a commit: a stable, small integer used to tint the
/// gutter so that two authors' blocks are visually distinct without a colour
/// map per repository.
[[nodiscard]] int blameColourIndex(const QString& sha);

// ---------------------------------------------------------------------------
// Three-way merge
// ---------------------------------------------------------------------------

/// Which side of a conflict to keep.
enum class MergeChoice {
    Ours = 0,
    Theirs,
    Both,        ///< ours then theirs
    BothReversed,///< theirs then ours
    Base,        ///< the common ancestor, when the file carried it
};

[[nodiscard]] QString mergeChoiceName(MergeChoice choice);
[[nodiscard]] MergeChoice mergeChoiceFromName(const QString& name);

/// One conflicted region.
struct MergeHunk {
    int startLine = 0;        ///< 0-based line of the `<<<<<<<` marker
    int endLine = 0;          ///< 0-based line of the `>>>>>>>` marker
    QString oursLabel;
    QString theirsLabel;
    QString baseLabel;
    QStringList baseLines;
    QStringList ourLines;
    QStringList theirLines;
    bool hasBase = false;
};

/// A file with conflict markers, split into the text around the hunks.
struct MergeFile {
    QStringList prefix;   ///< lines before the first hunk
    QList<MergeHunk> hunks;
    /// Text between hunk i and hunk i+1 (index i). Kept apart from the hunks
    /// so that resolving one side never moves the surrounding code.
    QStringList betweenTexts;
    QStringList suffix;   ///< lines after the last hunk
    bool hasConflicts = false;
    QString newline = QStringLiteral("\n");
};

/// Parse a working-tree file that git left with conflict markers.
///
/// A file with no markers is not a conflict: it comes back with
/// `hasConflicts == false` and every line in `suffix`, so the caller can show
/// "this file is already resolved" instead of an empty editor.
[[nodiscard]] MergeFile parseConflictFile(const QString& text);

/// The text of one hunk resolved to `choice`.
[[nodiscard]] QStringList resolveHunk(const MergeHunk& hunk, MergeChoice choice);

/// Rebuild the whole file with one choice per hunk. `choices` must have one
/// entry per hunk; a short list resolves the rest as Ours (the safe default
/// for a "keep my work" action).
[[nodiscard]] QString resolveConflictFile(const MergeFile& file,
                                          const QList<MergeChoice>& choices);

/// Resolve every hunk with the same choice (the "accept all ours" button).
[[nodiscard]] QString resolveAll(const MergeFile& file, MergeChoice choice);

/// Count the hunks, for the status line.
[[nodiscard]] int conflictCount(const MergeFile& file);

// ---------------------------------------------------------------------------
// Commit message and signing
// ---------------------------------------------------------------------------

/// Fill a commit template. `%branch%`-style placeholders are not a git
/// convention, so only the ones git itself documents (`git config
/// commit.template` files are plain text plus `#` comments) are honoured:
///   `{branch}` and `{files}` are the two the IDE adds.
/// Comment lines starting with `#` are kept — git strips them on commit and
/// they are where the template's instructions live.
[[nodiscard]] QString applyCommitTemplate(const QString& templateText,
                                          const QString& branch,
                                          const QStringList& files);

/// `git commit` arguments for `message`.
///
/// `-S` is only added when signing is on; `--gpg-sign=<key>` only when a key
/// is configured. `--signoff` is independent. `--cleanup=strip` is always
/// added: the template's `#` comments must not end up in the message.
[[nodiscard]] QStringList commitArguments(const QString& message,
                                          bool amend,
                                          bool signOff,
                                          bool gpgSign,
                                          const QString& gpgKey);

/// How this repository is configured to sign.
struct SigningConfig {
    bool enabled = false;
    QString keyId;
    QString format;   ///< "openpgp" or "ssh"
    QString program;  ///< gpg.program, when set
};

/// Parse the output of `git config --get-regexp '^(commit|gpg|user)\.'`.
/// Only the keys that decide signing are read; everything else is ignored, so
/// an unusual-but-valid config does not read as "signing off".
[[nodiscard]] SigningConfig parseSigningConfig(const QString& configText);

/// The `git config` queries that feed parseSigningConfig().
[[nodiscard]] QStringList signingConfigQueries();

/// Credential-helper / transport probes for the settings page: what git will
/// use for HTTPS and SSH, so the user can see it rather than guess.
[[nodiscard]] QStringList authenticationQueries();

// ---------------------------------------------------------------------------
// Submodules and worktrees
// ---------------------------------------------------------------------------

/// One row of `git submodule status`.
struct SubmoduleEntry {
    QString sha;
    QString path;
    QString describe;
    QChar status;  ///< ' ' in sync, '-' uninitialised, '+' checked-out differs, 'U' conflict
};

[[nodiscard]] QList<SubmoduleEntry> parseSubmoduleStatus(const QString& text);

/// One row of `git worktree list --porcelain`.
struct WorktreeEntry {
    QString path;
    QString head;
    QString branch;
    bool bare = false;
    bool detached = false;
    bool locked = false;
    bool prunable = false;

    /// `git worktree list --porcelain` prints the main working tree first.
    /// It used to be identified by "has no branch", which is wrong the moment
    /// the main worktree is on a detached HEAD — a common state while
    /// bisecting — so the caller now says which index it is.
    [[nodiscard]] bool isMain(int index) const { return index == 0; }
};

[[nodiscard]] QList<WorktreeEntry> parseWorktreeList(const QString& text);

/// `git worktree remove` refuses a dirty tree without `--force`; the builder
/// takes the flag explicitly so the UI can ask first.
[[nodiscard]] QStringList removeWorktree(const QString& path, bool force);

/// `git submodule add <url> <path>` and the follow-up init/update.
[[nodiscard]] QStringList addSubmodule(const QString& url, const QString& path,
                                       bool shallow);

/// `git remote set-url` / `git remote rename` — the two remote operations the
/// 0.2.0 Remotes tab could list but not change.
[[nodiscard]] QStringList setRemoteUrl(const QString& name, const QString& url,
                                       bool push);
[[nodiscard]] QStringList renameRemote(const QString& from, const QString& to);

}  // namespace LightCpp::Workbench::Git
