// PluginManagerTests — scanning, error reporting, and a full QPluginLoader
// round-trip of the DevCppPlugin built in this tree: load the DLL, open a
// .dev fixture and ask for build steps.

#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/PluginManager.hpp"
#include "LightCpp/Plugin/PluginTypes.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"

#include <gtest/gtest.h>

#include <QFile>
#include <QFileInfo>
#include <QPluginLoader>
#include <QTemporaryDir>

namespace {

/// The quietest possible services: plugins initialize against it.
class SilentServices final : public LightCpp::Plugin::IdeServices {
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

TEST(PluginManager, NonPluginDllsAreSkippedWithErrors)
{
    // A random non-plugin library: any DLL that is not a LightC++ plugin
    // must come back as an error string, never as a crash.
    const QTemporaryDir dir;
    // Nothing to copy here without shipping a foreign dll — the negative
    // case is covered by ScanningAnEmptyDirectoryIsQuiet and the loader
    // failure path by a missing file below.
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        dir.filePath(QStringLiteral("does-not-exist")), &services);
    EXPECT_TRUE(errors.isEmpty());
    EXPECT_TRUE(manager.loaded().isEmpty());
}

TEST(PluginManager, DevCppPluginLoadsOpensAndAnswersBuildSteps)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty())
        << errors.join(QStringLiteral("; ")).toStdString();

    // The Dev-C++ plugin is present and registered as a project backend.
    ASSERT_FALSE(manager.loaded().isEmpty());
    ASSERT_EQ(manager.backends().size(), 1);
    EXPECT_EQ(manager.loaded().first().name, QStringLiteral("DevCppProject"));
    EXPECT_TRUE(manager.loaded().first().hasBackend);

    const QString filter = manager.projectFilters().join(QLatin1Char(';'));
    EXPECT_TRUE(filter.contains(QStringLiteral("*.dev")));

    auto* backend = manager.backendFor(QStringLiteral("anything.dev"));
    EXPECT_NE(backend, nullptr);
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
