// CMakeProjectTests — locating a CMake project and shaping its command lines.
//
// Everything happens in a QTemporaryDir so the real source tree is never
// touched, and so "does it climb out of the project" can be tested by
// building actual directory trees.

#include "LightCpp/Project/CMakeProject.hpp"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace {

using LightCpp::Project::buildArguments;
using LightCpp::Project::buildDirectory;
using LightCpp::Project::builtExecutables;
using LightCpp::Project::CMakeProject;
using LightCpp::Project::configureArguments;
using LightCpp::Project::findCMakeProject;
using LightCpp::Project::isConfigured;
using LightCpp::Project::newestExecutable;
using LightCpp::Project::preferredExecutable;
using LightCpp::Project::BuildConfiguration;

/// Writes a file, creating any missing directories. Returns its path.
QString writeFile(const QString& root, const QString& relative,
                  const QByteArray& content = QByteArray("[project]\n"))
{
    const QString path = QDir(root).filePath(relative);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
    file.write(content);
    file.close();
    return path;
}

QString executableName(const QString& stem)
{
#ifdef Q_OS_WIN
    return stem + QStringLiteral(".exe");
#else
    return stem;
#endif
}

QString writeExecutable(const QString& root, const QString& stem,
                        const QByteArray& content = QByteArray("executable"))
{
    const QString path = writeFile(root, executableName(stem), content);
#ifndef Q_OS_WIN
    QFile file(path);
    if (file.open(QIODevice::ReadWrite)) {
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                            | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                            | QFileDevice::ExeGroup | QFileDevice::ReadOther
                            | QFileDevice::ExeOther);
    }
#endif
    return path;
}

/// Makes a file look older/newer than its neighbours, so "newest" is decided
/// by an explicit timestamp instead of by how fast the test ran.
void setModified(const QString& path, int secondsFromNow)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        return;
    }
    file.setFileTime(QDateTime::currentDateTime().addSecs(secondsFromNow),
                     QFileDevice::FileModificationTime);
    file.close();
}

CMakeProject projectAt(const QString& dir)
{
    return CMakeProject{dir};
}

}  // namespace

TEST(CMakeProject, FindsTheProjectInTheGivenDirectory)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeFile(temp.path(), QStringLiteral("CMakeLists.txt"));

    const auto found = findCMakeProject(temp.path());
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->sourceDir, QDir(temp.path()).absolutePath());
}

TEST(CMakeProject, ClimbsUpToTheProjectRoot)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeFile(temp.path(), QStringLiteral("CMakeLists.txt"));
    const QString deep = QDir(temp.path()).filePath(QStringLiteral("src/app/detail"));
    QDir().mkpath(deep);

    const auto found = findCMakeProject(deep);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->sourceDir, QDir(temp.path()).absolutePath());
}

TEST(CMakeProject, AcceptsAFilePathAtTheStartDirectory)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeFile(temp.path(), QStringLiteral("CMakeLists.txt"));
    writeFile(temp.path(), QStringLiteral("src/main.cpp"));

    // Called with a file rather than a directory — the search starts in the
    // file's own directory.
    const auto found = findCMakeProject(
        QDir(temp.path()).filePath(QStringLiteral("src/main.cpp")));
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->sourceDir, QDir(temp.path()).absolutePath());
}

TEST(CMakeProject, DoesNotClimbPastARepositoryRoot)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    // A CMakeLists.txt above the repository must not be adopted as "the
    // project" just because it is an ancestor.
    writeFile(temp.path(), QStringLiteral("CMakeLists.txt"));
    const QString repo = QDir(temp.path()).filePath(QStringLiteral("repo"));
    QDir().mkpath(QDir(repo).filePath(QStringLiteral(".git")));
    QDir().mkpath(QDir(repo).filePath(QStringLiteral("src")));

    EXPECT_FALSE(findCMakeProject(QDir(repo).filePath(QStringLiteral("src"))).has_value());
}

TEST(CMakeProject, RespectsTheDepthLimit)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeFile(temp.path(), QStringLiteral("CMakeLists.txt"));
    const QString deep = QDir(temp.path()).filePath(QStringLiteral("a/b/c"));
    QDir().mkpath(deep);

    EXPECT_FALSE(findCMakeProject(deep, 1).has_value());
    EXPECT_TRUE(findCMakeProject(deep, 3).has_value());
}

TEST(CMakeProject, MissingOrEmptyStartDirectoryYieldsNothing)
{
    EXPECT_FALSE(findCMakeProject(QString()).has_value());
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    EXPECT_FALSE(findCMakeProject(
        QDir(temp.path()).filePath(QStringLiteral("missing/directory"))).has_value());
}

TEST(CMakeProject, BuildTreeIsSeparatePerConfiguration)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString root = temp.path();
    const CMakeProject project = projectAt(root);
    EXPECT_EQ(buildDirectory(project, BuildConfiguration::Debug),
              QDir(root).filePath(QStringLiteral("build/Debug")));
    EXPECT_EQ(buildDirectory(project, BuildConfiguration::Release),
              QDir(root).filePath(QStringLiteral("build/Release")));
}

TEST(CMakeProject, ConfigureArgumentsSelectTheBuildType)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString root = temp.path();
    const CMakeProject project = projectAt(root);
    EXPECT_EQ(configureArguments(project, BuildConfiguration::Release),
              QStringList({QStringLiteral("-S"), root,
                           QStringLiteral("-B"), QDir(root).filePath("build/Release"),
                           QStringLiteral("-G"), QStringLiteral("Ninja"),
                           QStringLiteral("-DCMAKE_BUILD_TYPE=Release"),
                           QStringLiteral("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")}));
}

TEST(CMakeProject, ConfigureArgumentsCanOmitTheGenerator)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const CMakeProject project = projectAt(temp.path());
    const QStringList args = configureArguments(project, BuildConfiguration::Debug,
                                                QString(), false);
    EXPECT_FALSE(args.contains(QStringLiteral("-G")));
    EXPECT_FALSE(args.contains(QStringLiteral("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")));
    EXPECT_TRUE(args.contains(QStringLiteral("-DCMAKE_BUILD_TYPE=Debug")));
}

TEST(CMakeProject, BuildArgumentsCarryTheConfiguration)
{
    // Single-config generators take the build type from the cache and ignore
    // --config; multi-config generators use --config and ignore the cache.
    // Sending both is what makes one command line right for either.
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString root = temp.path();
    const CMakeProject project = projectAt(root);
    EXPECT_EQ(buildArguments(project, BuildConfiguration::Debug),
              QStringList({QStringLiteral("--build"),
                           QDir(root).filePath("build/Debug"),
                           QStringLiteral("--config"), QStringLiteral("Debug")}));
}

TEST(CMakeProject, IsConfiguredFollowsTheCacheFile)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const CMakeProject project = projectAt(temp.path());

    EXPECT_FALSE(isConfigured(project, BuildConfiguration::Debug));
    writeFile(temp.path(), QStringLiteral("build/Debug/CMakeCache.txt"));
    EXPECT_TRUE(isConfigured(project, BuildConfiguration::Debug));
    // The other configuration is still unconfigured: a cache belongs to
    // exactly one build type.
    EXPECT_FALSE(isConfigured(project, BuildConfiguration::Release));
}

TEST(CMakeProject, BuiltExecutablesSkipsCompilerProbes)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString tree = QDir(temp.path()).filePath(QStringLiteral("build/Debug"));
    const QString app = writeExecutable(temp.path(),
                                        QStringLiteral("build/Debug/app"));
    writeExecutable(temp.path(),
                    QStringLiteral("build/Debug/CMakeFiles/3.24/cmTC_1a2b"));
    writeFile(temp.path(), QStringLiteral("build/Debug/helper.dll"));
    // A directory containing a file is not itself an executable artifact.
    writeFile(temp.path(), QStringLiteral("build/Debug/bundle/inside.txt"));
    setModified(app, -10);

    const QStringList found = builtExecutables(tree);
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found.first(), app);
}

TEST(CMakeProject, NewestExecutableWins)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString tree = QDir(temp.path()).filePath(QStringLiteral("build/Debug"));
    const QString older = writeExecutable(temp.path(),
                                          QStringLiteral("build/Debug/older"));
    const QString newer = writeExecutable(temp.path(),
                                          QStringLiteral("build/Debug/newer"));
    setModified(older, -3600);
    setModified(newer, -10);

    EXPECT_EQ(newestExecutable(tree), newer);
    EXPECT_EQ(builtExecutables(tree).first(), newer);
}

TEST(CMakeProject, EmptyOrMissingBuildTreeHasNoExecutables)
{
    EXPECT_TRUE(builtExecutables(QString()).isEmpty());
    EXPECT_TRUE(newestExecutable(QString()).isEmpty());

    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    EXPECT_TRUE(builtExecutables(
        QDir(temp.path()).filePath(QStringLiteral("missing/build"))).isEmpty());
    EXPECT_TRUE(builtExecutables(temp.path()).isEmpty());
    EXPECT_TRUE(newestExecutable(temp.path()).isEmpty());
}


// --- preferredExecutable -----------------------------------------------

/// A configured build tree: CMakeCache.txt with (or without) the top-level
/// project name, plus the exes the two-target shape produces.
void makeBuildTree(const QString& root, const char* cacheProjectLine)
{
    writeFile(root, "CMakeCache.txt",
              QByteArray(cacheProjectLine) + QByteArray("\nother:flag=x\n"));
    writeExecutable(root, "hello", QByteArray("main"));
    writeExecutable(root, "test_runner", QByteArray("test"));
}

TEST(CMakeProject, PreferredExecutablePicksTheProjectNamesake)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString tree = dir.filePath("build/Debug");
    makeBuildTree(tree,
                  "CMAKE_PROJECT_NAME:STATIC=hello");
    // Make the test runner the NEWEST executable: "newest of everything"
    // would pick it, the project-name match must beat that.
    setModified(QDir(tree).filePath(executableName(QStringLiteral("hello"))), -60);
    setModified(QDir(tree).filePath(executableName(QStringLiteral("test_runner"))), 0);

    EXPECT_EQ(preferredExecutable(tree),
              QDir(tree).filePath("hello.exe"));
}

TEST(CMakeProject, PreferredExecutableFallsBackToNewestWithoutAName)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString tree = dir.filePath("build/Debug");
    makeBuildTree(tree, "CMAKE_GENERATOR:INTERNAL=Ninja");
    setModified(QDir(tree).filePath("hello.exe"), -60);
    setModified(QDir(tree).filePath("test_runner.exe"), 0);

    EXPECT_EQ(preferredExecutable(tree),
              QDir(tree).filePath("test_runner.exe"));
}

TEST(CMakeProject, PreferredExecutableIsNewestWhenItIsTheOnlyOne)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString tree = dir.filePath("build/Debug");
    writeFile(tree, "CMakeCache.txt", "CMAKE_PROJECT_NAME:STATIC=hello\n");
    const QString tool = writeExecutable(tree, "tool");

    EXPECT_EQ(preferredExecutable(tree), tool);
}

