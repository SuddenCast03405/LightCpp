// PluginPathsTests — where the IDE looks for plugins. The resolution used
// to live inside MainWindow::loadPlugins() while the plugin panel and the
// enable/disable/uninstall actions rebuilt `<exeDir>/plugins` by hand, so
// running the raw build-tree executable listed nothing and toggled paths
// that did not exist (R2-17). These cases pin the resolution itself; the
// App layer no longer has an opinion of its own.

#include "LightCpp/Plugin/PluginPaths.hpp"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

/// Drop an empty file at `path` — the resolver only ever looks at names.
bool touch(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write("x");
    return file.flush();
}

/// Create `directory` holding one plugin library named `dllName`; returns
/// the directory, or an empty string when the fixture could not be built.
QString makePluginDirectory(const QString& directory, const QString& dllName)
{
    if (!QDir().mkpath(directory)) {
        return {};
    }
    if (!touch(QDir(directory).filePath(dllName))) {
        return {};
    }
    return directory;
}

/// Create `path` as a directory; returns it, or an empty string on failure.
QString makeDirectory(const QString& path)
{
    return QDir().mkpath(path) ? path : QString{};
}

}  // namespace

TEST(PluginPaths, DefaultIsAlwaysThePluginsDirectoryNextToTheExecutable)
{
    const QTemporaryDir dir;
    const QString exeDir = makeDirectory(dir.filePath(QStringLiteral("bin")));
    ASSERT_FALSE(exeDir.isEmpty());
    EXPECT_EQ(LightCpp::Plugin::defaultPluginsDirectory(exeDir),
              exeDir + QStringLiteral("/plugins"));
    // With nothing to find anywhere, the resolver hands back exactly that.
    EXPECT_EQ(LightCpp::Plugin::resolvePluginsDirectory(exeDir),
              LightCpp::Plugin::defaultPluginsDirectory(exeDir));
}

TEST(PluginPaths, PrefersTheDirectoryNextToTheExecutable)
{
    const QTemporaryDir dir;
    const QString exeDir = makeDirectory(dir.filePath(QStringLiteral("bin")));
    ASSERT_FALSE(exeDir.isEmpty());
    const QString beside =
        makePluginDirectory(dir.filePath(QStringLiteral("bin/plugins")),
                            QStringLiteral("libdevcppplugin.dll"));
    ASSERT_FALSE(beside.isEmpty());
    // Also present one level up: the one next to the exe still wins.
    ASSERT_FALSE(makePluginDirectory(dir.filePath(QStringLiteral("plugins")),
                                     QStringLiteral("libdevcppplugin.dll"))
                     .isEmpty());
    EXPECT_EQ(LightCpp::Plugin::resolvePluginsDirectory(exeDir), beside);
}

TEST(PluginPaths, TheDirectoryNextToTheExecutableNeedsNoPluginDllToWin)
{
    // The staged layout is authoritative the moment it exists: a plugins/
    // directory beside the exe is *the* plugins directory, empty or not.
    // Only the build-tree candidates have to prove themselves.
    const QTemporaryDir dir;
    const QString exeDir = makeDirectory(dir.filePath(QStringLiteral("bin")));
    ASSERT_FALSE(exeDir.isEmpty());
    const QString beside =
        makeDirectory(dir.filePath(QStringLiteral("bin/plugins")));
    ASSERT_FALSE(beside.isEmpty());
    ASSERT_FALSE(makePluginDirectory(dir.filePath(QStringLiteral("plugins")),
                                     QStringLiteral("libdevcppplugin.dll"))
                     .isEmpty());
    EXPECT_EQ(LightCpp::Plugin::resolvePluginsDirectory(exeDir), beside);
}

TEST(PluginPaths, FallsBackToTheBuildTreeDirectory)
{
    // Build-tree layout: the exe sits in <build>/src/LightCppApp while the
    // plugin dlls land in <build>/plugins. The real dll is named in lower
    // case, which is what makes the `*Plugin*.dll` filter interesting.
    const QTemporaryDir dir;
    const QString buildTree =
        makePluginDirectory(dir.filePath(QStringLiteral("plugins")),
                            QStringLiteral("libdevcppplugin.dll"));
    ASSERT_FALSE(buildTree.isEmpty());
    const QString exeDir =
        makeDirectory(dir.filePath(QStringLiteral("build/src/LightCppApp")));
    ASSERT_FALSE(exeDir.isEmpty());
    EXPECT_EQ(LightCpp::Plugin::resolvePluginsDirectory(exeDir), buildTree);
}

TEST(PluginPaths, IgnoresAnAncestorPluginsDirectoryWithoutPluginDlls)
{
    // An unrelated "plugins" folder further up (an IDE's, a browser's):
    // it must not hijack the search just because it is named right.
    const QTemporaryDir dir;
    ASSERT_TRUE(QDir().mkpath(dir.filePath(QStringLiteral("plugins"))));
    ASSERT_TRUE(touch(dir.filePath(QStringLiteral("plugins/other.dll"))));
    const QString exeDir =
        makeDirectory(dir.filePath(QStringLiteral("build/src/LightCppApp")));
    ASSERT_FALSE(exeDir.isEmpty());
    // Nothing qualified, so we get the exe-adjacent default back even
    // though it does not exist — the caller reports that.
    EXPECT_EQ(LightCpp::Plugin::resolvePluginsDirectory(exeDir),
              exeDir + QStringLiteral("/plugins"));
}

TEST(PluginPaths, TheNearestBuildTreeDirectoryWins)
{
    const QTemporaryDir dir;
    const QString nearest =
        makePluginDirectory(dir.filePath(QStringLiteral("a/plugins")),
                            QStringLiteral("libdevcppplugin.dll"));
    ASSERT_FALSE(nearest.isEmpty());
    ASSERT_FALSE(makePluginDirectory(dir.filePath(QStringLiteral("plugins")),
                                     QStringLiteral("libdevcppplugin.dll"))
                     .isEmpty());
    const QString exeDir = makeDirectory(dir.filePath(QStringLiteral("a/b/c")));
    ASSERT_FALSE(exeDir.isEmpty());
    EXPECT_EQ(LightCpp::Plugin::resolvePluginsDirectory(exeDir), nearest);
}

TEST(PluginPaths, StopsAtTheSearchLimit)
{
    const QTemporaryDir dir;
    const QString sixUp =
        makePluginDirectory(dir.filePath(QStringLiteral("plugins")),
                            QStringLiteral("libdevcppplugin.dll"));
    ASSERT_FALSE(sixUp.isEmpty());
    // Six levels down: the default four-level walk cannot get there...
    const QString exeDir =
        makeDirectory(dir.filePath(QStringLiteral("a/b/c/d/e/f")));
    ASSERT_FALSE(exeDir.isEmpty());
    const QString fallback = exeDir + QStringLiteral("/plugins");
    EXPECT_EQ(LightCpp::Plugin::resolvePluginsDirectory(exeDir), fallback);
    EXPECT_EQ(LightCpp::Plugin::resolvePluginsDirectory(exeDir, 3), fallback);
    // ...a limit that reaches it does, and zero never walks at all.
    EXPECT_EQ(LightCpp::Plugin::resolvePluginsDirectory(exeDir, 6), sixUp);
    EXPECT_EQ(LightCpp::Plugin::resolvePluginsDirectory(exeDir, 0), fallback);
}
