// GitExtra — implementation. See the header for the two traps (blame blocks
// and conflict markers that are not at column 0).

#include "LightCpp/Workbench/GitExtra.hpp"

#include <QCryptographicHash>

namespace LightCpp::Workbench::Git {
namespace {

bool startsWithMarker(const QString& line, QChar marker)
{
    // git writes the markers at column 0 in the default conflict style. A
    // marker indented inside a conflicted raw string is NOT a conflict
    // delimiter, and treating it as one corrupts the file.
    return !line.isEmpty() && line.at(0) == marker;
}

QString markerLabel(const QString& line, int markerLength)
{
    return line.mid(markerLength).trimmed();
}

}  // namespace

// ---------------------------------------------------------------------------
// Blame
// ---------------------------------------------------------------------------

QList<BlameBlock> groupBlame(const QList<BlameLine>& lines)
{
    QList<BlameBlock> blocks;
    for (const BlameLine& line : lines) {
        if (!blocks.isEmpty()) {
            BlameBlock& last = blocks.last();
            if (last.sha == line.sha && last.firstLine + last.lineCount == line.line) {
                ++last.lineCount;
                last.content.append(line.content);
                continue;
            }
        }
        BlameBlock block;
        block.sha = line.sha;
        block.shortSha = line.sha.left(8);
        block.author = line.author;
        block.date = line.date;
        block.summary = line.summary;
        block.firstLine = line.line;
        block.lineCount = 1;
        block.uncommitted = line.uncommitted
                            || line.sha.startsWith(QLatin1String("00000000"));
        block.content.append(line.content);
        blocks.append(block);
    }
    return blocks;
}

int blameColourIndex(const QString& sha)
{
    // FNV-1a over the sha, folded to 8 buckets. Stable across runs and across
    // machines, so the same commit keeps its tint.
    quint32 hash = 2166136261u;
    for (const QChar ch : sha) {
        hash ^= static_cast<quint32>(ch.unicode());
        hash *= 16777619u;
    }
    return static_cast<int>(hash % 8u);
}

// ---------------------------------------------------------------------------
// Merge
// ---------------------------------------------------------------------------

QString mergeChoiceName(MergeChoice choice)
{
    switch (choice) {
    case MergeChoice::Ours:         return QStringLiteral("ours");
    case MergeChoice::Theirs:       return QStringLiteral("theirs");
    case MergeChoice::Both:         return QStringLiteral("both");
    case MergeChoice::BothReversed: return QStringLiteral("both-reversed");
    case MergeChoice::Base:         return QStringLiteral("base");
    }
    return QStringLiteral("ours");
}

MergeChoice mergeChoiceFromName(const QString& name)
{
    const QString lower = name.trimmed().toLower();
    if (lower == QLatin1String("theirs")) {
        return MergeChoice::Theirs;
    }
    if (lower == QLatin1String("both")) {
        return MergeChoice::Both;
    }
    if (lower == QLatin1String("both-reversed")) {
        return MergeChoice::BothReversed;
    }
    if (lower == QLatin1String("base")) {
        return MergeChoice::Base;
    }
    return MergeChoice::Ours;
}

MergeFile parseConflictFile(const QString& text)
{
    MergeFile file;
    file.newline = text.contains(QLatin1String("\r\n")) ? QStringLiteral("\r\n")
                                                        : QStringLiteral("\n");
    QString body = text;
    if (body.endsWith(QLatin1String("\r\n"))) {
        body.chop(2);
    } else if (body.endsWith(QLatin1Char('\n'))) {
        body.chop(1);
    }
    QStringList lines = body.split(QLatin1Char('\n'));
    // A CRLF file leaves a trailing '\r' on every line; strip it per line so
    // the rebuilt file does not accumulate mixed endings.
    if (file.newline == QLatin1String("\r\n")) {
        for (QString& line : lines) {
            if (line.endsWith(QLatin1Char('\r'))) {
                line.chop(1);
            }
        }
    }

    int index = 0;
    QStringList pending;  // lines since the last hunk ended
    while (index < lines.size()) {
        const QString& line = lines.at(index);
        if (!startsWithMarker(line, QLatin1Char('<')) || line.size() < 8) {
            pending.append(line);
            ++index;
            continue;
        }
        // Count the marker's length: `<<<<<<<` is 7, but some tools write
        // more; the label follows the run of the same character.
        int markerLength = 0;
        while (markerLength < line.size() && line.at(markerLength) == QLatin1Char('<')) {
            ++markerLength;
        }
        MergeHunk hunk;
        hunk.startLine = index;
        hunk.oursLabel = markerLabel(line, markerLength);
        ++index;

        // Ours until `|||||||` (base) or `=======`.
        bool inBase = false;
        bool inTheirs = false;
        QStringList ours;
        QStringList base;
        QStringList theirs;
        bool closed = false;
        for (; index < lines.size(); ++index) {
            const QString& current = lines.at(index);
            if (!inTheirs && current.startsWith(QLatin1String("|||||||"))) {
                inBase = true;
                hunk.hasBase = true;
                hunk.baseLabel = current.mid(7).trimmed();
                continue;
            }
            if (!inTheirs && current.startsWith(QLatin1String("======="))
                && current.trimmed() == QLatin1String("=======")) {
                inBase = false;
                inTheirs = true;
                continue;
            }
            if (inTheirs && current.startsWith(QLatin1String(">>>>>>>"))) {
                int endMarker = 0;
                while (endMarker < current.size()
                       && current.at(endMarker) == QLatin1Char('>')) {
                    ++endMarker;
                }
                hunk.theirsLabel = markerLabel(current, endMarker);
                hunk.endLine = index;
                closed = true;
                ++index;
                break;
            }
            if (inTheirs) {
                theirs.append(current);
            } else if (inBase) {
                base.append(current);
            } else {
                ours.append(current);
            }
        }
        if (!closed) {
            // An unterminated marker block is not a conflict we can resolve;
            // hand the text back unchanged rather than truncate the file.
            file.prefix += pending;
            file.prefix += lines.mid(hunk.startLine);
            file.hasConflicts = false;
            return file;
        }
        hunk.ourLines = ours;
        hunk.baseLines = base;
        hunk.theirLines = theirs;
        if (file.hunks.isEmpty()) {
            file.prefix = pending;
        } else {
            // Text between hunks belongs to the previous hunk's trailing
            // context; keep it in front of this one by appending to the
            // previous hunk's tail is not possible, so it is stored as the
            // hunk's leading context in `suffix` of the previous hunk. The
            // simplest correct model: treat the between-text as part of the
            // resolved output by remembering it in `betweenTexts`.
            file.betweenTexts.append(pending);
        }
        pending.clear();
        file.hunks.append(hunk);
    }
    file.suffix = pending;
    file.hasConflicts = !file.hunks.isEmpty();
    return file;
}

QStringList resolveHunk(const MergeHunk& hunk, MergeChoice choice)
{
    switch (choice) {
    case MergeChoice::Ours:   return hunk.ourLines;
    case MergeChoice::Theirs: return hunk.theirLines;
    case MergeChoice::Base:   return hunk.baseLines;
    case MergeChoice::Both: {
        QStringList both = hunk.ourLines;
        both.append(hunk.theirLines);
        return both;
    }
    case MergeChoice::BothReversed: {
        QStringList both = hunk.theirLines;
        both.append(hunk.ourLines);
        return both;
    }
    }
    return hunk.ourLines;
}

QString resolveConflictFile(const MergeFile& file, const QList<MergeChoice>& choices)
{
    QStringList out = file.prefix;
    for (int i = 0; i < file.hunks.size(); ++i) {
        if (i > 0 && i - 1 < file.betweenTexts.size()) {
            out += file.betweenTexts.at(i - 1);
        }
        const MergeChoice choice = i < choices.size() ? choices.at(i) : MergeChoice::Ours;
        out += resolveHunk(file.hunks.at(i), choice);
    }
    out += file.suffix;
    return out.join(file.newline);
}

QString resolveAll(const MergeFile& file, MergeChoice choice)
{
    QList<MergeChoice> choices;
    choices.reserve(file.hunks.size());
    for (int i = 0; i < file.hunks.size(); ++i) {
        choices.append(choice);
    }
    return resolveConflictFile(file, choices);
}

int conflictCount(const MergeFile& file)
{
    return static_cast<int>(file.hunks.size());
}

// ---------------------------------------------------------------------------
// Commit message
// ---------------------------------------------------------------------------

QString applyCommitTemplate(const QString& templateText, const QString& branch,
                            const QStringList& files)
{
    QString text = templateText;
    text.replace(QLatin1String("{branch}"), branch);
    text.replace(QLatin1String("{files}"),
                 files.isEmpty() ? QStringLiteral("(no files)")
                                 : files.join(QStringLiteral(", ")));
    return text;
}

QStringList commitArguments(const QString& message, bool amend, bool signOff,
                            bool gpgSign, const QString& gpgKey)
{
    QStringList arguments{QStringLiteral("commit"), QStringLiteral("--file"),
                          QStringLiteral("-")};
    // --cleanup=strip removes the `#` comment lines a template carries and
    // trims trailing whitespace; without it those comments become the message
    // whenever the message came from a file.
    arguments << QStringLiteral("--cleanup=strip");
    if (amend) {
        arguments << QStringLiteral("--amend");
    }
    if (signOff) {
        arguments << QStringLiteral("--signoff");
    }
    if (gpgSign) {
        if (!gpgKey.trimmed().isEmpty()) {
            arguments << QStringLiteral("--gpg-sign=%1").arg(gpgKey.trimmed());
        } else {
            arguments << QStringLiteral("--gpg-sign");
        }
    }
    Q_UNUSED(message);  // the message goes to stdin, not to argv
    return arguments;
}

SigningConfig parseSigningConfig(const QString& configText)
{
    SigningConfig config;
    for (const QString& raw : configText.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const qsizetype space = line.indexOf(QLatin1Char(' '));
        if (space <= 0) {
            continue;
        }
        const QString key = line.left(space);
        const QString value = line.mid(space + 1).trimmed();
        if (key == QLatin1String("commit.gpgsign")) {
            config.enabled = value.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
        } else if (key == QLatin1String("user.signingkey")) {
            config.keyId = value;
        } else if (key == QLatin1String("gpg.format")) {
            config.format = value;
        } else if (key == QLatin1String("gpg.program")) {
            config.program = value;
        }
    }
    // A configured key is not the same as "sign by default": git only signs
    // when commit.gpgsign is true or -S is passed. The IDE offers the key in
    // the dialog and lets the user opt in per commit.
    return config;
}

QStringList signingConfigQueries()
{
    return {
        QStringLiteral("config"), QStringLiteral("--get-regexp"),
        QStringLiteral("^(commit\\.gpgsign|user\\.signingkey|gpg\\.format|gpg\\.program)$"),
    };
}

QStringList authenticationQueries()
{
    return {
        QStringLiteral("config"), QStringLiteral("--get-regexp"),
        QStringLiteral("^(credential\\.|core\\.sshCommand|url\\.)"),
    };
}

// ---------------------------------------------------------------------------
// Submodules / worktrees / remotes
// ---------------------------------------------------------------------------

QList<SubmoduleEntry> parseSubmoduleStatus(const QString& text)
{
    QList<SubmoduleEntry> entries;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        if (raw.trimmed().isEmpty()) {
            continue;
        }
        // " <sha> <path> (<describe>)" — the first character is the status and
        // is a space when the submodule is in sync.
        SubmoduleEntry entry;
        entry.status = raw.at(0);
        QString rest = raw.mid(1).trimmed();
        const qsizetype space = rest.indexOf(QLatin1Char(' '));
        if (space <= 0) {
            continue;
        }
        entry.sha = rest.left(space);
        rest = rest.mid(space + 1).trimmed();
        const qsizetype open = rest.lastIndexOf(QLatin1String(" ("));
        if (open > 0) {
            entry.path = rest.left(open);
            entry.describe = rest.mid(open + 2);
            if (entry.describe.endsWith(QLatin1Char(')'))) {
                entry.describe.chop(1);
            }
        } else {
            entry.path = rest;
        }
        if (!entry.path.isEmpty()) {
            entries.append(entry);
        }
    }
    return entries;
}

QList<WorktreeEntry> parseWorktreeList(const QString& text)
{
    QList<WorktreeEntry> entries;
    WorktreeEntry current;
    bool started = false;
    const auto flush = [&] {
        if (started && !current.path.isEmpty()) {
            entries.append(current);
        }
        current = WorktreeEntry{};
        started = false;
    };
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        if (raw.trimmed().isEmpty()) {
            flush();
            continue;
        }
        const qsizetype space = raw.indexOf(QLatin1Char(' '));
        const QString key = space < 0 ? raw : raw.left(space);
        const QString value = space < 0 ? QString() : raw.mid(space + 1);
        if (key == QLatin1String("worktree")) {
            flush();
            current.path = value;
            started = true;
        } else if (key == QLatin1String("HEAD")) {
            current.head = value;
        } else if (key == QLatin1String("branch")) {
            current.branch = value;
            if (current.branch.startsWith(QLatin1String("refs/heads/"))) {
                current.branch = current.branch.mid(11);
            }
        } else if (key == QLatin1String("bare")) {
            current.bare = true;
        } else if (key == QLatin1String("detached")) {
            current.detached = true;
        } else if (key == QLatin1String("locked")) {
            current.locked = true;
        } else if (key == QLatin1String("prunable")) {
            current.prunable = true;
        }
    }
    flush();
    return entries;
}

QStringList removeWorktree(const QString& path, bool force)
{
    QStringList arguments{QStringLiteral("worktree"), QStringLiteral("remove")};
    if (force) {
        arguments << QStringLiteral("--force");
    }
    arguments << path;
    return arguments;
}

QStringList addSubmodule(const QString& url, const QString& path, bool shallow)
{
    QStringList arguments{QStringLiteral("submodule"), QStringLiteral("add")};
    if (shallow) {
        arguments << QStringLiteral("--depth") << QStringLiteral("1");
    }
    arguments << url << path;
    return arguments;
}

QStringList setRemoteUrl(const QString& name, const QString& url, bool push)
{
    QStringList arguments{QStringLiteral("remote"), QStringLiteral("set-url")};
    if (push) {
        arguments << QStringLiteral("--push");
    }
    arguments << name << url;
    return arguments;
}

QStringList renameRemote(const QString& from, const QString& to)
{
    return {QStringLiteral("remote"), QStringLiteral("rename"), from, to};
}

}  // namespace LightCpp::Workbench::Git
