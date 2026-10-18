// ToolchainPaths — where the fallback tools are, and what "not found" means.
//
// The property that matters is not *which* directory is chosen on this machine
// but that the answer is never invented: the version of this file that used
// constants returned E:/msys64/ucrt64/bin even where no such directory
// existed, and every consumer downstream failed naming a path the user had
// never chosen. So the tests below pin the search order (with an injected
// existence predicate, so they do not need an MSYS2 install) and pin the
// contract "empty, or a path that is really there".

#include "LightCpp/App/ToolchainPaths.hpp"

#include <QDir>
#include <QFileInfo>

#include <gtest/gtest.h>

using LightCpp::App::ToolchainPaths::firstWithTool;

namespace {

/// Predicate over a fixed set of directories: exactly what the real callers do
/// with QFileInfo::exists, without a filesystem.
std::function<bool(const QString&)> containing(const QStringList& dirs)
{
    return [dirs](const QString& dir) { return dirs.contains(dir); };
}

QStringList list(std::initializer_list<const char*> items)
{
    QStringList out;
    for (const char* item : items) {
        out.append(QString::fromLatin1(item));
    }
    return out;
}

}  // namespace

TEST(ToolchainPaths, TheFirstCandidateThatHasTheToolWins)
{
    const QStringList candidates =
        list({"/nowhere/a", "/nowhere/b", "/nowhere/c"});
    EXPECT_EQ(firstWithTool(candidates, containing(list({"/nowhere/c"}))),
              QStringLiteral("/nowhere/c"));
}

TEST(ToolchainPaths, CandidatesWithoutTheToolAreSkipped)
{
    const QStringList candidates = list({"/first", "/second"});
    // Both exist as directories, only the second carries the tool: the
    // predicate is what decides, not the order alone.
    EXPECT_EQ(firstWithTool(candidates, containing(list({"/second"}))),
              QStringLiteral("/second"));
}

TEST(ToolchainPaths, ACandidateThatIsNotThereIsNotTheAnswer)
{
    // The shape of the defect this file was rewritten for: the old code
    // answered with the compiled-in path whether or not it existed. Here the
    // candidate is checked against the real filesystem and is not there, so
    // the only acceptable answer is "not found".
    const QStringList candidates = {QStringLiteral("Z:/definitely/not/here/bin")};
    const auto exists = [](const QString& dir) {
        return QFileInfo::exists(
            QDir(dir).filePath(QStringLiteral("g++.exe")));
    };
    EXPECT_TRUE(firstWithTool(candidates, exists).isEmpty());
}

TEST(ToolchainPaths, NothingFoundIsEmptyAndNotTheFirstCandidate)
{
    const QStringList candidates = list({"/first", "/second"});
    EXPECT_TRUE(firstWithTool(candidates, containing(QStringList())).isEmpty());
}

TEST(ToolchainPaths, AnEmptyCandidateNeverMatches)
{
    // A predicate that says yes to everything, including "": the empty entry
    // must still be skipped, because an empty directory in the answer means
    // "resolve it through PATH", not "the current directory".
    const QStringList candidates = list({"", "/only"});
    const auto yes = [](const QString&) { return true; };
    EXPECT_EQ(firstWithTool(candidates, yes), QStringLiteral("/only"));
}

TEST(ToolchainPaths, NoCandidatesAtAllIsEmpty)
{
    const auto yes = [](const QString&) { return true; };
    EXPECT_TRUE(firstWithTool(QStringList(), yes).isEmpty());
}

// --- the real resolution, on whatever machine this runs on -----------------

TEST(ToolchainPaths, TheBuildBinEitherHoldsGxxOrSaysItDoesNotKnow)
{
    const QString bin = LightCpp::App::ToolchainPaths::mingwBin();
    if (bin.isEmpty()) {
        return;  // no MSYS2 anywhere: an honest answer, not a failure
    }
    EXPECT_TRUE(QFileInfo::exists(QDir(bin).filePath(QStringLiteral("g++.exe"))))
        << "mingwBin() answered " << bin.toStdString()
        << ", which holds no g++.exe";
}

TEST(ToolchainPaths, EveryToolPathIsEitherEmptyOrOnDisk)
{
    const QList<QPair<const char*, QString>> tools = {
        {"clangd", LightCpp::App::ToolchainPaths::clangdPath()},
        {"clang-format", LightCpp::App::ToolchainPaths::clangFormatPath()},
        {"git", LightCpp::App::ToolchainPaths::gitPath()},
    };
    for (const auto& tool : tools) {
        if (tool.second.isEmpty()) {
            continue;
        }
        EXPECT_TRUE(QFileInfo::exists(tool.second))
            << tool.first << " answered " << tool.second.toStdString()
            << ", which does not exist";
    }
}

TEST(ToolchainPaths, AToolFoundInTheBuildBinComesFromThatBin)
{
    // Same prefix as the compiler unless the build bin has no copy: a
    // clangd from another install would disagree with the headers g++
    // actually uses.
    const QString bin = LightCpp::App::ToolchainPaths::mingwBin();
    const QString clangd = LightCpp::App::ToolchainPaths::clangdPath();
    if (bin.isEmpty() || clangd.isEmpty()) {
        return;
    }
    const QString inBin = QDir(bin).filePath(QStringLiteral("clangd.exe"));
    if (QFileInfo::exists(inBin)) {
        EXPECT_EQ(QFileInfo(clangd).canonicalFilePath(),
                  QFileInfo(inBin).canonicalFilePath());
    }
}
