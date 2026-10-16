// GTest coverage for the recursive toolchain folder scan.

#include "LightCpp/Settings/ToolchainScanner.hpp"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using LightCpp::Settings::Toolchain;
using LightCpp::Settings::ToolchainScanOptions;
using LightCpp::Settings::sameTools;
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
    // root -> 1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 7 -> 8 -> 9 -> 10: the compiler
    // sits ten levels below the root, one past the default depth (9) that
    // exists to reach the Visual Studio layout.
    ASSERT_TRUE(touch(root + "/1/2/3/4/5/6/7/8/9/10/gcc.exe"));
    ASSERT_TRUE(touch(root + "/1/2/3/4/5/6/7/8/9/10/g++.exe"));

    EXPECT_TRUE(scanForToolchains(root).isEmpty());

    ToolchainScanOptions options;
    options.maxDepth = 10;
    const QList<Toolchain> found = scanForToolchains(root, options);
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found[0].compilers.value("gcc"),
              root + "/1/2/3/4/5/6/7/8/9/10/gcc.exe");
}

TEST(ToolchainScannerTests, TheDefaultDepthReachesAVisualStudioInstall)
{
    // The layout that actually ships: the user picks the "Microsoft Visual
    // Studio" folder as the root, and cl.exe sits nine levels down. The old
    // default of 5 answered "no compilers found" for every VS install
    // (R2-34), so this pins the depth that the default has to cover.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath("Microsoft Visual Studio");
    const QString clDir = root
                          + "/2022/Community/VC/Tools/MSVC/14.44.35207/bin"
                            "/Hostx64/x64";
    ASSERT_TRUE(touch(clDir + "/cl.exe"));

    const QList<Toolchain> found = scanForToolchains(root);
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found[0].compilers.value("cl"), clDir + "/cl.exe");
    // One level less and the walk must stop short: the depth is what makes
    // the difference, not a lucky exclusion rule.
    ToolchainScanOptions shallower;
    shallower.maxDepth = 8;
    EXPECT_TRUE(scanForToolchains(root, shallower).isEmpty());

    // An msys2-style install, for contrast, is only three levels down and
    // was already reachable at the old default.
    const QString msysRoot = dir.filePath("msys64");
    ASSERT_TRUE(touch(msysRoot + "/ucrt64/bin/g++.exe"));
    const QList<Toolchain> msys = scanForToolchains(msysRoot);
    ASSERT_EQ(msys.size(), 1);
    EXPECT_EQ(msys[0].compilers.value("g++"),
              msysRoot + "/ucrt64/bin/g++.exe");
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
    QStringList names;
    for (const Toolchain& toolchain : found) {
        names.append(toolchain.name);
        if (toolchain.compilers.value("g++").endsWith("mingw32/bin/g++.exe")) {
            // A directory with only a C++ compiler still counts.
            EXPECT_TRUE(toolchain.compilers.value("gcc").isEmpty());
        } else {
            EXPECT_EQ(toolchain.compilers.value("gcc"),
                      root + "/ucrt64/bin/gcc.exe");
            EXPECT_EQ(toolchain.compilers.value("g++"),
                      root + "/ucrt64/bin/g++.exe");
        }
    }
    // Both folders are called "bin"; the picker has to tell them apart, so
    // each name carries one parent segment (R2-35).
    names.sort();
    EXPECT_EQ(names, (QStringList{QStringLiteral("mingw32/bin"),
                                  QStringLiteral("ucrt64/bin")}));
}

TEST(ToolchainScannerTests, OnlyCollidingNamesAreQualified)
{
    // Three toolchains, two of them in a folder called `bin` and one in a
    // folder whose name nobody else uses. Disambiguation must touch the
    // collision only: an untouched name is what the user already knows.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath("sdk");
    ASSERT_TRUE(touch(root + "/ucrt64/bin/gcc.exe"));
    ASSERT_TRUE(touch(root + "/clang64/bin/clang.exe"));
    ASSERT_TRUE(touch(root + "/vendor-llvm/g++.exe"));

    const QList<Toolchain> found = scanForToolchains(root);
    ASSERT_EQ(found.size(), 3);
    QStringList names;
    for (const Toolchain& toolchain : found) {
        names.append(toolchain.name);
    }
    names.sort();
    EXPECT_EQ(names, (QStringList{QStringLiteral("clang64/bin"),
                                  QStringLiteral("ucrt64/bin"),
                                  QStringLiteral("vendor-llvm")}));
}

TEST(ToolchainScannerTests, NamesAreExtendedOneSegmentAtATime)
{
    // Two `bin` folders whose parents happen to match as well: the first
    // segment that differs is the third one, so the qualified names are
    // "sdk/x/bin" and "y/x/bin" — not the whole path, and not a bare "bin".
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath("sdk");
    ASSERT_TRUE(touch(root + "/x/bin/gcc.exe"));
    ASSERT_TRUE(touch(root + "/y/x/bin/gcc.exe"));

    const QList<Toolchain> found = scanForToolchains(root);
    ASSERT_EQ(found.size(), 2);
    QStringList names;
    for (const Toolchain& toolchain : found) {
        names.append(toolchain.name);
    }
    names.sort();
    EXPECT_EQ(names, (QStringList{QStringLiteral("sdk/x/bin"),
                                  QStringLiteral("y/x/bin")}));
}

TEST(ToolchainScannerTests, SameToolsIgnoresIdNameAndEmptyValues)
{
    // A stored toolchain versus a freshly scanned one: same executables, but
    // the id and the display name differ by construction. Empty entries are
    // "not there" on either side, never "different" (R2-35).
    Toolchain stored;
    stored.id = QStringLiteral("user-toolchain-1");
    stored.name = QStringLiteral("My MSYS2");
    stored.compilers.insert(QStringLiteral("gcc"),
                            QStringLiteral("E:/msys64/ucrt64/bin/gcc.exe"));
    stored.compilers.insert(QStringLiteral("g++"),
                            QStringLiteral("E:/msys64/ucrt64/bin/g++.exe"));
    stored.buildTools.insert(QStringLiteral("cmake"),
                             QStringLiteral("E:/msys64/ucrt64/bin/cmake.exe"));
    stored.buildTools.insert(QStringLiteral("ninja"), QString());

    Toolchain scanned;
    scanned.id = QStringLiteral("found-toolchain-1");
    scanned.name = QStringLiteral("bin");
    scanned.compilers.insert(QStringLiteral("gcc"),
                             QStringLiteral("E:/msys64/ucrt64/bin/gcc.exe"));
    scanned.compilers.insert(QStringLiteral("g++"),
                             QStringLiteral("E:/msys64/ucrt64/bin/g++.exe"));
    scanned.buildTools.insert(QStringLiteral("cmake"),
                              QStringLiteral("E:/msys64/ucrt64/bin/cmake.exe"));

    EXPECT_TRUE(sameTools(stored, scanned));
    EXPECT_TRUE(sameTools(scanned, stored));
    // A toolchain compares equal to itself whatever it holds.
    EXPECT_TRUE(sameTools(stored, stored));

    Toolchain otherPath = scanned;
    otherPath.compilers.insert(QStringLiteral("gcc"),
                               QStringLiteral("E:/other/bin/gcc.exe"));
    EXPECT_FALSE(sameTools(stored, otherPath));

    Toolchain missingTool = scanned;
    missingTool.buildTools.remove(QStringLiteral("cmake"));
    EXPECT_FALSE(sameTools(stored, missingTool));

    Toolchain extraDebugger = scanned;
    extraDebugger.compilers.insert(
        QStringLiteral("gdb"), QStringLiteral("E:/msys64/ucrt64/bin/gdb.exe"));
    EXPECT_FALSE(sameTools(stored, extraDebugger));

    Toolchain differentClangd = scanned;
    differentClangd.clangd = QStringLiteral("E:/msys64/ucrt64/bin/clangd.exe");
    EXPECT_FALSE(sameTools(stored, differentClangd));
}

TEST(ToolchainScannerTests, ARescanOfTheSameTreeMatchesTheStoredToolchain)
{
    // End to end for the "Identify tools…" dedup: scan, store the result
    // (letting the caller assign its own id and let the user rename it),
    // scan again, and the two must compare equal. Otherwise every rescan
    // appends another copy of the same toolchain.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.filePath("msys64");
    ASSERT_TRUE(touch(root + "/ucrt64/bin/gcc.exe"));
    ASSERT_TRUE(touch(root + "/ucrt64/bin/g++.exe"));
    ASSERT_TRUE(touch(root + "/ucrt64/bin/gdb.exe"));

    const QList<Toolchain> first = scanForToolchains(root);
    ASSERT_EQ(first.size(), 1);
    Toolchain stored = first.first();
    stored.id = QStringLiteral("found-toolchain-1");
    stored.name = QStringLiteral("renamed by the user");

    const QList<Toolchain> second = scanForToolchains(root);
    ASSERT_EQ(second.size(), 1);
    EXPECT_TRUE(sameTools(stored, second.first()));

    // ...and a *different* tree is not mistaken for it.
    const QTemporaryDir otherDir;
    const QString otherRoot = otherDir.filePath("msys64");
    ASSERT_TRUE(touch(otherRoot + "/ucrt64/bin/gcc.exe"));
    ASSERT_TRUE(touch(otherRoot + "/ucrt64/bin/g++.exe"));
    const QList<Toolchain> other = scanForToolchains(otherRoot);
    ASSERT_EQ(other.size(), 1);
    EXPECT_FALSE(sameTools(stored, other.first()));
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
