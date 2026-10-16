// GitClientTests — what the client does when there is no working directory.
//
// R2-18: the client used to hand an empty working directory to QProcess, which
// falls back to *this process's* start-up directory. `git init` then created a
// repository wherever the IDE happened to be launched from — the install
// folder, or the user's home — and the window reported the init as successful
// and showed the branch of that unrelated directory.
//
// The refusal path runs without starting a process on purpose: this machine
// cannot create a child's pipes, so anything that actually spawned git could
// not be tested here at all — and the interesting behaviour is precisely that
// nothing is spawned.

#include "LightCpp/App/GitClient.hpp"

#include <gtest/gtest.h>

#include <QObject>
#include <QString>

using LightCpp::App::GitClient;

namespace {

/// The arguments a run is reported with.
struct Report {
    int calls = 0;
    QString title;
    QString text;
    int exitCode = 0;

    void capture(const QString& t, const QString& x, int code)
    {
        ++calls;
        title = t;
        text = x;
        exitCode = code;
    }
};

}  // namespace

TEST(GitClient, ACommandWithNoWorkingDirectoryIsRefused)
{
    GitClient client;
    // A path that does not exist: if the client tried to run anything, the
    // report would be a different one (and on a working machine, a real
    // process). Empty working directory must be caught before that.
    client.setToolPath(QStringLiteral("/nonexistent/git"));

    Report report;
    QObject::connect(&client, &GitClient::commandFinished,
                     [&report](const QString& t, const QString& x, int c) {
                         report.capture(t, x, c);
                     });

    client.run({QStringLiteral("init")}, QStringLiteral("git init"));

    // Emitted synchronously: the refusal needs no event loop.
    EXPECT_EQ(report.calls, 1);
    EXPECT_EQ(report.title, QStringLiteral("git init"));
    EXPECT_EQ(report.exitCode, -1);
    EXPECT_NE(report.text.indexOf(QStringLiteral("working directory")), -1);
    // The queue drained — the job was answered, not left pending.
    EXPECT_FALSE(client.isBusy());
}

TEST(GitClient, AStatusRefreshWithNoWorkingDirectoryIsTheEmptyAnswer)
{
    GitClient client;
    int calls = 0;
    LightCpp::Project::GitStatusSummary summary;
    QObject::connect(&client, &GitClient::statusReady,
                     [&](const LightCpp::Project::GitStatusSummary& s) {
                         ++calls;
                         summary = s;
                     });

    client.refreshStatus();

    // The same shape `git status` outside a repository produces: no branch,
    // no entries — which the window reads as "nothing to track here".
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(summary.branch.isEmpty());
    EXPECT_TRUE(summary.entries.isEmpty());
    EXPECT_FALSE(summary.detached);
}

TEST(GitClient, EveryQueuedCommandIsStillAnswered)
{
    // The refusal must not wedge the queue: it pumps the next job on the way
    // out, so a second command queued behind the first is answered too.
    GitClient client;
    int finished = 0;
    QObject::connect(&client, &GitClient::commandFinished,
                     [&finished](const QString&, const QString&, int) { ++finished; });

    client.run({QStringLiteral("init")}, QStringLiteral("git init"));
    client.run({QStringLiteral("status")}, QStringLiteral("git status"));

    EXPECT_EQ(finished, 2);
    EXPECT_FALSE(client.isBusy());
}
