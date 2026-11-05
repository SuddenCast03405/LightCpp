// GitStatusTests — parsing `git status --porcelain=v1 -b`.
//
// Every case feeds raw porcelain text, which is the format git explicitly
// promises to keep stable, so these inputs can be written by hand instead of
// produced by a real repository.

#include "LightCpp/Project/GitStatus.hpp"

#include <gtest/gtest.h>

namespace {

using LightCpp::Project::describeGitStatus;
using LightCpp::Project::GitStatusSummary;
using LightCpp::Project::parseGitStatus;

GitStatusSummary parse(const char* porcelain)
{
    return parseGitStatus(QByteArray(porcelain));
}

TEST(GitStatus, CleanTree)
{
    const GitStatusSummary status = parse("## main...origin/main\n");
    EXPECT_EQ(status.branch, QStringLiteral("main"));
    EXPECT_EQ(status.upstream, QStringLiteral("origin/main"));
    EXPECT_EQ(status.ahead, 0);
    EXPECT_EQ(status.behind, 0);
    EXPECT_FALSE(status.detached);
    EXPECT_TRUE(status.isEmpty());
    EXPECT_EQ(describeGitStatus(status), QStringLiteral("working tree clean"));
    EXPECT_EQ(status.label(), QStringLiteral("main"));
}

TEST(GitStatus, TracksAheadAndBehind)
{
    const GitStatusSummary status =
        parse("## main...origin/main [ahead 1, behind 2]\n"
              " M src/a.cpp\n"
              "?? notes.txt\n"
              "M  src/b.cpp\n");
    EXPECT_EQ(status.ahead, 1);
    EXPECT_EQ(status.behind, 2);
    ASSERT_EQ(status.entries.size(), 3);
    EXPECT_EQ(status.stagedCount(), 1);
    EXPECT_EQ(status.unstagedCount(), 1);
    EXPECT_EQ(status.untrackedCount(), 1);
    EXPECT_EQ(describeGitStatus(status),
              QStringLiteral("1 staged, 1 not staged, 1 untracked"));

    // "main ↑1 ↓2 ●" — drift markers, then a dot for the dirty tree.
    const QString expectedLabel = QStringLiteral("main ") + QChar(0x2191)
                                  + QStringLiteral("1 ") + QChar(0x2193)
                                  + QStringLiteral("2 ") + QChar(0x25CF);
    EXPECT_EQ(status.label(), expectedLabel);
}

TEST(GitStatus, StagedAndUnstagedAreDistinguished)
{
    const GitStatusSummary status =
        parse("## main\n"
              " M only-worktree.cpp\n"
              "M  only-index.cpp\n"
              "MM both.cpp\n");
    ASSERT_EQ(status.entries.size(), 3);

    EXPECT_FALSE(status.entries.at(0).hasStagedChanges());
    EXPECT_TRUE(status.entries.at(0).hasUnstagedChanges());

    EXPECT_TRUE(status.entries.at(1).hasStagedChanges());
    EXPECT_FALSE(status.entries.at(1).hasUnstagedChanges());

    EXPECT_TRUE(status.entries.at(2).hasStagedChanges());
    EXPECT_TRUE(status.entries.at(2).hasUnstagedChanges());

    EXPECT_EQ(status.entries.at(2).code(), QStringLiteral("MM"));
    EXPECT_EQ(status.stagedCount(), 2);
    EXPECT_EQ(status.unstagedCount(), 2);
}

TEST(GitStatus, UntrackedAndIgnoredAreNotStagedChanges)
{
    const GitStatusSummary status =
        parse("## main\n"
              "?? new.txt\n"
              "!! build/\n");
    ASSERT_EQ(status.entries.size(), 2);
    EXPECT_TRUE(status.entries.at(0).isUntracked());
    EXPECT_FALSE(status.entries.at(0).hasStagedChanges());
    EXPECT_FALSE(status.entries.at(0).hasUnstagedChanges());
    EXPECT_TRUE(status.entries.at(1).isIgnored());
    EXPECT_EQ(status.untrackedCount(), 1);
    EXPECT_EQ(status.stagedCount(), 0);
}

TEST(GitStatus, RenameCarriesBothPaths)
{
    const GitStatusSummary status =
        parse("## main\n"
              "R  old/name.cpp -> new/name.cpp\n");
    ASSERT_EQ(status.entries.size(), 1);
    EXPECT_EQ(status.entries.at(0).indexStatus, u'R');
    EXPECT_EQ(status.entries.at(0).path, QStringLiteral("new/name.cpp"));
    EXPECT_EQ(status.entries.at(0).originalPath, QStringLiteral("old/name.cpp"));
}

TEST(GitStatus, DeletedFileIsRecognised)
{
    const GitStatusSummary status = parse("## main\n D gone.cpp\n");
    ASSERT_EQ(status.entries.size(), 1);
    EXPECT_TRUE(status.entries.at(0).hasUnstagedChanges());
    EXPECT_EQ(status.entries.at(0).code(), QStringLiteral(" D"));
}

TEST(GitStatus, DetachedHeadIsReported)
{
    const GitStatusSummary status =
        parse("## HEAD (no branch)\n"
              " M x.cpp\n");
    EXPECT_TRUE(status.detached);
    EXPECT_TRUE(status.branch.isEmpty());
    EXPECT_EQ(status.label(), QStringLiteral("(detached HEAD) \u25CF"));
}

TEST(GitStatus, UnbornBranchHasANameButNoUpstream)
{
    const GitStatusSummary status =
        parse("## No commits yet on main\n"
              "?? a.txt\n");
    EXPECT_EQ(status.branch, QStringLiteral("main"));
    EXPECT_TRUE(status.upstream.isEmpty());
    EXPECT_FALSE(status.detached);
}

TEST(GitStatus, UnbornBranchCanStillNameItsUpstream)
{
    // git prints the tracking relation for an unborn branch too when the
    // config already points at a remote. Treating that as "just a name"
    // leaked the upstream into the branch ("main...origin/main") and left the
    // upstream empty, so the status bar showed a branch that does not exist.
    const GitStatusSummary status =
        parse("## No commits yet on main...origin/main\n"
              "?? a.txt\n");
    EXPECT_EQ(status.branch, QStringLiteral("main"));
    EXPECT_EQ(status.upstream, QStringLiteral("origin/main"));
    EXPECT_FALSE(status.detached);

    // Same for the other wording of the same state.
    const GitStatusSummary initial =
        parse("## Initial commit on trunk...origin/trunk [ahead 1]\n");
    EXPECT_EQ(initial.branch, QStringLiteral("trunk"));
    EXPECT_EQ(initial.upstream, QStringLiteral("origin/trunk"));
    EXPECT_EQ(initial.ahead, 1);
}

TEST(GitStatus, BranchWithoutUpstream)
{
    const GitStatusSummary status = parse("## feature/x\n");
    EXPECT_EQ(status.branch, QStringLiteral("feature/x"));
    EXPECT_TRUE(status.upstream.isEmpty());
}

TEST(GitStatus, QuotedPathsAreUnescaped)
{
    const GitStatusSummary status =
        parse("## main\n"
              "?? \"weird name\\twith tab.cpp\"\n"
              "?? \"quote\\\"inside.cpp\"\n");
    ASSERT_EQ(status.entries.size(), 2);
    EXPECT_EQ(status.entries.at(0).path, QStringLiteral("weird name\twith tab.cpp"));
    EXPECT_EQ(status.entries.at(1).path, QStringLiteral("quote\"inside.cpp"));
}

TEST(GitStatus, CrlfOutputIsTolerated)
{
    const GitStatusSummary status = parse("## main\r\n M a.cpp\r\n?? b.txt\r\n");
    EXPECT_EQ(status.branch, QStringLiteral("main"));
    EXPECT_EQ(status.entries.size(), 2);
    EXPECT_EQ(status.entries.at(0).path, QStringLiteral("a.cpp"));
    EXPECT_EQ(status.entries.at(1).path, QStringLiteral("b.txt"));
}

TEST(GitStatus, EmptyAndGarbageInputIsHarmless)
{
    EXPECT_TRUE(parse("").isEmpty());
    EXPECT_TRUE(parse("\n\n").isEmpty());
    EXPECT_TRUE(parse("not porcelain at all").isEmpty());
    EXPECT_TRUE(parse("\n").branch.isEmpty());
}

TEST(GitStatus, GarbageLinesAreSkippedWithoutLosingTheRealOnes)
{
    // "not porcelain at all" happens to look like two status columns followed
    // by a space, so the letters themselves have to be checked — and a bad
    // line must not take its neighbours down with it.
    const GitStatusSummary status =
        parse("## main\n M a.cpp\nnot porcelain at all\n?? b.txt\n");
    ASSERT_EQ(status.entries.size(), 2);
    EXPECT_EQ(status.entries.at(0).path, QStringLiteral("a.cpp"));
    EXPECT_EQ(status.entries.at(1).path, QStringLiteral("b.txt"));
}

TEST(GitStatus, TheUncommonStatusLettersAreAccepted)
{
    // `T` (typechange) and `U` (unmerged) are rare enough that a stricter
    // letter check would silently hide them rather than fail loudly.
    const GitStatusSummary status = parse("## main\nT  a.h\nUU b.cpp\n");
    ASSERT_EQ(status.entries.size(), 2);
    EXPECT_EQ(status.entries.at(0).code(), QStringLiteral("T "));
    EXPECT_EQ(status.entries.at(1).code(), QStringLiteral("UU"));
    EXPECT_TRUE(status.entries.at(1).hasStagedChanges());
    EXPECT_TRUE(status.entries.at(1).hasUnstagedChanges());
}

TEST(GitStatus, LabelMarksADirtyTreeWithoutDrift)
{
    const GitStatusSummary status = parse("## main\n M a.cpp\n");
    EXPECT_EQ(status.label(), QStringLiteral("main \u25CF"));
}

TEST(GitStatus, UnknownTrackingWordsLeaveTheCountsAlone)
{
    // The counting words are the one part of the header git does not formally
    // promise to keep in English; an unrecognised label must not be guessed at.
    const GitStatusSummary status =
        parse("## main...origin/main [vorsprung 1, zurueck 2]\n");
    EXPECT_EQ(status.ahead, 0);
    EXPECT_EQ(status.behind, 0);
    EXPECT_EQ(status.branch, QStringLiteral("main"));
    EXPECT_EQ(status.upstream, QStringLiteral("origin/main"));
}

}  // namespace
