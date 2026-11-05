#include "LightCpp/App/GitCommandPolicy.hpp"

#include <QSet>

namespace LightCpp::App::GitCommandPolicy {
namespace {

constexpr int kReadOnlyBudgetMs = 60 * 1000;
constexpr int kLongBudgetMs = 10 * 60 * 1000;

const QSet<QString>& networkCommands()
{
    static const QSet<QString> names{
        QStringLiteral("fetch"),      QStringLiteral("pull"),
        QStringLiteral("push"),       QStringLiteral("clone"),
        QStringLiteral("ls-remote"),  QStringLiteral("remote"),
        QStringLiteral("submodule"),  QStringLiteral("fetch-pack"),
        QStringLiteral("send-pack"),  QStringLiteral("archive"),
    };
    return names;
}

const QSet<QString>& repositoryWritingCommands()
{
    static const QSet<QString> names{
        QStringLiteral("add"),          QStringLiteral("rm"),
        QStringLiteral("mv"),           QStringLiteral("reset"),
        QStringLiteral("commit"),       QStringLiteral("checkout"),
        QStringLiteral("switch"),       QStringLiteral("restore"),
        QStringLiteral("merge"),        QStringLiteral("rebase"),
        QStringLiteral("cherry-pick"),  QStringLiteral("revert"),
        QStringLiteral("am"),           QStringLiteral("apply"),
        QStringLiteral("stash"),        QStringLiteral("clean"),
        QStringLiteral("read-tree"),    QStringLiteral("update-index"),
        QStringLiteral("worktree"),     QStringLiteral("gc"),
        QStringLiteral("prune"),        QStringLiteral("maintenance"),
    };
    return names;
}

/// True when `word` is an option that swallows the next argument. git's global
/// options are the ones that can sit before the subcommand.
bool optionTakesValue(const QString& word)
{
    return word == QLatin1String("-c") || word == QLatin1String("-C")
           || word == QLatin1String("--git-dir")
           || word == QLatin1String("--work-tree")
           || word == QLatin1String("--namespace")
           || word == QLatin1String("--exec-path")
           || word == QLatin1String("--config-env");
}

QString describeBudget(int timeoutMs)
{
    // "1 min" reads like a lie when the budget is exactly a minute, so minutes
    // only start above that.
    if (timeoutMs >= 2 * 60000 && timeoutMs % 60000 == 0) {
        return QStringLiteral("%1 min").arg(timeoutMs / 60000);
    }
    return QStringLiteral("%1 s").arg(timeoutMs / 1000);
}

}  // namespace

Kind kindOf(const QStringList& arguments)
{
    for (int i = 0; i < arguments.size(); ++i) {
        const QString& word = arguments.at(i);
        if (word.isEmpty()) {
            continue;
        }
        if (word.startsWith(u'-')) {
            if (optionTakesValue(word)) {
                ++i;  // skip the option's value as well
            }
            continue;
        }
        const QString name = word.toLower();
        if (networkCommands().contains(name)) {
            return Kind::Network;
        }
        if (repositoryWritingCommands().contains(name)) {
            return Kind::WritesRepository;
        }
        // status, diff, log, show, rev-parse, ... plus anything unknown.
        static const QSet<QString> readOnly{
            QStringLiteral("status"),    QStringLiteral("diff"),
            QStringLiteral("log"),       QStringLiteral("show"),
            QStringLiteral("rev-parse"), QStringLiteral("blame"),
            QStringLiteral("describe"),  QStringLiteral("shortlog"),
            QStringLiteral("whatchanged"), QStringLiteral("cat-file"),
            QStringLiteral("ls-files"),  QStringLiteral("ls-tree"),
            QStringLiteral("branch"),    QStringLiteral("tag"),
            QStringLiteral("for-each-ref"), QStringLiteral("reflog"),
            QStringLiteral("grep"),      QStringLiteral("version"),
        };
        if (readOnly.contains(name)) {
            return Kind::ReadOnly;
        }
        return Kind::WritesRepository;
    }
    // No subcommand at all: nothing will run, and the short budget costs
    // nothing.
    return Kind::ReadOnly;
}

int timeoutMsFor(Kind kind)
{
    switch (kind) {
    case Kind::ReadOnly:
        return kReadOnlyBudgetMs;
    case Kind::WritesRepository:
    case Kind::Network:
        return kLongBudgetMs;
    }
    return kLongBudgetMs;
}

bool mayLeaveIndexLock(Kind kind)
{
    return kind != Kind::ReadOnly;
}

QString timeoutMessage(const QString& title, Kind kind, int timeoutMs)
{
    const QString name = title.isEmpty() ? QStringLiteral("git") : title;
    QString text = QStringLiteral("%1 did not finish within %2 and was killed")
                       .arg(name, describeBudget(timeoutMs));
    if (kind == Kind::Network) {
        text += QStringLiteral(
            " — the remote side may still be working, so the next fetch or push"
            " may report the operation as already in progress");
    }
    if (mayLeaveIndexLock(kind)) {
        text += QStringLiteral(
            " — if the next command fails because .git/index.lock exists,"
            " delete that file and run it again");
    }
    return text;
}

}  // namespace LightCpp::App::GitCommandPolicy
