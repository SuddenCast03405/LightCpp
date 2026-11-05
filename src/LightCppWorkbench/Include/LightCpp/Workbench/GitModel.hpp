// GitModel — branches, tags, remotes, stashes, blame, conflicts, and the
// exact command each operation needs.
//
// lightcpp.1.md §八 asks for twenty-six things. In 0.2.0 the IDE shelled out
// with `git status`, `git diff`, `git log` and `git commit`; everything else
// was "type it in the console". This module is the missing middle: the
// *reading* (`git branch -vv` and friends, parsed) and the *writing* (the
// argument list for each operation), with no process of its own.
//
// Two rules it encodes because they are easy to get wrong:
//
//   1. Destructive operations name their target explicitly. `git checkout -B
//      main` silently resets a branch; `git switch <name>` refuses when the
//      branch does not exist, which is the behaviour a GUI wants. The builder
//      returns the refusing form.
//   2. A merge/rebase/cherry-pick that stops on a conflict must leave the
//      repository in a state the IDE can name: `MERGE_HEAD` / `rebase-merge` /
//      `CHERRY_PICK_HEAD` under .git. `detectOperationState()` reads those
//      names, so the window can offer "continue / abort" instead of a raw
//      "git said something".
//
// Parsing is anchored on git's stable output:
//   `git branch -vv`   "* main  abc1234 [origin/main: ahead 2] subject"
//                       "  dev   def5678 subject"
//   `git tag -l`       one name per line (with --format, a separator we pick)
//   `git remote -v`    "origin\tgit@… (fetch)" / "(push)"
//   `git stash list`   "stash@{0}: WIP on main: abc1234 subject"
//   `git blame`        "abc1234 (Author 2024-01-01 12:00:00 +0000 12) code"
//   `git status`       already parsed by LightCppProject::parseStatus; the
//                      conflict codes are read here.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench {
namespace Git {

struct Branch {
    QString name;
    QString upstream;      ///< "origin/main", empty when none
    QString sha;
    QString subject;
    bool isCurrent = false;
    bool isRemote = false;  ///< lives under refs/remotes
    int ahead = 0;
    int behind = 0;
    bool detached = false;  ///< the synthetic "(HEAD detached at …)" entry
};

struct Tag {
    QString name;
    QString sha;
    QString subject;
    bool annotated = false;
};

struct Remote {
    QString name;
    QString fetchUrl;
    QString pushUrl;
};

struct StashEntry {
    int index = 0;
    QString message;    ///< "WIP on main: abc1234 subject"
    QString branch;     ///< parsed out of the message, when present
};

struct BlameLine {
    int line = 0;            ///< 1-based line in the file
    QString sha;
    QString author;
    QString date;            ///< "2024-01-01 12:00:00 +0000"
    QString summary;         ///< the commit subject (porcelain only)
    QString content;
    bool uncommitted = false;  ///< the 0000000… prefix
};

/// One hunk of a unified diff — what an inline gutter renders.
struct DiffHunk {
    int oldStart = 0;
    int oldCount = 0;
    int newStart = 0;
    int newCount = 0;
    /// Lines in order, each prefixed with ' ', '+' or '-'.
    QStringList lines;
};

struct DiffFile {
    QString oldPath;
    QString newPath;
    bool binary = false;
    int added = 0;
    int removed = 0;
    QList<DiffHunk> hunks;
};

/// A file git could not merge.
struct ConflictFile {
    QString path;
    /// 'A' added, 'D' deleted, 'U' modified — from the two status columns.
    QChar oursStatus;
    QChar theirsStatus;
    bool bothAdded = false;      ///< AA
    bool bothDeleted = false;    ///< DD
    bool deletedByUs = false;    ///< DU
    bool deletedByThem = false;  ///< UD
};

/// Where a stopped merge/rebase/cherry-pick is.
enum class OperationState {
    Clean,
    Merging,
    Rebasing,
    CherryPicking,
    Reverting,
    Bisecting,
};

[[nodiscard]] QString operationName(OperationState state);

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

[[nodiscard]] QList<Branch> parseBranches(const QString& text);
[[nodiscard]] QList<Tag> parseTags(const QString& text);
[[nodiscard]] QList<Remote> parseRemotes(const QString& text);
[[nodiscard]] QList<StashEntry> parseStashes(const QString& text);
[[nodiscard]] QList<BlameLine> parseBlame(const QString& text,
                                          bool porcelain = false);
[[nodiscard]] QList<DiffFile> parseUnifiedDiff(const QString& text);
/// The conflict files of a `git status --porcelain=v1` transcript.
[[nodiscard]] QList<ConflictFile> conflictsFromStatus(const QString& porcelain);
/// The state, decided from the names present in the .git directory
/// (`MERGE_HEAD`, `rebase-merge`, `CHERRY_PICK_HEAD`, `REVERT_HEAD`,
/// `BISECT_LOG`).
[[nodiscard]] OperationState detectOperationState(const QStringList& gitDirEntries);

/// The branch name a `## ` status header names, or empty.
[[nodiscard]] QString branchFromStatusHeader(const QString& headerLine);

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

/// Everything that can be done to a branch, as an argument list.
namespace Commands {

[[nodiscard]] QStringList listBranches();
[[nodiscard]] QStringList checkout(const QString& branch);
[[nodiscard]] QStringList createBranch(const QString& name, const QString& from);
/// `git switch -c` refuses a name that exists, which is what a "create"
/// button should do.
[[nodiscard]] QStringList createAndCheckout(const QString& name,
                                            const QString& from);
[[nodiscard]] QStringList deleteBranch(const QString& name, bool force);
[[nodiscard]] QStringList renameBranch(const QString& from, const QString& to);
[[nodiscard]] QStringList setUpstream(const QString& branch, const QString& remote);

[[nodiscard]] QStringList merge(const QString& branch, bool noFastForward,
                                bool squash);
[[nodiscard]] QStringList rebase(const QString& onto, bool interactive);
[[nodiscard]] QStringList cherryPick(const QString& commit);
[[nodiscard]] QStringList revert(const QString& commit);
[[nodiscard]] QStringList abortOperation(OperationState state);
[[nodiscard]] QStringList continueOperation(OperationState state);

[[nodiscard]] QStringList stashPush(const QString& message, bool includeUntracked);
[[nodiscard]] QStringList stashList();
[[nodiscard]] QStringList stashPop(int index);
[[nodiscard]] QStringList stashApply(int index);
[[nodiscard]] QStringList stashDrop(int index);

[[nodiscard]] QStringList fetch(const QString& remote, bool prune);
[[nodiscard]] QStringList pull(const QString& remote, const QString& branch,
                               bool rebase);
[[nodiscard]] QStringList push(const QString& remote, const QString& branch,
                               bool setUpstream, bool forceWithLease);

[[nodiscard]] QStringList listTags();
[[nodiscard]] QStringList createTag(const QString& name, const QString& message,
                                    const QString& commit);
[[nodiscard]] QStringList deleteTag(const QString& name, bool remote);

[[nodiscard]] QStringList listRemotes();
[[nodiscard]] QStringList addRemote(const QString& name, const QString& url);
[[nodiscard]] QStringList removeRemote(const QString& name);

[[nodiscard]] QStringList listSubmodules();
[[nodiscard]] QStringList updateSubmodules(bool init, bool recursive);
[[nodiscard]] QStringList listWorktrees();
[[nodiscard]] QStringList addWorktree(const QString& path, const QString& branch);

[[nodiscard]] QStringList blame(const QString& file, int startLine, int lineCount);
[[nodiscard]] QStringList diffFile(const QString& file, bool staged);

}  // namespace Commands

}  // namespace Git
}  // namespace LightCpp::Workbench
