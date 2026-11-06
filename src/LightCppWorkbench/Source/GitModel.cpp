// GitModel — implementation. See the header for the formats.

#include "LightCpp/Workbench/GitModel.hpp"

#include <QDir>
#include <QRegularExpression>

namespace LightCpp::Workbench {
namespace Git {
namespace {

QString normalize(QString path)
{
    path = path.trimmed();
    if (path.size() >= 3 && path.at(1) == QLatin1Char(':')
        && (path.at(2) == QLatin1Char('\\') || path.at(2) == QLatin1Char('/'))) {
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        return path;
    }
    return QDir::fromNativeSeparators(path);
}

/// "ahead 2, behind 1" / "ahead 2" / "gone" — the bracket body of branch -vv.
void parseTracking(const QString& body, Branch& branch)
{
    static const QRegularExpression ahead(QStringLiteral("ahead (\\d+)"));
    static const QRegularExpression behind(QStringLiteral("behind (\\d+)"));
    if (const auto match = ahead.match(body); match.hasMatch()) {
        branch.ahead = match.captured(1).toInt();
    }
    if (const auto match = behind.match(body); match.hasMatch()) {
        branch.behind = match.captured(1).toInt();
    }
}

}  // namespace

QString operationName(OperationState state)
{
    switch (state) {
    case OperationState::Merging:      return QStringLiteral("merge");
    case OperationState::Rebasing:     return QStringLiteral("rebase");
    case OperationState::CherryPicking: return QStringLiteral("cherry-pick");
    case OperationState::Reverting:    return QStringLiteral("revert");
    case OperationState::Bisecting:    return QStringLiteral("bisect");
    case OperationState::Clean:        break;
    }
    return QStringLiteral("clean");
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

QList<Branch> parseBranches(const QString& text)
{
    // "* main                abc1234 [origin/main: ahead 2, behind 1] Subject"
    // "  dev                 def5678 Subject"
    // "  remotes/origin/x    aaa1111 Subject"   (git branch -a)
    // "* (HEAD detached at abc1234) abc1234 Subject"
    QList<Branch> branches;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        if (raw.trimmed().isEmpty()) {
            continue;
        }
        Branch branch;
        QString rest = raw;
        if (rest.startsWith(QLatin1Char('*'))) {
            branch.isCurrent = true;
            rest = rest.mid(1);
        }
        rest = rest.trimmed();
        // A detached HEAD is spelled "(HEAD detached at abc1234) abc1234 …";
        // the parenthesised part is the name and contains spaces, so it is
        // taken before the whitespace split.
        if (rest.startsWith(QLatin1String("(HEAD detached"))) {
            const qsizetype close = rest.indexOf(QLatin1Char(')'));
            if (close > 0) {
                branch.name = rest.mid(1, close - 1);
                branch.detached = true;
                const QString tail = rest.mid(close + 1).trimmed();
                const QStringList tailParts = tail.split(QRegularExpression(QStringLiteral("\\s+")),
                                                         Qt::SkipEmptyParts);
                branch.sha = tailParts.value(0);
                branch.subject = tail.mid(tailParts.value(0).size()).trimmed();
                branches.append(branch);
                continue;
            }
        }
        const QStringList parts = rest.split(QRegularExpression(QStringLiteral("\\s+")),
                                             Qt::SkipEmptyParts);
        if (parts.size() < 2) {
            continue;
        }
        branch.name = parts.at(0);
        branch.sha = parts.at(1);
        if (branch.name.startsWith(QLatin1String("remotes/"))) {
            branch.isRemote = true;
            branch.name = branch.name.mid(8);
        }

        // The upstream is the bracketed token, and it is the only part that
        // can contain a space (", behind 1") — so read it by brackets.
        const qsizetype open = rest.indexOf(QLatin1Char('['));
        const qsizetype close = rest.indexOf(QLatin1Char(']'), open + 1);
        QString subjectStart = rest;
        if (open >= 0 && close > open) {
            const QString body = rest.mid(open + 1, close - open - 1);
            const qsizetype colon = body.indexOf(QLatin1Char(':'));
            branch.upstream = (colon > 0 ? body.left(colon) : body).trimmed();
            parseTracking(body, branch);
            subjectStart = rest.mid(close + 1);
        }
        branch.subject = subjectStart.trimmed();
        branches.append(branch);
    }
    return branches;
}

QList<Tag> parseTags(const QString& text)
{
    // `git tag -l --format='%(refname:short)%09%(objectname:short)%09%(contents:subject)'`
    QList<Tag> tags;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        if (raw.trimmed().isEmpty()) {
            continue;
        }
        Tag tag;
        const QStringList fields = raw.split(QLatin1Char('\t'));
        tag.name = fields.value(0).trimmed();
        tag.sha = fields.value(1).trimmed();
        tag.subject = fields.value(2).trimmed();
        // Without --format it is one bare name per line; that is enough for
        // the list, and `annotated` stays false because there is no evidence.
        tag.annotated = fields.size() >= 3 && !tag.subject.isEmpty();
        if (!tag.name.isEmpty()) {
            tags.append(tag);
        }
    }
    return tags;
}

QList<Remote> parseRemotes(const QString& text)
{
    // "origin\tgit@github.com:x/y.git (fetch)"
    QList<Remote> remotes;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QStringList fields = line.split(QLatin1Char('\t'));
        if (fields.size() < 2) {
            continue;
        }
        const QString name = fields.at(0).trimmed();
        QString url = fields.at(1).trimmed();
        const bool isPush = url.endsWith(QLatin1String("(push)"));
        if (url.endsWith(QLatin1String("(fetch)")) || isPush) {
            url = url.left(url.lastIndexOf(QLatin1Char('('))).trimmed();
        }
        Remote* existing = nullptr;
        for (Remote& remote : remotes) {
            if (remote.name == name) {
                existing = &remote;
                break;
            }
        }
        if (existing == nullptr) {
            Remote remote;
            remote.name = name;
            if (isPush) {
                remote.pushUrl = url;
            } else {
                remote.fetchUrl = url;
            }
            remotes.append(remote);
        } else if (isPush) {
            existing->pushUrl = url;
        } else {
            existing->fetchUrl = url;
        }
    }
    return remotes;
}

QList<StashEntry> parseStashes(const QString& text)
{
    // "stash@{0}: WIP on main: abc1234 Subject"
    // "stash@{1}: On dev: message the user typed"
    static const QRegularExpression pattern(
        QStringLiteral("^stash@\\{(\\d+)\\}:\\s*(.*)$"));
    QList<StashEntry> entries;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const auto match = pattern.match(raw.trimmed());
        if (!match.hasMatch()) {
            continue;
        }
        StashEntry entry;
        entry.index = match.captured(1).toInt();
        entry.message = match.captured(2);
        // Both forms put the branch between "on "/"On " and the next ':'.
        static const QRegularExpression branchPattern(
            QStringLiteral("^(?:WIP on|On)\\s+([^:]+):"));
        if (const auto named = branchPattern.match(entry.message); named.hasMatch()) {
            entry.branch = named.captured(1).trimmed();
        }
        entries.append(entry);
    }
    return entries;
}

QList<BlameLine> parseBlame(const QString& text, bool porcelain)
{
    QList<BlameLine> lines;
    if (porcelain) {
        // "<sha> <orig-line> <final-line> <group-size>" then header key/values,
        // then "\t<content>".
        BlameLine current;
        bool inHeader = false;
        for (const QString& raw : text.split(QLatin1Char('\n'))) {
            if (raw.startsWith(QLatin1Char('\t'))) {
                current.content = raw.mid(1);
                current.uncommitted = current.sha.startsWith(QLatin1String("0000000"));
                lines.append(current);
                current = BlameLine{};
                inHeader = false;
                continue;
            }
            static const QRegularExpression head(
                QStringLiteral("^([0-9a-f]{40})\\s+\\d+\\s+(\\d+)(?:\\s+\\d+)?$"));
            if (const auto match = head.match(raw); match.hasMatch()) {
                current.sha = match.captured(1);
                current.line = match.captured(2).toInt();
                inHeader = true;
                continue;
            }
            if (!inHeader) {
                continue;
            }
            if (raw.startsWith(QLatin1String("author "))) {
                current.author = raw.mid(7);
            } else if (raw.startsWith(QLatin1String("author-time "))) {
                // Kept as the raw epoch in date; the UI formats it.
                current.date = raw.mid(12);
            } else if (raw.startsWith(QLatin1String("summary "))) {
                current.summary = raw.mid(8);
            }
        }
        return lines;
    }

    // "abc1234 (Author Name 2024-01-01 12:00:00 +0000 12) code"
    static const QRegularExpression pattern(
        QStringLiteral("^([0-9a-f^]+)\\s*\\((.+?)\\s+(\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d [+-]\\d{4})\\s+(\\d+)\\)\\s?(.*)$"));
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const auto match = pattern.match(raw);
        if (!match.hasMatch()) {
            continue;
        }
        BlameLine line;
        line.sha = match.captured(1);
        line.author = match.captured(2).trimmed();
        line.date = match.captured(3);
        line.line = match.captured(4).toInt();
        line.content = match.captured(5);
        line.uncommitted = line.sha.startsWith(QLatin1String("0000000"));
        lines.append(line);
    }
    return lines;
}

QList<DiffFile> parseUnifiedDiff(const QString& text)
{
    QList<DiffFile> files;
    DiffFile current;
    DiffHunk hunk;
    bool inHunk = false;

    const auto flushHunk = [&] {
        if (inHunk) {
            current.hunks << hunk;
            hunk = DiffHunk{};
            inHunk = false;
        }
    };
    const auto flushFile = [&] {
        flushHunk();
        if (!current.oldPath.isEmpty() || !current.newPath.isEmpty()) {
            files << current;
        }
        current = DiffFile{};
    };

    static const QRegularExpression header(
        QStringLiteral("^@@ -(\\d+)(?:,(\\d+))? \\+(\\d+)(?:,(\\d+))? @@"));
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        if (raw.startsWith(QLatin1String("diff --git "))) {
            flushFile();
            // "diff --git a/x b/y" — the paths may contain spaces, so the
            // authoritative names come from the ---/+++ lines below.
            continue;
        }
        if (raw.startsWith(QLatin1String("--- "))) {
            const QString path = raw.mid(4).trimmed();
            current.oldPath = (path == QLatin1String("/dev/null"))
                                  ? QString()
                                  : normalize(QString(path).remove(0, path.startsWith(QLatin1String("a/")) ? 2 : 0));
            continue;
        }
        if (raw.startsWith(QLatin1String("+++ "))) {
            const QString path = raw.mid(4).trimmed();
            current.newPath = (path == QLatin1String("/dev/null"))
                                  ? QString()
                                  : normalize(QString(path).remove(0, path.startsWith(QLatin1String("b/")) ? 2 : 0));
            continue;
        }
        if (raw.startsWith(QLatin1String("Binary files "))) {
            current.binary = true;
            continue;
        }
        if (const auto match = header.match(raw); match.hasMatch()) {
            flushHunk();
            hunk.oldStart = match.captured(1).toInt();
            hunk.oldCount = match.captured(2).isEmpty() ? 1 : match.captured(2).toInt();
            hunk.newStart = match.captured(3).toInt();
            hunk.newCount = match.captured(4).isEmpty() ? 1 : match.captured(4).toInt();
            inHunk = true;
            continue;
        }
        if (!inHunk) {
            continue;
        }
        if (raw.startsWith(QLatin1Char('+')) && !raw.startsWith(QLatin1String("+++"))) {
            hunk.lines << raw;
            ++current.added;
            continue;
        }
        if (raw.startsWith(QLatin1Char('-')) && !raw.startsWith(QLatin1String("---"))) {
            hunk.lines << raw;
            ++current.removed;
            continue;
        }
        if (raw.startsWith(QLatin1Char(' '))) {
            hunk.lines << raw;
            continue;
        }
        if (raw.isEmpty()) {
            // A blank line ends a hunk: unified diff never represents an
            // empty context line without its leading space.
            flushHunk();
            continue;
        }
        if (raw.startsWith(QLatin1String("\\ No newline"))) {
            continue;
        }
        // Anything else ends the hunk (a new file header, a signature).
        flushHunk();
    }
    flushFile();
    return files;
}

QList<ConflictFile> conflictsFromStatus(const QString& porcelain)
{
    QList<ConflictFile> conflicts;
    for (const QString& raw : porcelain.split(QLatin1Char('\n'))) {
        if (raw.size() < 4) {
            continue;
        }
        const QChar indexStatus = raw.at(0);
        const QChar workTreeStatus = raw.at(1);
        // "XY path"; unmerged combinations are the only ones with U or with
        // A/A, A/D, D/A, D/D pairs.
        const bool unmerged = indexStatus == QLatin1Char('U')
                              || workTreeStatus == QLatin1Char('U')
                              || (indexStatus == QLatin1Char('A') && workTreeStatus == QLatin1Char('A'))
                              || (indexStatus == QLatin1Char('D') && workTreeStatus == QLatin1Char('D'));
        if (!unmerged) {
            continue;
        }
        ConflictFile file;
        file.path = normalize(raw.mid(3));
        file.oursStatus = indexStatus;
        file.theirsStatus = workTreeStatus;
        file.bothAdded = indexStatus == QLatin1Char('A') && workTreeStatus == QLatin1Char('A');
        file.bothDeleted = indexStatus == QLatin1Char('D') && workTreeStatus == QLatin1Char('D');
        file.deletedByUs = indexStatus == QLatin1Char('D') && workTreeStatus == QLatin1Char('U');
        file.deletedByThem = indexStatus == QLatin1Char('U') && workTreeStatus == QLatin1Char('D');
        conflicts.append(file);
    }
    return conflicts;
}

OperationState detectOperationState(const QStringList& gitDirEntries)
{
    // Order matters: a rebase also writes CHERRY_PICK_HEAD while it replays.
    if (gitDirEntries.contains(QStringLiteral("rebase-merge"))
        || gitDirEntries.contains(QStringLiteral("rebase-apply"))) {
        return OperationState::Rebasing;
    }
    if (gitDirEntries.contains(QStringLiteral("MERGE_HEAD"))) {
        return OperationState::Merging;
    }
    if (gitDirEntries.contains(QStringLiteral("CHERRY_PICK_HEAD"))) {
        return OperationState::CherryPicking;
    }
    if (gitDirEntries.contains(QStringLiteral("REVERT_HEAD"))) {
        return OperationState::Reverting;
    }
    if (gitDirEntries.contains(QStringLiteral("BISECT_LOG"))) {
        return OperationState::Bisecting;
    }
    return OperationState::Clean;
}

QString branchFromStatusHeader(const QString& headerLine)
{
    // "## main...origin/main [ahead 1]" / "## HEAD (no branch)"
    if (!headerLine.startsWith(QLatin1String("## "))) {
        return {};
    }
    QString rest = headerLine.mid(3);
    const qsizetype dots = rest.indexOf(QLatin1String("..."));
    if (dots >= 0) {
        rest = rest.left(dots);
    }
    const qsizetype space = rest.indexOf(QLatin1Char(' '));
    if (space >= 0) {
        rest = rest.left(space);
    }
    if (rest.startsWith(QLatin1String("No commits yet"))) {
        return {};
    }
    return rest.trimmed();
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

namespace Commands {

QStringList listBranches()
{
    return {QStringLiteral("branch"), QStringLiteral("-vv"), QStringLiteral("--no-color")};
}

QStringList checkout(const QString& branch)
{
    // `switch` rather than `checkout`: it refuses an ambiguous name instead of
    // creating a branch from it, which is the safe reading of "go there".
    return {QStringLiteral("switch"), branch};
}

QStringList createBranch(const QString& name, const QString& from)
{
    QStringList arguments{QStringLiteral("branch"), name};
    if (!from.isEmpty()) {
        arguments << from;
    }
    return arguments;
}

QStringList createAndCheckout(const QString& name, const QString& from)
{
    QStringList arguments{QStringLiteral("switch"), QStringLiteral("-c"), name};
    if (!from.isEmpty()) {
        arguments << from;
    }
    return arguments;
}

QStringList deleteBranch(const QString& name, bool force)
{
    return {QStringLiteral("branch"), force ? QStringLiteral("-D") : QStringLiteral("-d"), name};
}

QStringList renameBranch(const QString& from, const QString& to)
{
    return {QStringLiteral("branch"), QStringLiteral("-m"), from, to};
}

QStringList setUpstream(const QString& branch, const QString& remote)
{
    return {QStringLiteral("branch"), QStringLiteral("--set-upstream-to=%1").arg(remote), branch};
}

QStringList merge(const QString& branch, bool noFastForward, bool squash)
{
    QStringList arguments{QStringLiteral("merge")};
    if (noFastForward) {
        arguments << QStringLiteral("--no-ff");
    }
    if (squash) {
        arguments << QStringLiteral("--squash");
    }
    arguments << branch;
    return arguments;
}

QStringList rebase(const QString& onto, bool interactive)
{
    QStringList arguments{QStringLiteral("rebase")};
    if (interactive) {
        // An interactive rebase needs a terminal; the IDE runs it in the
        // embedded terminal instead of capturing it, which the caller decides.
        arguments << QStringLiteral("--interactive");
    }
    if (!onto.isEmpty()) {
        arguments << onto;
    }
    return arguments;
}

QStringList cherryPick(const QString& commit)
{
    return {QStringLiteral("cherry-pick"), commit};
}

QStringList revert(const QString& commit)
{
    return {QStringLiteral("revert"), QStringLiteral("--no-edit"), commit};
}

QStringList abortOperation(OperationState state)
{
    switch (state) {
    case OperationState::Merging:       return {QStringLiteral("merge"), QStringLiteral("--abort")};
    case OperationState::Rebasing:      return {QStringLiteral("rebase"), QStringLiteral("--abort")};
    case OperationState::CherryPicking: return {QStringLiteral("cherry-pick"), QStringLiteral("--abort")};
    case OperationState::Reverting:     return {QStringLiteral("revert"), QStringLiteral("--abort")};
    case OperationState::Bisecting:     return {QStringLiteral("bisect"), QStringLiteral("reset")};
    case OperationState::Clean:         break;
    }
    return {};
}

QStringList continueOperation(OperationState state)
{
    switch (state) {
    case OperationState::Merging:
        // A merge has no --continue: committing is the continuation.
        return {QStringLiteral("commit"), QStringLiteral("--no-edit")};
    case OperationState::Rebasing:      return {QStringLiteral("rebase"), QStringLiteral("--continue")};
    case OperationState::CherryPicking: return {QStringLiteral("cherry-pick"), QStringLiteral("--continue")};
    case OperationState::Reverting:     return {QStringLiteral("revert"), QStringLiteral("--continue")};
    case OperationState::Bisecting:     return {QStringLiteral("bisect"), QStringLiteral("good")};
    case OperationState::Clean:         break;
    }
    return {};
}

QStringList stashPush(const QString& message, bool includeUntracked)
{
    QStringList arguments{QStringLiteral("stash"), QStringLiteral("push")};
    if (includeUntracked) {
        arguments << QStringLiteral("--include-untracked");
    }
    if (!message.isEmpty()) {
        arguments << QStringLiteral("-m") << message;
    }
    return arguments;
}

QStringList stashList()
{
    return {QStringLiteral("stash"), QStringLiteral("list")};
}

QStringList stashPop(int index)
{
    return {QStringLiteral("stash"), QStringLiteral("pop"),
            QStringLiteral("stash@{%1}").arg(index)};
}

QStringList stashApply(int index)
{
    return {QStringLiteral("stash"), QStringLiteral("apply"),
            QStringLiteral("stash@{%1}").arg(index)};
}

QStringList stashDrop(int index)
{
    return {QStringLiteral("stash"), QStringLiteral("drop"),
            QStringLiteral("stash@{%1}").arg(index)};
}

QStringList fetch(const QString& remote, bool prune)
{
    QStringList arguments{QStringLiteral("fetch")};
    if (prune) {
        arguments << QStringLiteral("--prune");
    }
    if (!remote.isEmpty()) {
        arguments << remote;
    }
    return arguments;
}

QStringList pull(const QString& remote, const QString& branch, bool rebase)
{
    QStringList arguments{QStringLiteral("pull")};
    if (rebase) {
        arguments << QStringLiteral("--rebase");
    }
    if (!remote.isEmpty()) {
        arguments << remote;
        if (!branch.isEmpty()) {
            arguments << branch;
        }
    }
    return arguments;
}

QStringList push(const QString& remote, const QString& branch,
                 bool setUpstream, bool forceWithLease)
{
    QStringList arguments{QStringLiteral("push")};
    // --force-with-lease rather than --force: it refuses to overwrite a
    // remote that moved since the last fetch, which is the only form a GUI
    // should offer.
    if (forceWithLease) {
        arguments << QStringLiteral("--force-with-lease");
    }
    if (setUpstream) {
        arguments << QStringLiteral("--set-upstream");
    }
    if (!remote.isEmpty()) {
        arguments << remote;
        if (!branch.isEmpty()) {
            arguments << branch;
        }
    }
    return arguments;
}

QStringList listTags()
{
    return {QStringLiteral("tag"), QStringLiteral("-l"),
            QStringLiteral("--format=%(refname:short)%09%(objectname:short)%09%(contents:subject)")};
}

QStringList createTag(const QString& name, const QString& message,
                      const QString& commit)
{
    QStringList arguments{QStringLiteral("tag")};
    if (!message.isEmpty()) {
        arguments << QStringLiteral("-a") << name << QStringLiteral("-m") << message;
    } else {
        arguments << name;
    }
    if (!commit.isEmpty()) {
        arguments << commit;
    }
    return arguments;
}

QStringList deleteTag(const QString& name, bool remote)
{
    if (remote) {
        return {QStringLiteral("push"), QStringLiteral("--delete"),
                QStringLiteral("origin"), name};
    }
    return {QStringLiteral("tag"), QStringLiteral("-d"), name};
}

QStringList clone(const QString& url, const QString& directory, int depth,
                  bool singleBranch, bool recurseSubmodules)
{
    const QString source = url.trimmed();
    if (source.isEmpty()) {
        return {};
    }
    QStringList args{QStringLiteral("clone")};
    if (depth > 0) {
        args << QStringLiteral("--depth") << QString::number(depth);
        if (singleBranch) {
            args << QStringLiteral("--single-branch");
        }
    }
    if (recurseSubmodules) {
        args << QStringLiteral("--recurse-submodules");
    }
    // `--` before the URLs so neither a URL nor a directory that begins with
    // a dash can be read as an option.
    args << QStringLiteral("--");
    args << source;
    const QString destination = directory.trimmed();
    if (!destination.isEmpty()) {
        args << destination;
    }
    return args;
}

QStringList listRemotes()
{
    return {QStringLiteral("remote"), QStringLiteral("-v")};
}

QStringList addRemote(const QString& name, const QString& url)
{
    return {QStringLiteral("remote"), QStringLiteral("add"), name, url};
}

QStringList removeRemote(const QString& name)
{
    return {QStringLiteral("remote"), QStringLiteral("remove"), name};
}

QStringList listSubmodules()
{
    return {QStringLiteral("submodule"), QStringLiteral("status")};
}

QStringList updateSubmodules(bool init, bool recursive)
{
    QStringList arguments{QStringLiteral("submodule"), QStringLiteral("update")};
    if (init) {
        arguments << QStringLiteral("--init");
    }
    if (recursive) {
        arguments << QStringLiteral("--recursive");
    }
    return arguments;
}

QStringList listWorktrees()
{
    return {QStringLiteral("worktree"), QStringLiteral("list"), QStringLiteral("--porcelain")};
}

QStringList addWorktree(const QString& path, const QString& branch)
{
    QStringList arguments{QStringLiteral("worktree"), QStringLiteral("add"), path};
    if (!branch.isEmpty()) {
        arguments << branch;
    }
    return arguments;
}

QStringList blame(const QString& file, int startLine, int lineCount)
{
    QStringList arguments{QStringLiteral("blame"), QStringLiteral("--porcelain")};
    if (startLine > 0 && lineCount > 0) {
        arguments << QStringLiteral("-L") << QStringLiteral("%1,+%2").arg(startLine).arg(lineCount);
    }
    arguments << QStringLiteral("--") << file;
    return arguments;
}

QStringList diffFile(const QString& file, bool staged)
{
    QStringList arguments{QStringLiteral("diff")};
    if (staged) {
        arguments << QStringLiteral("--cached");
    }
    arguments << QStringLiteral("--") << file;
    return arguments;
}

}  // namespace Commands

}  // namespace Git
}  // namespace LightCpp::Workbench
