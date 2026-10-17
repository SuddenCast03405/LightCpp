#pragma once

#include <QString>
#include <QStringList>

namespace LightCpp::App::GitCommandPolicy {

/// What a git command can do to the repository — and therefore how long the
/// watchdog may let it run before giving up on it.
///
/// One budget for everything is what R2-46 was: the same 60 s covered
/// `git status` (bounded by the size of its answer) and `git commit` (which
/// runs the user's hooks, and can legitimately take minutes). The command that
/// got killed was whichever happened to be running, and a commit killed in the
/// middle leaves `.git/index.lock` behind — after which *every* later git
/// command fails until someone deletes that file by hand.
enum class Kind {
    /// Reads the repository: status, diff, log, show, rev-parse. Bounded by the
    /// amount of output, so a minute is already generous.
    ReadOnly,
    /// Writes the index or the working tree: add, reset, commit, checkout. Can
    /// be slow (hooks, a large checkout) and can hold `.git/index.lock`.
    WritesRepository,
    /// Talks to a remote: fetch, pull, push, clone. The wall clock belongs to
    /// the network, not to git.
    Network,
};

/// Classify by the subcommand — the first argument that is not an option.
/// Options that take a value (`-c key=value`, `-C dir`) are skipped together
/// with that value, so `git -c core.quotepath=false push` is a push.
///
/// An unrecognised subcommand is treated as `WritesRepository`: giving an
/// unknown command the long budget only costs patience if it hangs, while
/// killing it early can cost a lock and a half-written index.
[[nodiscard]] Kind kindOf(const QStringList& arguments);

/// The watchdog budget for `kind`, in milliseconds.
[[nodiscard]] int timeoutMsFor(Kind kind);

/// True when a kill could have left `.git/index.lock` behind: everything that
/// is not a pure read. Used to decide whether the timeout message should tell
/// the user how to clear it.
[[nodiscard]] bool mayLeaveIndexLock(Kind kind);

/// The line reported when `title` was killed at `timeoutMs`. It names the
/// command, the budget it exceeded, and — when the command could have written
/// the repository — what the next failure will look like and how to clear it.
[[nodiscard]] QString timeoutMessage(const QString& title, Kind kind,
                                     int timeoutMs);

}  // namespace LightCpp::App::GitCommandPolicy
