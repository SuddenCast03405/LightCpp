// GitCommandPolicyTests — how long git may run, and what a kill is allowed to
// leave behind.
//
// R2-46: one 60 s watchdog covered every command. `git commit` runs the user's
// hooks and can take minutes, so a commit could be killed in the middle — which
// leaves `.git/index.lock` behind, and after that *every* git command fails
// until someone deletes that file by hand. A `git fetch` was killed for the
// same reason, and the status refresh that timed out emitted nothing at all, so
// the panel simply stopped updating.
//
// The claims are pure: what kind of command this is, what budget that kind
// gets, and what the timeout message is allowed to say.

#include "LightCpp/App/GitCommandPolicy.hpp"

#include <gtest/gtest.h>

#include <QString>
#include <QStringList>

using namespace LightCpp::App::GitCommandPolicy;

TEST(GitCommandPolicy, AStatusRefreshIsARead)
{
    // Exactly what GitClient::refreshStatus() queues.
    const QStringList args{QStringLiteral("status"),
                           QStringLiteral("--porcelain=v1"),
                           QStringLiteral("-b"), QStringLiteral("-uall")};
    EXPECT_EQ(kindOf(args), Kind::ReadOnly);
    EXPECT_EQ(timeoutMsFor(kindOf(args)), 60 * 1000);
    EXPECT_FALSE(mayLeaveIndexLock(kindOf(args)));
}

TEST(GitCommandPolicy, ACommitIsGivenTheLongBudget)
{
    const QStringList args{QStringLiteral("commit"), QStringLiteral("-m"),
                           QStringLiteral("message")};
    EXPECT_EQ(kindOf(args), Kind::WritesRepository);
    EXPECT_EQ(timeoutMsFor(kindOf(args)), 10 * 60 * 1000);
    // A kill here is what leaves the lock behind.
    EXPECT_TRUE(mayLeaveIndexLock(kindOf(args)));
}

TEST(GitCommandPolicy, AFetchIsANetworkCommand)
{
    EXPECT_EQ((kindOf({QStringLiteral("fetch"), QStringLiteral("origin")})),
              Kind::Network);
    EXPECT_EQ(timeoutMsFor(Kind::Network), 10 * 60 * 1000);
}

TEST(GitCommandPolicy, GlobalOptionsAreSkippedWithTheirValues)
{
    // A `-c` / `-C` value is not the subcommand: taking it for one is how a
    // push ends up classified as something else.
    const QStringList args{QStringLiteral("-c"),
                           QStringLiteral("core.quotepath=false"),
                           QStringLiteral("-C"), QStringLiteral("sub"),
                           QStringLiteral("push")};
    EXPECT_EQ(kindOf(args), Kind::Network);
}

TEST(GitCommandPolicy, EveryCommandTheWindowIssuesIsClassified)
{
    // The real call sites, so a new one that falls into the wrong bucket shows
    // up here rather than as a git killed at 60 s.
    struct Case {
        QStringList args;
        Kind kind;
    };
    const Case cases[] = {
        {{QStringLiteral("status"), QStringLiteral("--porcelain=v1")},
         Kind::ReadOnly},
        {{QStringLiteral("log"), QStringLiteral("--oneline"),
          QStringLiteral("-n"), QStringLiteral("40")},
         Kind::ReadOnly},
        {{QStringLiteral("diff"), QStringLiteral("--staged")}, Kind::ReadOnly},
        {{QStringLiteral("add"), QStringLiteral("--all")},
         Kind::WritesRepository},
        {{QStringLiteral("reset"), QStringLiteral("--quiet")},
         Kind::WritesRepository},
        {{QStringLiteral("commit"), QStringLiteral("-m"), QStringLiteral("x")},
         Kind::WritesRepository},
        // Not in any list: falls through to the conservative answer.
        {{QStringLiteral("init")}, Kind::WritesRepository},
    };
    for (const Case& testCase : cases) {
        EXPECT_EQ(kindOf(testCase.args), testCase.kind)
            << testCase.args.join(u' ').toStdString();
    }
}

TEST(GitCommandPolicy, AnUnknownSubcommandGetsTheLongBudgetToo)
{
    // Killing an unknown command early can cost a lock; giving it time only
    // costs patience if it hangs.
    EXPECT_EQ(kindOf({QStringLiteral("frobnicate")}), Kind::WritesRepository);
    EXPECT_EQ(kindOf({QStringLiteral("init")}), Kind::WritesRepository);
}

TEST(GitCommandPolicy, OnlyReadsGetTheShortBudget)
{
    EXPECT_LT(timeoutMsFor(Kind::ReadOnly),
              timeoutMsFor(Kind::WritesRepository));
    EXPECT_LT(timeoutMsFor(Kind::ReadOnly), timeoutMsFor(Kind::Network));
}

TEST(GitCommandPolicy, TheCommitTimeoutMessageNamesTheCommandTheBudgetAndTheLock)
{
    const QString text = timeoutMessage(QStringLiteral("git commit"),
                                        Kind::WritesRepository, 10 * 60 * 1000);
    EXPECT_NE(text.indexOf(QStringLiteral("git commit")), -1);
    EXPECT_NE(text.indexOf(QStringLiteral("10 min")), -1);
    // The actionable half: what the next failure will look like, and the fix.
    EXPECT_NE(text.indexOf(QStringLiteral("index.lock")), -1);
}

TEST(GitCommandPolicy, AReadsTimeoutMessageDoesNotPromiseALock)
{
    const QString text = timeoutMessage(QStringLiteral("git status"),
                                        Kind::ReadOnly, 60 * 1000);
    EXPECT_NE(text.indexOf(QStringLiteral("60 s")), -1);
    // Nothing was written, so there is nothing to clean up — mentioning the
    // lock here would send the user looking for a file that does not exist.
    EXPECT_EQ(text.indexOf(QStringLiteral("index.lock")), -1);
}

TEST(GitCommandPolicy, ANetworkTimeoutSaysTheRemoteMayStillBeWorking)
{
    const QString text = timeoutMessage(QStringLiteral("git fetch"),
                                        Kind::Network, 10 * 60 * 1000);
    EXPECT_NE(text.indexOf(QStringLiteral("remote")), -1);
}