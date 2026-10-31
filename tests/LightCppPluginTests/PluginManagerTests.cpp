// PluginManagerTests — scanning, error reporting, and a full QPluginLoader
// round-trip of the DevCppPlugin built in this tree: load the DLL, open a
// .dev fixture and ask for build steps.

#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/PluginManager.hpp"
#include "LightCpp/Plugin/PluginTypes.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"

#include <gtest/gtest.h>

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPluginLoader>
#include <QLibrary>
#include <QTemporaryDir>

namespace {

QString testLibrarySuffix()
{
#ifdef Q_OS_WIN
    return QStringLiteral(".dll");
#else
    return QStringLiteral(".so");
#endif
}

/// The quietest possible services: plugins initialize against it, and tests
/// that care about one specific call derive from it.
class SilentServices : public LightCpp::Plugin::IdeServices {
public:
    void log(const QString&) override {}
    [[nodiscard]] QString activeEditorPath() const override { return {}; }
    [[nodiscard]] QString editorText() const override { return {}; }
    void setEditorText(const QString&) override {}
    void insertAtCursor(const QString&) override {}
    void gotoLine(int) override {}
    [[nodiscard]] QStringList actionIds() const override { return {}; }
    [[nodiscard]] QString actionText(const QString&) const override
    {
        return {};
    }
    void setActionText(const QString&, const QString&) override {}
    [[nodiscard]] QStringList menuIds() const override { return {}; }
    [[nodiscard]] QString menuTitle(const QString&) const override
    {
        return {};
    }
    void setMenuTitle(const QString&, const QString&) override {}
    void registerCommand(const QString&, const QString&, const QString&,
                         const QString&, std::function<void()>) override
    {
    }
};

QString writeDevProject(const QTemporaryDir& dir, const QString& content)
{
    const QString path = dir.filePath(QStringLiteral("hello.dev"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return {};
    }
    file.write(content.toUtf8());
    return path;
}

const char* kDevFixture =
    "[Project]\n"
    "FileName=hello.dev\n"
    "Name=hello\n"
    "Type=0\n"
    "IsCpp=1\n"
    "UnitCount=1\n"
    "ExeOutput=\n"
    "ObjectOutput=\n"
    "[Unit1]\n"
    "FileName=main.cpp\n"
    "Compile=1\n"
    "Link=1\n";

}  // namespace

TEST(PluginManager, ScanningAnEmptyDirectoryIsQuiet)
{
    const QTemporaryDir dir;
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors =
        manager.loadSharedPlugins(dir.path(), &services);
    EXPECT_TRUE(errors.isEmpty());
    EXPECT_TRUE(manager.loaded().isEmpty());
    EXPECT_TRUE(manager.backends().isEmpty());
}

TEST(PluginManager, AMissingPluginsDirectoryIsQuiet)
{
    // A directory with nothing in it yields no errors and no plugins. The
    // "a file that is not a plugin" case is *not* this one — it lives in
    // AnUnloadableLibraryReportsWhyItFailed below, which puts a real file in
    // front of the loader. This test used to be called
    // NonPluginDllsAreSkippedWithErrors and asserted nothing of the sort.
    const QTemporaryDir dir;
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        dir.filePath(QStringLiteral("does-not-exist")), &services);
    EXPECT_TRUE(errors.isEmpty());
    EXPECT_TRUE(manager.loaded().isEmpty());
}

#ifdef Q_OS_WIN
TEST(PluginManager, DevCppPluginLoadsOpensAndAnswersBuildSteps)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty())
        << errors.join(QStringLiteral("; ")).toStdString();

    // The Dev-C++ plugin is present and registered as a project backend.
    // The exact loaded count grows with the first-party plugin set (the
    // MSVC project plugin ships next to this one), so this test pins the
    // Dev-C++ entry rather than the total.
    ASSERT_FALSE(manager.loaded().isEmpty());
    ASSERT_FALSE(manager.backends().isEmpty());
    const auto devPlugin = std::find_if(manager.loaded().cbegin(),
                                        manager.loaded().cend(),
                                        [](const auto& entry) {
                                            return entry.name
                                                   == QStringLiteral(
                                                       "DevCppProject");
                                        });
    ASSERT_TRUE(devPlugin != manager.loaded().cend());
    EXPECT_TRUE(devPlugin->hasBackend);

    const QString filter = manager.projectFilters().join(QLatin1Char(';'));
    EXPECT_TRUE(filter.contains(QStringLiteral("*.dev")));

    auto* backend = manager.backendFor(QStringLiteral("anything.dev"));
    ASSERT_NE(backend, nullptr);
    EXPECT_EQ(backend->projectFilter(), QStringLiteral("Dev-C++ (*.dev)"));
    EXPECT_EQ(manager.backendFor(QStringLiteral("anything.txt")), nullptr);

    // Open a real .dev fixture through the plugin.
    const QTemporaryDir projectDir;
    const QString devPath = writeDevProject(projectDir, kDevFixture);
    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(devPath, &summary, &error))
        << error.toStdString();
    EXPECT_EQ(summary.name, QStringLiteral("hello"));
    EXPECT_EQ(summary.unitCount, 1);
    EXPECT_EQ(summary.buildUnitCount, 1);
    EXPECT_TRUE(summary.producesExecutable);
    EXPECT_EQ(summary.outputPath,
              projectDir.filePath(QStringLiteral("hello.exe")));

    // Build steps: the plugin regenerates the Dev-C++ style makefile, then
    // hands the build to make — not a direct compiler invocation.
    LightCpp::Plugin::BuildRequest request;
    request.configuration = QStringLiteral("Debug");
    request.compiler = QStringLiteral("E:/toolchain/g++.exe");
    request.make = QStringLiteral("E:/toolchain/mingw32-make.exe");
    const QList<LightCpp::Plugin::BuildStep> steps =
        backend->buildSteps(request);
    ASSERT_EQ(steps.size(), 1);
    EXPECT_EQ(steps.first().program,
              QStringLiteral("E:/toolchain/mingw32-make.exe"));
    EXPECT_TRUE(steps.first().arguments.contains(QStringLiteral("-f")));
    EXPECT_TRUE(steps.first().arguments.join(QLatin1Char(' '))
                    .contains(QStringLiteral("Makefile.win")));
    EXPECT_TRUE(steps.first().arguments.contains(QStringLiteral("all")));
    EXPECT_EQ(steps.first().workingDirectory, projectDir.path());
    // The makefile itself now exists, written where Dev-C++ writes it.
    EXPECT_TRUE(QFileInfo::exists(
        projectDir.filePath(QStringLiteral("Makefile.win"))));

    // Configure is a no-op for .dev — the file itself is the configuration.
    EXPECT_TRUE(backend->configureSteps(request).isEmpty());
}

// ---------------------------------------------------------------------------
// Registered-plugin hot unload
// ---------------------------------------------------------------------------

namespace {

class TrackedPlugin final : public LightCpp::Plugin::Plugin {
public:
    TrackedPlugin(QString name, int* shutdownCalls)
        : name_(std::move(name)), shutdownCalls_(shutdownCalls)
    {
    }
    [[nodiscard]] QString name() const override { return name_; }
    [[nodiscard]] QString version() const override { return "1.0"; }
    bool initialize(LightCpp::Plugin::IdeServices*) override { return true; }
    void shutdown() override { ++*shutdownCalls_; }

private:
    QString name_;
    int* shutdownCalls_;
};

}  // namespace

#endif  // Q_OS_WIN

TEST(PluginManager, RegisteredPluginHotUnloadsBySource)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;

    int shutdownCalls = 0;
    auto first = std::make_unique<TrackedPlugin>(QStringLiteral("alpha"),
                                                 &shutdownCalls);
    auto second = std::make_unique<TrackedPlugin>(QStringLiteral("beta"),
                                                  &shutdownCalls);
    manager.registerPlugin(std::move(first), &services, "C:/plugins/lua/a.lua");
    manager.registerPlugin(std::move(second), &services, "C:/plugins/lua/b.lua");
    ASSERT_EQ(manager.loaded().size(), 2);

    // Wrong source: nothing unloads, nothing shuts down.
    EXPECT_FALSE(manager.unloadRegistered("C:/plugins/lua/missing.lua"));
    EXPECT_EQ(manager.loaded().size(), 2);
    EXPECT_EQ(shutdownCalls, 0);

    // The matching source unloads exactly that plugin: shutdown() ran, the
    // registry and the loaded list both dropped it, the sibling stayed.
    EXPECT_TRUE(manager.unloadRegistered("C:/plugins/lua/b.lua"));
    EXPECT_EQ(shutdownCalls, 1);
    ASSERT_EQ(manager.loaded().size(), 1);
    EXPECT_EQ(manager.loaded().first().name, QStringLiteral("alpha"));

    // Unloading again finds nothing.
    EXPECT_FALSE(manager.unloadRegistered("C:/plugins/lua/b.lua"));
}

TEST(PluginManager, DisabledSharedPluginsAreSkipped)
{
    // A disable list entry that matches a dll file name keeps the loader
    // from even trying that dll — the point of the disable feature. Every
    // dll actually built into the plugin directory is listed, so nothing
    // loads at all. The names are discovered rather than hardcoded: they
    // differ between toolchains (MinGW prefixes `lib`, MSVC does not), and
    // the old hardcoded pair would silently list nothing on MSVC and pass
    // while testing nothing.
    const QDir pluginDir(QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR));
    QStringList dlls;
    for (const QFileInfo& info : pluginDir.entryInfoList(QDir::Files)) {
        if (QLibrary::isLibrary(info.filePath())) {
            dlls.append(info.fileName());
        }
    }
    if (dlls.isEmpty()) {
        GTEST_SKIP() << "no native shared plugin is built for this platform";
    }

    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services, dlls);
    EXPECT_TRUE(errors.isEmpty());
    EXPECT_TRUE(manager.loaded().isEmpty());
    EXPECT_TRUE(manager.backends().isEmpty());
}

// ---------------------------------------------------------------------------
// Failure paths (R2-47)
// ---------------------------------------------------------------------------

namespace {

/// SilentServices plus a record of every removePluginCommands() call: the
/// host is supposed to clean up after a plugin whose initialize() refused.
class RecordingServices final : public SilentServices {
public:
    void removePluginCommands(const QString& pluginName) override
    {
        removals.append(pluginName);
    }

    QStringList removals;
};

/// A plugin that refuses to initialize after registering a menu entry —
/// the shape of a script that failed halfway through. It cannot undo its
/// registrations itself, so the host has to.
class RefusingPlugin final : public LightCpp::Plugin::Plugin {
public:
    explicit RefusingPlugin(QString name) : name_(std::move(name)) {}

    [[nodiscard]] QString name() const override { return name_; }
    [[nodiscard]] QString version() const override { return "1.0"; }
    bool initialize(LightCpp::Plugin::IdeServices*) override { return false; }
    void shutdown() override { ++shutdownCalls; }

    int shutdownCalls = 0;

private:
    QString name_;
};

}  // namespace

TEST(PluginManager, ARefusedPluginLosesTheCommandsItRegistered)
{
    RecordingServices services;
    LightCpp::Plugin::PluginManager manager;

    // The name deliberately differs from the source's stem: the Lua host
    // already cleans up under the *file* name from inside its own
    // initialize(), so this second call is the only thing covering a
    // plugin.name() that does not match the script file.
    auto plugin = std::make_unique<RefusingPlugin>(QStringLiteral("broken-script"));
    manager.registerPlugin(std::move(plugin), &services,
                           QStringLiteral("C:/plugins/lua/some-other-stem.lua"));

    EXPECT_TRUE(manager.loaded().isEmpty());
    ASSERT_EQ(services.removals.size(), 1);
    EXPECT_EQ(services.removals.first(), QStringLiteral("broken-script"));
}

TEST(PluginManager, APluginThatInitializesKeepsItsCommands)
{
    // The counterpart: a successful initialize() must not have its commands
    // torn down under it.
    RecordingServices services;
    LightCpp::Plugin::PluginManager manager;

    int shutdownCalls = 0;
    auto plugin = std::make_unique<TrackedPlugin>(QStringLiteral("alpha"),
                                                  &shutdownCalls);
    manager.registerPlugin(std::move(plugin), &services,
                           QStringLiteral("C:/plugins/lua/a.lua"));

    ASSERT_EQ(manager.loaded().size(), 1);
    EXPECT_TRUE(services.removals.isEmpty());
}

TEST(PluginManager, AnUnloadableLibraryReportsWhyItFailed)
{
    // A text file wearing a shared-library suffix: the path check passes and the load
    // itself fails, which is the shape of a plugin with missing
    // dependencies. The manager must hand the user a reason, not a bare
    // "broken shared library: ".
    //
    // This is a *characterization* test, not a reverse-proof. The review
    // claimed the old code read errorString() before load(), where it "must
    // be empty", so the panel showed nothing after the colon. Measured on
    // this Qt (6.x, .workbuddy/_errprobe.cpp): a constructed QPluginLoader
    // whose path exists has already tried to read the file's metadata and
    // errorString() is set from that — for a too-small file it is
    // "Failed to extract plugin meta data from '…': '…' is too small",
    // *before* load() is ever called. So the old three-part expression did
    // report the real reason (its post-load branch is the one that wins),
    // and only the reading order and the redundant pre-load snapshot were
    // worth cleaning up. Pinning the message text here would assert Qt's
    // wording rather than our behaviour, so the check stays at "a reason is
    // always attached to the file name".
    const QTemporaryDir dir;
    const QString brokenName = QStringLiteral("broken") + testLibrarySuffix();
    QFile broken(dir.filePath(brokenName));
    ASSERT_TRUE(broken.open(QIODevice::WriteOnly));
    broken.write("this is not a plugin");
    broken.close();

    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(dir.path(), &services);

    ASSERT_EQ(errors.size(), 1);
    const QString prefix = brokenName + QStringLiteral(": ");
    EXPECT_TRUE(errors.first().startsWith(prefix))
        << errors.first().toStdString();
    // Something was said about the failure, not just the file name.
    EXPECT_GT(errors.first().size(), prefix.size())
        << errors.first().toStdString();
    EXPECT_TRUE(manager.loaded().isEmpty());
    EXPECT_TRUE(manager.backends().isEmpty());
}

// ---------------------------------------------------------------------------
// A failed reopen must not leave the previous project behind (R2-29)
// ---------------------------------------------------------------------------

#ifdef Q_OS_WIN
TEST(PluginManager, AFailedReopenDropsThePreviousProject)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty())
        << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = manager.backendFor(QStringLiteral("anything.dev"));
    ASSERT_NE(backend, nullptr);

    LightCpp::Plugin::BuildRequest request;
    request.configuration = QStringLiteral("Debug");
    request.compiler = QStringLiteral("E:/toolchain/g++.exe");
    request.make = QStringLiteral("E:/toolchain/mingw32-make.exe");

    // A good project opens and builds.
    const QTemporaryDir goodDir;
    const QString good = writeDevProject(goodDir, kDevFixture);
    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(good, &summary, &error))
        << error.toStdString();
    ASSERT_FALSE(backend->buildSteps(request).isEmpty());

    // A truncated file: valid text, but it lists no units, so the parse
    // fails. Asking to build afterwards must not regenerate the makefile of
    // the project that is no longer open — buildSteps() reads project_ and
    // would happily do exactly that if the failed open left it alive.
    const QTemporaryDir brokenDir;
    const QString brokenPath = brokenDir.filePath(QStringLiteral("broken.dev"));
    QFile broken(brokenPath);
    ASSERT_TRUE(broken.open(QIODevice::WriteOnly | QIODevice::Text));
    broken.write("[Project]\nFileName=broken.dev\nName=broken\n");
    broken.close();

    EXPECT_FALSE(backend->openProject(brokenPath, &summary, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(backend->buildSteps(request).isEmpty());
    EXPECT_TRUE(backend->configureSteps(request).isEmpty());

    // And reopening the good one still works: the backend is not wedged.
    ASSERT_TRUE(backend->openProject(good, &summary, &error))
        << error.toStdString();
    EXPECT_FALSE(backend->buildSteps(request).isEmpty());
}

#endif  // Q_OS_WIN
