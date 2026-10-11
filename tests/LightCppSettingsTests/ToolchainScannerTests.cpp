// GTest coverage for the recursive toolchain folder scan.

#include "LightCpp/Settings/ToolchainScanner.hpp"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using LightCpp::Settings::Toolchain;
using LightCpp::Settings::ToolchainScanOptions;
using LightCpp::Settings::scanForToolchains;

namespace {

/// Create an empty file, creating parent directories as needed. The scan
/// only checks existence, so an empty file behaves like a real executable.
bool touch(const QString& path)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly);
}

}  // namespace

TEST(ToolchainScannerTests, RootHostingCompilerFillsRoles)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath("sdk");
    ASSERT_TRUE(touch(root + "/gcc.exe"));
    ASSERT_TRUE(touch(root + "/g++.exe"));
    ASSERT_TRUE(touch(root + "/gdb.exe"));
    ASSERT_TRUE(touch(root + "/cmake.exe"));

    const QList<Toolchain> found = scanForToolchains(root);
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found[0].name, QLatin1String("sdk"));
    EXPECT_EQ(found[0].compilers.value("gcc"), root + "/gcc.exe");
    EXPECT_EQ(found[0].compilers.value("g++"), root + "/g++.exe");
    EXPECT_EQ(found[0].compilers.value("gdb"), root + "/gdb.exe");
    EXPECT_EQ(found[0].buildTools.value("cmake"), root + "/cmake.exe");
}

TEST(ToolchainScannerTests, RecursesIntoSubdirectories)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath("root");
    ASSERT_TRUE(touch(root + "/tools/nightly/bin/clang.exe"));
    ASSERT_TRUE(touch(root + "/tools/nightly/bin/clang++.exe"));

    const QList<Toolchain> found = scanForToolchains(root);
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found[0].name, QLatin1String("bin"));
    EXPECT_EQ(found[0].compilers.value("clang"),
              root + "/tools/nightly/bin/clang.exe");
    EXPECT_EQ(found[0].compilers.value("clang++"),
              root + "/tools/nightly/bin/clang++.exe");
}

TEST(ToolchainScannerTests, DefaultExclusionsAreSkipped)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath("root");
    ASSERT_TRUE(touch(root + "/lib/gcc.exe"));
    ASSERT_TRUE(touch(root + "/include/g++.exe"));
    ASSERT_TRUE(touch(root + "/share/tools/clang.exe"));
    ASSERT_TRUE(touch(root + "/var/cache/cl.exe"));

    EXPECT_TRUE(scanForToolchains(root).isEmpty());
}

TEST(ToolchainScannerTests, ExclusionNamesAreUserExtensibleAndCaseInsensitive)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath("root");
    ASSERT_TRUE(touch(root + "/vendor/gcc.exe"));
    ASSERT_TRUE(touch(root + "/LIB/keepout/g++.exe"));
    ASSERT_TRUE(touch(root + "/bin/clang.exe"));

    ToolchainScanOptions options;
    options.excludedDirectories.append("vendor");
    const QList<Toolchain> found = scanForToolchains(root, options);
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found[0].compilers.value("clang"), root + "/bin/clang.exe");
}

TEST(ToolchainScannerTests, DepthLimitIsRespected)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath("root");
    // root -> 1 -> 2 -> 3 -> 4 -> 5 -> 6: the compiler sits six levels
    // below the root.
    ASSERT_TRUE(touch(root + "/1/2/3/4/5/6/gcc.exe"));
    ASSERT_TRUE(touch(root + "/1/2/3/4/5/6/g++.exe"));

    EXPECT_TRUE(scanForToolchains(root).isEmpty());

    ToolchainScanOptions options;
    options.maxDepth = 6;
    const QList<Toolchain> found = scanForToolchains(root, options);
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found[0].compilers.value("gcc"),
              root + "/1/2/3/4/5/6/gcc.exe");
}

TEST(ToolchainScannerTests, MultipleDirectoriesProduceMultipleToolchains)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath("msys64");
    ASSERT_TRUE(touch(root + "/ucrt64/bin/gcc.exe"));
    ASSERT_TRUE(touch(root + "/ucrt64/bin/g++.exe"));
    ASSERT_TRUE(touch(root + "/mingw32/bin/g++.exe"));

    const QList<Toolchain> found = scanForToolchains(root);
    ASSERT_EQ(found.size(), 2);
    // Walk order follows the directory listing; match by path, not index.
    for (const Toolchain& toolchain : found) {
        if (toolchain.compilers.value("g++").endsWith("mingw32/bin/g++.exe")) {
            // A directory with only a C++ compiler still counts.
            EXPECT_TRUE(toolchain.compilers.value("gcc").isEmpty());
            EXPECT_TRUE(toolchain.name.isEmpty()
                        || toolchain.name == QLatin1String("bin"));
        } else {
            EXPECT_EQ(toolchain.compilers.value("gcc"),
                      root + "/ucrt64/bin/gcc.exe");
            EXPECT_EQ(toolchain.compilers.value("g++"),
                      root + "/ucrt64/bin/g++.exe");
        }
    }
}

TEST(ToolchainScannerTests, DirectoryWithoutCompilerIsNotAToolchain)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath("root");
    ASSERT_TRUE(touch(root + "/bin/gdb.exe"));
    ASSERT_TRUE(touch(root + "/bin/ninja.exe"));

    EXPECT_TRUE(scanForToolchains(root).isEmpty());
}

TEST(ToolchainScannerTests, WalkStopsBelowAToolchainDirectory)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath("root");
    ASSERT_TRUE(touch(root + "/ucrt64/bin/gcc.exe"));
    ASSERT_TRUE(touch(root + "/ucrt64/bin/g++.exe"));
    ASSERT_TRUE(touch(root + "/ucrt64/bin/inner/clang++.exe"));

    const QList<Toolchain> found = scanForToolchains(root);
    ASSERT_EQ(found.size(), 1);
}

TEST(ToolchainScannerTests, NonExistentRootYieldsNothing)
{
    EXPECT_TRUE(scanForToolchains("Z:/definitely/not/there").isEmpty());
}

TEST(ToolchainScannerTests, EveryCompilerFoundIsRecordedSeparately)
{
    // A bin hosting both families keeps all four compiler keys: the kit's
    // compilerFamily picks what it needs.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath("mixed");
    ASSERT_TRUE(touch(root + "/gcc.exe"));
    ASSERT_TRUE(touch(root + "/g++.exe"));
    ASSERT_TRUE(touch(root + "/clang.exe"));
    ASSERT_TRUE(touch(root + "/clang++.exe"));
    ASSERT_TRUE(touch(root + "/gdb.exe"));
    ASSERT_TRUE(touch(root + "/lldb.exe"));

    const QList<Toolchain> found = scanForToolchains(root);
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found[0].compilers.value("gcc"), root + "/gcc.exe");
    EXPECT_EQ(found[0].compilers.value("g++"), root + "/g++.exe");
    EXPECT_EQ(found[0].compilers.value("clang"), root + "/clang.exe");
    EXPECT_EQ(found[0].compilers.value("clang++"), root + "/clang++.exe");
    EXPECT_EQ(found[0].compilers.value("gdb"), root + "/gdb.exe");
    EXPECT_EQ(found[0].compilers.value("lldb"), root + "/lldb.exe");
}
