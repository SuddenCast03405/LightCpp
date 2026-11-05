// CompileDatabaseTests — which directory the project's database is found in.
//
// R2-24: the search stopped two levels below the project root, so CMake's
// preset default — `out/build/<preset>` — was never reached, and clangd then
// started against the synthesized compile_flags.txt instead. Every project
// include turned into a red squiggle ("file not found"), the project's own -I
// flags were gone, and the language server's answers were built on a database
// that described nothing.
//
// The trees here are real directories: the search is a filesystem question, and
// a stubbed lister would let the answer disagree with what QDir does.

#include "LightCpp/App/CompileDatabase.hpp"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

using LightCpp::App::CompileDatabase::findProjectDirectory;

namespace {

/// Create `relative` under the temporary root and put a database in it.
void writeDatabase(const QString& root, const QString& relative)
{
    const QString dir = QDir(root).filePath(relative);
    ASSERT_TRUE(QDir().mkpath(dir));
    QFile file(QDir(dir).filePath(QStringLiteral("compile_commands.json")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("[]");
}

}  // namespace

TEST(CompileDatabase, APlainBuildDirectoryIsFound)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeDatabase(temp.path(), QStringLiteral("build"));

    EXPECT_EQ(findProjectDirectory(temp.path()),
              QDir(QDir(temp.path()).filePath(QStringLiteral("build"))).absolutePath());
}

TEST(CompileDatabase, APresetDirectoryUnderOutIsFound)
{
    // The reported case: CMake presets default to out/build/<preset>.
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeDatabase(temp.path(), QStringLiteral("out/build/msys2-ucrt64"));

    const QString expected = QDir(QDir(temp.path())
                                      .filePath(QStringLiteral("out/build/msys2-ucrt64")))
                                 .absolutePath();
    EXPECT_EQ(findProjectDirectory(temp.path()), expected);
}

TEST(CompileDatabase, AConfigurationDirectoryIsFound)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeDatabase(temp.path(), QStringLiteral("build/Debug"));

    EXPECT_EQ(QFileInfo(findProjectDirectory(temp.path())).fileName(), QStringLiteral("Debug"));
}

TEST(CompileDatabase, ABuildRootWithAnOperatingSystemLevelIsFound)
{
    // build/<os>/<config>: also three levels, also under a build root.
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeDatabase(temp.path(), QStringLiteral("build/linux/release"));

    EXPECT_EQ(QFileInfo(findProjectDirectory(temp.path())).fileName(), QStringLiteral("release"));
}

TEST(CompileDatabase, TheProjectRootWinsOverABuildDirectory)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeDatabase(temp.path(), QString());
    writeDatabase(temp.path(), QStringLiteral("build"));

    EXPECT_EQ(findProjectDirectory(temp.path()), QDir(temp.path()).absolutePath());
}

TEST(CompileDatabase, TheShallowestDatabaseWinsAcrossBranches)
{
    // aaa/build (depth 2) and zzz (depth 1): the answer must not depend on
    // which branch happens to be walked first.
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeDatabase(temp.path(), QStringLiteral("aaa/build"));
    writeDatabase(temp.path(), QStringLiteral("zzz"));

    EXPECT_EQ(QFileInfo(findProjectDirectory(temp.path())).fileName(), QStringLiteral("zzz"));
}

TEST(CompileDatabase, APlainBuildDirectoryBeatsItsOwnPresetChild)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeDatabase(temp.path(), QStringLiteral("build"));
    writeDatabase(temp.path(), QStringLiteral("build/msys2-ucrt64"));

    EXPECT_EQ(QFileInfo(findProjectDirectory(temp.path())).fileName(), QStringLiteral("build"));
}

TEST(CompileDatabase, ASourceDirectoryIsNotWalkedThreeDeep)
{
    // The cost control, pinned: only a build root is walked past depth 2, so a
    // database three levels down inside the sources is not found. CMake does
    // not write one there, and walking every such directory would stat
    // thousands of them on every language-server start.
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeDatabase(temp.path(), QStringLiteral("src/impl/detail"));

    EXPECT_TRUE(findProjectDirectory(temp.path()).isEmpty());
}

TEST(CompileDatabase, AProjectWithoutADatabaseIsEmpty)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    ASSERT_TRUE(QDir().mkpath(QDir(temp.path()).filePath(QStringLiteral("src"))));

    EXPECT_TRUE(findProjectDirectory(temp.path()).isEmpty());
}

TEST(CompileDatabase, AnEmptyOrMissingRootIsEmpty)
{
    EXPECT_TRUE(findProjectDirectory(QString()).isEmpty());
    EXPECT_TRUE(findProjectDirectory(QStringLiteral("E:/no/such/project")).isEmpty());
}
