// FolderContentsTests — deciding what a directory the user opened actually is.
//
// The failure this guards against is silent: a folder that *is* a project but
// is classified as an ordinary folder still fills the Explorer in correctly,
// so the only symptom is Build and Run staying greyed out for no visible
// reason. Each case here is a directory tree built for real in a temp dir.

#include "LightCpp/Project/FolderContents.hpp"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace {

using LightCpp::Project::isDirectory;
using LightCpp::Project::projectFileIn;

/// Writes a file, creating any missing directories. Returns its path.
QString writeFile(const QString& root, const QString& relative,
                  const QByteArray& content = QByteArray())
{
    const QString path = QDir(root).filePath(relative);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return {};
    }
    if (!content.isEmpty()) {
        file.write(content);
    }
    file.close();
    return path;
}

/// A minimal but *valid* .dev file — isValid() requires a compilable unit, and
/// "is this a project" is the question being asked here.
QByteArray devContent()
{
    return QByteArray("[Project]\n"
                      "FileName=hello.dev\n"
                      "Name=hello\n"
                      "Type=0\n"
                      "IsCpp=1\n"
                      "UnitCount=1\n"
                      "[Unit1]\n"
                      "FileName=main.cpp\n"
                      "CompileCpp=1\n"
                      "Compile=1\n"
                      "Link=1\n");
}

}  // namespace

TEST(FolderContents, ADirectoryIsRecognised)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    EXPECT_TRUE(isDirectory(temp.path()));
    EXPECT_FALSE(isDirectory(QDir(temp.path()).filePath(QStringLiteral("nope"))));
    EXPECT_FALSE(isDirectory(QString()));
}

TEST(FolderContents, ACmakeFileInTheDirectoryIsFound)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString cmake = writeFile(temp.path(), QStringLiteral("CMakeLists.txt"));

    const auto found = projectFileIn(temp.path());
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found->isCMake);
    EXPECT_TRUE(found->isValid());
    // Resolve through QFileInfo on both sides: QDir::absolutePath() upper-cases
    // the drive letter, which would make this a test of Qt's spelling rather
    // than of which directory was found.
    EXPECT_EQ(QFileInfo(found->filePath).absolutePath(),
              QFileInfo(cmake).absolutePath());
    EXPECT_EQ(QFileInfo(found->filePath).fileName(), QStringLiteral("CMakeLists.txt"));
}

TEST(FolderContents, ACmakeFileIsFoundWhateverItsCase)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeFile(temp.path(), QStringLiteral("cmakelists.TXT"));

    const auto found = projectFileIn(temp.path());
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found->isCMake);
    EXPECT_EQ(QFileInfo(found->filePath).fileName(), QStringLiteral("cmakelists.TXT"));
}

TEST(FolderContents, ADevFileInTheDirectoryIsFound)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeFile(temp.path(), QStringLiteral("hello.dev"), devContent());

    const auto found = projectFileIn(temp.path());
    ASSERT_TRUE(found.has_value());
    EXPECT_FALSE(found->isCMake);
    EXPECT_EQ(QFileInfo(found->filePath).fileName(), QStringLiteral("hello.dev"));
}

TEST(FolderContents, CmakeWinsWhenBothArePresent)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeFile(temp.path(), QStringLiteral("CMakeLists.txt"));
    writeFile(temp.path(), QStringLiteral("hello.dev"), devContent());

    // Matches openProjectFile()'s own precedence, so a folder opened one way
    // and a descriptor opened the other way cannot disagree.
    const auto found = projectFileIn(temp.path());
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found->isCMake);
}

TEST(FolderContents, SeveralDevFilesAreChosenPredictably)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeFile(temp.path(), QStringLiteral("zulu.dev"), devContent());
    writeFile(temp.path(), QStringLiteral("alpha.dev"), devContent());

    const auto found = projectFileIn(temp.path());
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(QFileInfo(found->filePath).fileName(), QStringLiteral("alpha.dev"));
}

TEST(FolderContents, AnOrdinaryDirectoryHasNoProject)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeFile(temp.path(), QStringLiteral("main.cpp"));
    writeFile(temp.path(), QStringLiteral("README.md"));
    QDir(temp.path()).mkdir(QStringLiteral("src"));

    EXPECT_FALSE(projectFileIn(temp.path()).has_value());
}

TEST(FolderContents, ADescriptorInsideASubdirectoryDoesNotMakeItAProject)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    // A folder holding an unrelated project further down — the parent itself
    // is not one, and opening it must not silently adopt the child.
    writeFile(temp.path(), QStringLiteral("vendor/lib/CMakeLists.txt"));
    writeFile(temp.path(), QStringLiteral("vendor/tool/thing.dev"), devContent());

    EXPECT_FALSE(projectFileIn(temp.path()).has_value());
    // ...while the child directory itself is recognised, so the rule is
    // "directly inside", not "refuses to look at all".
    const auto child = projectFileIn(QDir(temp.path()).filePath(QStringLiteral("vendor/lib")));
    ASSERT_TRUE(child.has_value());
    EXPECT_TRUE(child->isCMake);
}

TEST(FolderContents, TheDevSuffixIsMatchedCaseInsensitively)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeFile(temp.path(), QStringLiteral("HELLO.DEV"), devContent());

    const auto found = projectFileIn(temp.path());
    ASSERT_TRUE(found.has_value());
    EXPECT_FALSE(found->isCMake);
    EXPECT_EQ(QFileInfo(found->filePath).fileName(), QStringLiteral("HELLO.DEV"));
}

TEST(FolderContents, ANonexistentDirectoryIsNotAProject)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString missing = QDir(temp.path()).filePath(QStringLiteral("gone"));
    EXPECT_FALSE(projectFileIn(missing).has_value());
}

TEST(FolderContents, AFilePathIsNotADirectory)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString file = writeFile(temp.path(), QStringLiteral("CMakeLists.txt"));

    // Handing a path to the file itself must not open the directory it is in:
    // the caller asked about this path.
    EXPECT_FALSE(projectFileIn(file).has_value());
}

TEST(FolderContents, DirectoriesAreNotMistakenForDescriptors)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    // A directory called CMakeLists.txt or x.dev is not a project descriptor —
    // trying to parse one would fail on read rather than here.
    QDir(temp.path()).mkdir(QStringLiteral("CMakeLists.txt"));
    QDir(temp.path()).mkdir(QStringLiteral("thing.dev"));

    EXPECT_FALSE(projectFileIn(temp.path()).has_value());
}
