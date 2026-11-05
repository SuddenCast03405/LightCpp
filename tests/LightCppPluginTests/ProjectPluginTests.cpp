// ProjectPluginTests — the first-party language/framework project backends
// (Go, Rust, Python, Zig, Qt/qmake, wxWidgets/Code::Blocks) and the MSVC
// backend's solution support, exercised through the real plugin DLLs the
// build tree produces. Loading them through QPluginLoader (via PluginManager)
// is the contract that matters: the IDE only ever sees
// LightCpp::Plugin::ProjectBackend.
//
// The assertions stick to what does not depend on the host's installed
// toolchains: what opens the descriptor, the summary the IDE shows, the
// output path Run Project uses per configuration, and the shape of the build
// steps when the tool happens to be present. A machine without go/cargo/zig/
// qmake/codeblocks must still pass — buildSteps() answers an empty list
// there, which is the deliberate "declined, reason logged" contract.

#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/PluginManager.hpp"
#include "LightCpp/Plugin/PluginTypes.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

class SilentServices final : public LightCpp::Plugin::IdeServices {
public:
    void log(const QString&) override {}
    [[nodiscard]] QString activeEditorPath() const override { return {}; }
    [[nodiscard]] QString editorText() const override { return {}; }
    void setEditorText(const QString&) override {}
    void insertAtCursor(const QString&) override {}
    void gotoLine(int) override {}
    [[nodiscard]] QStringList actionIds() const override { return {}; }
    [[nodiscard]] QString actionText(const QString&) const override { return {}; }
    void setActionText(const QString&, const QString&) override {}
    [[nodiscard]] QStringList menuIds() const override { return {}; }
    [[nodiscard]] QString menuTitle(const QString&) const override { return {}; }
    void setMenuTitle(const QString&, const QString&) override {}
    void registerCommand(const QString&, const QString&, const QString&,
                         const QString&, std::function<void()>) override
    {
    }
};

QString hostExeSuffix()
{
#ifdef Q_OS_WIN
    return QStringLiteral(".exe");
#else
    return {};
#endif
}

bool writeFile(const QString& path, const QString& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(content.toUtf8());
    return file.flush();
}

/// The backend that claims `descriptor`, or nullptr when the corresponding
/// plugin was not built (the Windows-only ones on POSIX builds). Each test
/// turns that into a GTEST_SKIP with the reason.
LightCpp::Plugin::ProjectBackend* backendFor(
    LightCpp::Plugin::PluginManager& manager, const QString& descriptor)
{
    return manager.backendFor(descriptor);
}

LightCpp::Plugin::BuildRequest makeRequest(const QString& configuration)
{
    LightCpp::Plugin::BuildRequest request;
    request.configuration = configuration;
    return request;
}

bool toolOnPath(const QString& name)
{
    if (!QStandardPaths::findExecutable(name).isEmpty()) {
        return true;
    }
#ifdef Q_OS_WIN
    // The plugins retry a bare name with .exe (QStandardPaths does not always
    // apply PATHEXT itself); mirror that so "no tool" is diagnosed the same
    // way the backend does it.
    if (!name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)
        && !QStandardPaths::findExecutable(name + QStringLiteral(".exe"))
                .isEmpty()) {
        return true;
    }
#endif
    return false;
}

bool anyToolInstalled(std::initializer_list<QString> names)
{
    for (const QString& name : names) {
        if (toolOnPath(name)) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Go
// ---------------------------------------------------------------------------

TEST(ProjectPlugins, GoModuleOpensWithItsMainPackage)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("go.mod"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the Go plugin was not built for this platform";
    }

    EXPECT_TRUE(backend->canOpenProject(QStringLiteral("C:/p/go.mod")));
    EXPECT_FALSE(backend->canOpenProject(QStringLiteral("C:/p/main.go")));
    EXPECT_TRUE(backend->projectFilter().contains(QStringLiteral("go.mod")));

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("go.mod")),
                          QStringLiteral("module example.com/hello\n\ngo 1.21\n")));
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("main.go")),
                          QStringLiteral("package main\n\nfunc main() {}\n")));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("go.mod")),
                                     &summary, &error))
        << error.toStdString();

    EXPECT_EQ(summary.name, QStringLiteral("hello"));
    EXPECT_EQ(summary.formatName, QStringLiteral("Go"));
    EXPECT_EQ(summary.projectDir, dir.path());
    EXPECT_EQ(summary.unitCount, 1);
    EXPECT_EQ(summary.buildUnitCount, 1);
    EXPECT_TRUE(summary.producesExecutable);
    EXPECT_EQ(summary.outputPath,
              dir.filePath(QStringLiteral("hello") + hostExeSuffix()));
    EXPECT_TRUE(summary.runProgram.isEmpty());  // the binary itself runs

    // Go has no Debug/Release output layout: both configurations name the
    // same file.
    EXPECT_EQ(backend->outputPath(QStringLiteral("Debug")),
              summary.outputPath);
    EXPECT_EQ(backend->outputPath(QStringLiteral("Release")),
              summary.outputPath);

    const QList<LightCpp::Plugin::BuildStep> steps =
        backend->buildSteps(makeRequest(QStringLiteral("Debug")));
    if (!steps.isEmpty()) {
        ASSERT_EQ(steps.size(), 1);
        EXPECT_FALSE(steps.first().program.isEmpty());
        EXPECT_TRUE(steps.first().arguments.contains(QStringLiteral("build")));
        EXPECT_TRUE(steps.first().arguments.contains(QStringLiteral("-o")));
        EXPECT_TRUE(steps.first().arguments.contains(summary.outputPath));
        EXPECT_EQ(steps.first().workingDirectory, dir.path());
    } else {
        EXPECT_FALSE(anyToolInstalled({QStringLiteral("go")}));
    }
    EXPECT_TRUE(backend->configureSteps(makeRequest(QStringLiteral("Debug")))
                    .isEmpty());
}

TEST(ProjectPlugins, GoModuleWithoutMainIsALibrary)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("go.mod"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the Go plugin was not built for this platform";
    }

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("go.mod")),
                          QStringLiteral("module example.com/lib\n")));
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("lib.go")),
                          QStringLiteral("package lib\n\nfunc Answer() int { return 42 }\n")));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("go.mod")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_FALSE(summary.producesExecutable);
    EXPECT_TRUE(summary.outputPath.isEmpty());
    EXPECT_TRUE(backend->outputPath(QStringLiteral("Debug")).isEmpty());

    const QList<LightCpp::Plugin::BuildStep> steps =
        backend->buildSteps(makeRequest(QStringLiteral("Debug")));
    if (!steps.isEmpty()) {
        EXPECT_TRUE(steps.first().arguments.contains(QStringLiteral("./...")));
    }
}

// ---------------------------------------------------------------------------
// Rust
// ---------------------------------------------------------------------------

TEST(ProjectPlugins, CargoBinaryOpensWithProfileSpecificOutput)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("Cargo.toml"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the Rust plugin was not built for this platform";
    }

    EXPECT_TRUE(backend->canOpenProject(QStringLiteral("C:/p/Cargo.toml")));
    EXPECT_FALSE(backend->canOpenProject(QStringLiteral("C:/p/main.rs")));

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("Cargo.toml")),
                          QStringLiteral("[package]\nname = \"hello\"\n"
                                         "version = \"0.1.0\"\nedition = \"2021\"\n")));
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("src/main.rs")),
                          QStringLiteral("fn main() {}\n")));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("Cargo.toml")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_EQ(summary.name, QStringLiteral("hello"));
    EXPECT_TRUE(summary.formatName.contains(QStringLiteral("Rust")));
    EXPECT_TRUE(summary.producesExecutable);
    EXPECT_EQ(summary.outputPath,
              dir.filePath(QStringLiteral("target/debug/hello") + hostExeSuffix()));

    // The backend answers per configuration: the Release binary lives in
    // target/release, so Run Project picks it after a Release build instead
    // of the stale debug one.
    EXPECT_EQ(backend->outputPath(QStringLiteral("Release")),
              dir.filePath(QStringLiteral("target/release/hello")
                           + hostExeSuffix()));

    const QList<LightCpp::Plugin::BuildStep> steps =
        backend->buildSteps(makeRequest(QStringLiteral("Release")));
    if (!steps.isEmpty()) {
        ASSERT_EQ(steps.size(), 1);
        EXPECT_TRUE(steps.first().arguments.contains(QStringLiteral("build")));
        EXPECT_TRUE(steps.first().arguments.contains(QStringLiteral("--release")));
        EXPECT_EQ(steps.first().workingDirectory, dir.path());
    } else {
        EXPECT_FALSE(anyToolInstalled({QStringLiteral("cargo")}));
    }

    // A [[bin]] name overrides what the package would have produced.
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("Cargo.toml")),
                          QStringLiteral("[package]\nname = \"hello\"\n"
                                         "version = \"0.1.0\"\n\n"
                                         "[[bin]]\nname = \"tool\"\n"
                                         "path = \"src/main.rs\"\n")));
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("Cargo.toml")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_EQ(backend->outputPath(QStringLiteral("Debug")),
              dir.filePath(QStringLiteral("target/debug/tool") + hostExeSuffix()));
}

TEST(ProjectPlugins, CargoLibraryHasNothingToRun)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("Cargo.toml"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the Rust plugin was not built for this platform";
    }

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("Cargo.toml")),
                          QStringLiteral("[package]\nname = \"hello\"\n"
                                         "version = \"0.1.0\"\n\n[lib]\n"
                                         "path = \"src/lib.rs\"\n")));
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("src/lib.rs")),
                          QStringLiteral("pub fn answer() -> i32 { 42 }\n")));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("Cargo.toml")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_FALSE(summary.producesExecutable);
    EXPECT_TRUE(summary.outputPath.isEmpty());
    EXPECT_TRUE(backend->outputPath(QStringLiteral("Debug")).isEmpty());
}

// ---------------------------------------------------------------------------
// Python
// ---------------------------------------------------------------------------

TEST(ProjectPlugins, PythonProjectRunsItsEntryScript)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("pyproject.toml"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the Python plugin was not built for this platform";
    }

    EXPECT_TRUE(backend->canOpenProject(QStringLiteral("C:/p/pyproject.toml")));
    EXPECT_TRUE(backend->canOpenProject(QStringLiteral("C:/p/setup.py")));
    EXPECT_TRUE(backend->canOpenProject(QStringLiteral("C:/p/requirements.txt")));
    EXPECT_FALSE(backend->canOpenProject(QStringLiteral("C:/p/main.py")));

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("pyproject.toml")),
                          QStringLiteral("[project]\nname = \"hello\"\n"
                                         "version = \"0.1.0\"\n"
                                         "requires-python = \">=3.9\"\n")));
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("main.py")),
                          QStringLiteral("print('hello')\n")));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("pyproject.toml")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_EQ(summary.name, QStringLiteral("hello"));
    EXPECT_TRUE(summary.formatName.contains(QStringLiteral("Python")));
    EXPECT_EQ(summary.outputPath, dir.filePath(QStringLiteral("main.py")));

    // An interpreted project has no binary: Run Project starts the
    // interpreter with the entry script, and is unavailable when either the
    // interpreter or the script is missing. What the backend reports is the
    // truth here; the PATH probe only cross-checks it.
    const bool hasInterpreter = !summary.runProgram.isEmpty();
    EXPECT_EQ(summary.producesExecutable, hasInterpreter);
    EXPECT_EQ(summary.producesExecutable,
              anyToolInstalled({QStringLiteral("python"),
                                QStringLiteral("python3"),
                                QStringLiteral("py")}));
    if (hasInterpreter) {
        EXPECT_FALSE(summary.runProgram.isEmpty());
        ASSERT_EQ(summary.runArguments.size(), 1);
        EXPECT_EQ(summary.runArguments.first(),
                  dir.filePath(QStringLiteral("main.py")));
    } else {
        EXPECT_TRUE(summary.runProgram.isEmpty());
        EXPECT_TRUE(summary.runArguments.isEmpty());
    }
    EXPECT_EQ(backend->outputPath(QStringLiteral("Debug")),
              dir.filePath(QStringLiteral("main.py")));

    const QList<LightCpp::Plugin::BuildStep> steps =
        backend->buildSteps(makeRequest(QStringLiteral("Debug")));
    if (hasInterpreter) {
        ASSERT_EQ(steps.size(), 1);
        EXPECT_TRUE(steps.first().arguments.contains(
            QStringLiteral("compileall")));
        EXPECT_EQ(steps.first().workingDirectory, dir.path());
    } else {
        EXPECT_TRUE(steps.isEmpty());
    }
    EXPECT_TRUE(backend->configureSteps(makeRequest(QStringLiteral("Debug")))
                    .isEmpty());
}

TEST(ProjectPlugins, PythonProjectWithoutAnEntryPointCannotRun)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("pyproject.toml"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the Python plugin was not built for this platform";
    }

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("pyproject.toml")),
                          QStringLiteral("[project]\nname = \"hello\"\n")));
    // A module that is not one of the entry-point names the backend knows.
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("hello/module.py")),
                          QStringLiteral("VALUE = 1\n")));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("pyproject.toml")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_FALSE(summary.producesExecutable);
    EXPECT_TRUE(summary.outputPath.isEmpty());
    EXPECT_TRUE(summary.runProgram.isEmpty());
    EXPECT_TRUE(backend->outputPath(QStringLiteral("Debug")).isEmpty());
}

// ---------------------------------------------------------------------------
// Zig
// ---------------------------------------------------------------------------

TEST(ProjectPlugins, ZigExecutableOpensWithItsArtifactName)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("build.zig"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the Zig plugin was not built for this platform";
    }

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(
        dir.filePath(QStringLiteral("build.zig")),
        QStringLiteral("const std = @import(\"std\");\n\n"
                       "pub fn build(b: *std.Build) void {\n"
                       "    const target = b.standardTargetOptions(.{});\n"
                       "    const optimize = b.standardOptimizeOption(.{});\n"
                       "    const exe = b.addExecutable(.{\n"
                       "        .name = \"hello\",\n"
                       "        .root_source_file = b.path(\"src/main.zig\"),\n"
                       "        .target = target,\n"
                       "        .optimize = optimize,\n"
                       "    });\n"
                       "    b.installArtifact(exe);\n"
                       "}\n")));
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("src/main.zig")),
                          QStringLiteral("pub fn main() void {}\n")));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("build.zig")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_EQ(summary.name, QStringLiteral("hello"));
    EXPECT_TRUE(summary.formatName.contains(QStringLiteral("Zig")));
    EXPECT_TRUE(summary.producesExecutable);
    EXPECT_EQ(summary.outputPath,
              QDir(dir.path()).filePath(QStringLiteral("zig-out/bin/hello")
                                        + hostExeSuffix()));
    EXPECT_EQ(backend->outputPath(QStringLiteral("Release")),
              summary.outputPath);  // zig-out/bin is shared by optimize modes

    const QList<LightCpp::Plugin::BuildStep> steps =
        backend->buildSteps(makeRequest(QStringLiteral("Release")));
    if (!steps.isEmpty()) {
        ASSERT_EQ(steps.size(), 1);
        EXPECT_TRUE(steps.first().arguments.contains(QStringLiteral("build")));
        EXPECT_TRUE(steps.first().arguments.contains(
            QStringLiteral("-Doptimize=ReleaseFast")));
        EXPECT_EQ(steps.first().workingDirectory, dir.path());
    } else {
        EXPECT_FALSE(anyToolInstalled({QStringLiteral("zig")}));
    }
}

TEST(ProjectPlugins, ZigLibraryIsNotRunnable)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("build.zig"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the Zig plugin was not built for this platform";
    }

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(
        dir.filePath(QStringLiteral("build.zig")),
        QStringLiteral("pub fn build(b: *std.Build) void {\n"
                       "    const lib = b.addStaticLibrary(.{ .name = \"hello\" });\n"
                       "    b.installArtifact(lib);\n"
                       "}\n")));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("build.zig")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_EQ(summary.name, QStringLiteral("hello"));
    EXPECT_FALSE(summary.producesExecutable);
    EXPECT_TRUE(summary.outputPath.isEmpty());
    EXPECT_TRUE(backend->outputPath(QStringLiteral("Debug")).isEmpty());
}

// ---------------------------------------------------------------------------
// Qt for C++ (qmake .pro)
// ---------------------------------------------------------------------------

TEST(ProjectPlugins, QtProUsesPerConfigurationShadowBuilds)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("app.pro"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the Qt plugin was not built for this platform";
    }

    EXPECT_TRUE(backend->canOpenProject(QStringLiteral("C:/p/app.pro")));
    EXPECT_FALSE(backend->canOpenProject(QStringLiteral("C:/p/app.cpp")));

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("app.pro")),
                          QStringLiteral("QT += core gui\n"
                                         "TEMPLATE = app\n"
                                         "TARGET = hello\n"
                                         "SOURCES += main.cpp\n")));
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("main.cpp")),
                          QStringLiteral("int main() { return 0; }\n")));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("app.pro")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_EQ(summary.name, QStringLiteral("hello"));
    EXPECT_TRUE(summary.formatName.contains(QStringLiteral("Qt")));
    EXPECT_TRUE(summary.producesExecutable);

    // Windows qmake puts a single-config build under build/<cfg>/<cfg>/, other
    // platforms keep it directly in build/<cfg>/.
    const QString debugDir = QDir(dir.path()).filePath(QStringLiteral("build/debug"));
    const QString releaseDir =
        QDir(dir.path()).filePath(QStringLiteral("build/release"));
#ifdef Q_OS_WIN
    const QString debugOutput =
        QDir(debugDir).filePath(QStringLiteral("debug/hello") + hostExeSuffix());
    const QString releaseOutput =
        QDir(releaseDir).filePath(QStringLiteral("release/hello")
                                  + hostExeSuffix());
#else
    const QString debugOutput =
        QDir(debugDir).filePath(QStringLiteral("hello") + hostExeSuffix());
    const QString releaseOutput =
        QDir(releaseDir).filePath(QStringLiteral("hello") + hostExeSuffix());
#endif
    EXPECT_EQ(summary.outputPath, debugOutput);
    EXPECT_EQ(backend->outputPath(QStringLiteral("Debug")), debugOutput);
    EXPECT_EQ(backend->outputPath(QStringLiteral("Release")), releaseOutput);

    // Configure and build both regenerate the makefile in the shadow
    // directory; the exact number of steps depends on the host tools.
    const QList<LightCpp::Plugin::BuildStep> configure =
        backend->configureSteps(makeRequest(QStringLiteral("Debug")));
    if (!configure.isEmpty()) {
        ASSERT_EQ(configure.size(), 1);
        EXPECT_EQ(configure.first().workingDirectory, debugDir);
        EXPECT_TRUE(configure.first().arguments.contains(dir.filePath(
            QStringLiteral("app.pro"))));
        EXPECT_TRUE(configure.first().arguments.contains(
            QStringLiteral("CONFIG+=debug")));
        EXPECT_TRUE(configure.first().arguments.contains(
            QStringLiteral("CONFIG-=release")));
    } else {
        EXPECT_FALSE(anyToolInstalled({QStringLiteral("qmake6"),
                                       QStringLiteral("qmake-qt6"),
                                       QStringLiteral("qmake")}));
    }

    const QList<LightCpp::Plugin::BuildStep> steps =
        backend->buildSteps(makeRequest(QStringLiteral("Debug")));
    if (!steps.isEmpty()) {
        ASSERT_EQ(steps.size(), 2);
        EXPECT_EQ(steps.first().workingDirectory, debugDir);
        EXPECT_EQ(steps.last().workingDirectory, debugDir);
    } else {
        // Build needs both halves: qmake to write the Makefile and make to
        // drive it. Either one missing declines the build with a reason.
        const bool qmakeInstalled =
            anyToolInstalled({QStringLiteral("qmake6"),
                              QStringLiteral("qmake-qt6"),
                              QStringLiteral("qmake")});
        const bool makeInstalled =
            anyToolInstalled({QStringLiteral("make"),
                              QStringLiteral("gmake"),
                              QStringLiteral("mingw32-make")});
        EXPECT_TRUE(!qmakeInstalled || !makeInstalled);
    }
}

TEST(ProjectPlugins, QtProDestDirOverridesTheConfigurationDirectory)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("app.pro"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the Qt plugin was not built for this platform";
    }

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("app.pro")),
                          QStringLiteral("TEMPLATE = app\n"
                                         "TARGET = hello\n"
                                         "DESTDIR = $$OUT_PWD/bin\n"
                                         "SOURCES += main.cpp\n")));
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("main.cpp")),
                          QStringLiteral("int main() { return 0; }\n")));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("app.pro")),
                                     &summary, &error))
        << error.toStdString();
    const QString expected =
        QDir(dir.path()).filePath(QStringLiteral("build/debug/bin/hello")
                                  + hostExeSuffix());
    EXPECT_EQ(summary.outputPath, expected);
    EXPECT_EQ(backend->outputPath(QStringLiteral("Release")),
              QDir(dir.path()).filePath(QStringLiteral("build/release/bin/hello")
                                        + hostExeSuffix()));
}

TEST(ProjectPlugins, QtSubdirsProjectIsNotRunnable)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("app.pro"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the Qt plugin was not built for this platform";
    }

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("app.pro")),
                          QStringLiteral("TEMPLATE = subdirs\n"
                                         "SUBDIRS += lib app\n")));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("app.pro")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_FALSE(summary.producesExecutable);
    EXPECT_TRUE(summary.outputPath.isEmpty());
    EXPECT_TRUE(backend->outputPath(QStringLiteral("Debug")).isEmpty());
}

// ---------------------------------------------------------------------------
// wxWidgets (Code::Blocks .cbp)
// ---------------------------------------------------------------------------

namespace {

const char* kCbpFixture =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<CodeBlocks_project_file>\n"
    "  <FileVersion major=\"1\" minor=\"6\"/>\n"
    "  <Project>\n"
    "    <Option title=\"hello\"/>\n"
    "    <Build>\n"
    "      <Target title=\"Debug\">\n"
    "        <Option output=\"bin/Debug/hello\"/>\n"
    "        <Option object_output=\"obj/Debug/\"/>\n"
    "        <Option type=\"0\"/>\n"
    "        <Add directory=\"$(#wx.include)\"/>\n"
    "      </Target>\n"
    "      <Target title=\"Release\">\n"
    "        <Option output=\"bin/Release/hello\"/>\n"
    "        <Option object_output=\"obj/Release/\"/>\n"
    "        <Option type=\"0\"/>\n"
    "      </Target>\n"
    "    </Build>\n"
    "    <Unit filename=\"main.cpp\"><Option compile=\"1\"/></Unit>\n"
    "    <Unit filename=\"data.txt\"><Option compile=\"0\"/></Unit>\n"
    "  </Project>\n"
    "</CodeBlocks_project_file>\n";

}  // namespace

TEST(ProjectPlugins, CodeBlocksProjectOpensAndSelectsTheConfigurationTarget)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("hello.cbp"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the wxWidgets plugin was not built for this platform";
    }

    EXPECT_TRUE(backend->projectFilter().contains(QStringLiteral("*.cbp")));

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("hello.cbp")),
                          QString::fromUtf8(kCbpFixture)));
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("main.cpp")),
                          QStringLiteral("int main() { return 0; }\n")));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("hello.cbp")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_EQ(summary.name, QStringLiteral("hello"));
    // The file references wx through $(#wx, so the backend reports itself as
    // wxWidgets rather than a plain Code::Blocks project.
    EXPECT_EQ(summary.formatName, QStringLiteral("wxWidgets"));
    EXPECT_EQ(summary.unitCount, 2);       // both Unit elements
    EXPECT_EQ(summary.buildUnitCount, 1);  // compile="0" one is excluded
    EXPECT_TRUE(summary.producesExecutable);

    // Run uses the configuration the user built: Debug and Release name
    // different output files in the project itself.
    EXPECT_EQ(summary.outputPath,
              QDir(dir.path()).filePath(QStringLiteral("bin/Debug/hello")
                                        + hostExeSuffix()));
    EXPECT_EQ(backend->outputPath(QStringLiteral("Debug")), summary.outputPath);
    EXPECT_EQ(backend->outputPath(QStringLiteral("Release")),
              QDir(dir.path()).filePath(QStringLiteral("bin/Release/hello")
                                        + hostExeSuffix()));

    const QList<LightCpp::Plugin::BuildStep> steps =
        backend->buildSteps(makeRequest(QStringLiteral("Release")));
    if (!steps.isEmpty()) {
        ASSERT_EQ(steps.size(), 1);
        EXPECT_TRUE(steps.first().arguments.contains(
            QStringLiteral("--target=Release")));
        EXPECT_TRUE(steps.first().arguments.contains(
            dir.filePath(QStringLiteral("hello.cbp"))));
        EXPECT_EQ(steps.first().workingDirectory, dir.path());
    } else {
        EXPECT_FALSE(anyToolInstalled({QStringLiteral("codeblocks"),
                                       QStringLiteral("CodeBlocks")}));
    }
    EXPECT_TRUE(backend->configureSteps(makeRequest(QStringLiteral("Debug")))
                    .isEmpty());
}

TEST(ProjectPlugins, CodeBlocksLibraryTargetHasNothingToRun)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("hello.cbp"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the wxWidgets plugin was not built for this platform";
    }

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(
        dir.filePath(QStringLiteral("hello.cbp")),
        QStringLiteral("<?xml version=\"1.0\"?>\n"
                       "<CodeBlocks_project_file>\n"
                       "  <Project>\n"
                       "    <Option title=\"hello\"/>\n"
                       "    <Build>\n"
                       "      <Target title=\"Debug\">\n"
                       "        <Option output=\"lib/Debug/libhello\"/>\n"
                       "        <Option type=\"1\"/>\n"
                       "      </Target>\n"
                       "    </Build>\n"
                       "    <Unit filename=\"main.cpp\"/>\n"
                       "  </Project>\n"
                       "</CodeBlocks_project_file>\n")));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("hello.cbp")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_FALSE(summary.producesExecutable);
    EXPECT_TRUE(summary.outputPath.isEmpty());
    EXPECT_TRUE(backend->outputPath(QStringLiteral("Debug")).isEmpty());
}

// ---------------------------------------------------------------------------
// MSVC — solution support (Windows-only plugin)
// ---------------------------------------------------------------------------

#ifdef Q_OS_WIN

namespace {

const char* kVcxprojFixture =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Project DefaultTargets=\"Build\" "
    "xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
    "  <ItemGroup Label=\"ProjectConfigurations\">\n"
    "    <ProjectConfiguration Include=\"Debug|Win32\">\n"
    "      <Configuration>Debug</Configuration>\n"
    "      <Platform>Win32</Platform>\n"
    "    </ProjectConfiguration>\n"
    "    <ProjectConfiguration Include=\"Release|Win32\">\n"
    "      <Configuration>Release</Configuration>\n"
    "      <Platform>Win32</Platform>\n"
    "    </ProjectConfiguration>\n"
    "  </ItemGroup>\n"
    "  <PropertyGroup Label=\"Globals\">\n"
    "    <ProjectName>hello</ProjectName>\n"
    "  </PropertyGroup>\n"
    "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|Win32'\" "
    "Label=\"Configuration\">\n"
    "    <ConfigurationType>Application</ConfigurationType>\n"
    "  </PropertyGroup>\n"
    "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|Win32'\" "
    "Label=\"Configuration\">\n"
    "    <ConfigurationType>Application</ConfigurationType>\n"
    "  </PropertyGroup>\n"
    "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Debug|Win32'\">\n"
    "    <OutDir>$(SolutionDir)build\\$(Configuration)\\</OutDir>\n"
    "    <TargetName>hello</TargetName>\n"
    "  </PropertyGroup>\n"
    "  <PropertyGroup Condition=\"'$(Configuration)|$(Platform)'=='Release|Win32'\">\n"
    "    <OutDir>$(SolutionDir)build\\$(Configuration)\\</OutDir>\n"
    "    <TargetName>hello</TargetName>\n"
    "  </PropertyGroup>\n"
    "  <ItemGroup>\n"
    "    <ClCompile Include=\"main.cpp\"/>\n"
    "  </ItemGroup>\n"
    "</Project>\n";

const char* kBrokenVcxprojFixture =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<Project DefaultTargets=\"Build\" "
    "xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\">\n"
    "  <ItemGroup>\n"
    "    <ClCompile Include=\"main.cpp\"/>\n"
    "  </ItemGroup>\n"
    "</Project>\n";

const char* kSolutionFixture =
    "Microsoft Visual Studio Solution File, Format Version 12.00\n"
    "# Visual Studio Version 17\n"
    "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"hello\", "
    "\"proj\\hello.vcxproj\", "
    "\"{11111111-1111-1111-1111-111111111111}\"\n"
    "EndProject\n"
    "Global\n"
    "EndGlobal\n";

}  // namespace

TEST(ProjectPlugins, VcxprojOpensWithConfigurationSpecificOutput)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("hello.vcxproj"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the MSVC plugin was not built (Windows only)";
    }

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("hello.vcxproj")),
                          QString::fromUtf8(kVcxprojFixture)));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("hello.vcxproj")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_EQ(summary.name, QStringLiteral("hello"));
    EXPECT_TRUE(summary.producesExecutable);
    EXPECT_EQ(summary.unitCount, 1);
    // A standalone project expands $(SolutionDir) to its own directory.
    EXPECT_EQ(summary.outputPath,
              QDir(dir.path()).filePath(QStringLiteral("build/Debug/hello.exe")));

    // The summary is a snapshot; outputPath() answers for the built
    // configuration, which is what Run Project consults.
    EXPECT_EQ(backend->outputPath(QStringLiteral("Debug")), summary.outputPath);
    EXPECT_EQ(backend->outputPath(QStringLiteral("Release")),
              QDir(dir.path()).filePath(QStringLiteral("build/Release/hello.exe")));
}

TEST(ProjectPlugins, SolutionOpensAndBuildsTheSolutionDescriptor)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("hello.sln"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the MSVC plugin was not built (Windows only)";
    }

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("hello.sln")),
                          QString::fromUtf8(kSolutionFixture)));
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("proj/hello.vcxproj")),
                          QString::fromUtf8(kVcxprojFixture)));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("hello.sln")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_EQ(summary.filePath, dir.filePath(QStringLiteral("hello.sln")));
    EXPECT_EQ(summary.projectDir, dir.path());
    EXPECT_TRUE(summary.formatName.contains(QStringLiteral("solution")));
    EXPECT_TRUE(summary.producesExecutable);

    // $(SolutionDir) is the solution root, not the project's subdirectory.
    EXPECT_EQ(summary.outputPath,
              QDir(dir.path()).filePath(QStringLiteral("build/Debug/hello.exe")));
    EXPECT_EQ(backend->outputPath(QStringLiteral("Release")),
              QDir(dir.path()).filePath(QStringLiteral("build/Release/hello.exe")));
    // Reopening goes through the .sln, not the project inside it.
    EXPECT_TRUE(backend->canOpenProject(summary.filePath));

    // msbuild is pointed at the solution (it applies the solution's
    // configuration mapping), and runs from the solution root.
    const QList<LightCpp::Plugin::BuildStep> steps =
        backend->buildSteps(makeRequest(QStringLiteral("Debug")));
    if (!steps.isEmpty()) {
        ASSERT_EQ(steps.size(), 1);
        EXPECT_EQ(steps.first().arguments.first(),
                  dir.filePath(QStringLiteral("hello.sln")));
        EXPECT_EQ(steps.first().workingDirectory, dir.path());
        EXPECT_TRUE(steps.first().arguments.join(QLatin1Char(' '))
                        .contains(QStringLiteral("/p:Configuration=Debug")));
    }
}

TEST(ProjectPlugins, FailedReopenDropsThePreviousProject)
{
    SilentServices services;
    LightCpp::Plugin::PluginManager manager;
    const QStringList errors = manager.loadSharedPlugins(
        QStringLiteral(LIGHTCPP_BUILT_PLUGIN_DIR), &services);
    ASSERT_TRUE(errors.isEmpty()) << errors.join(QStringLiteral("; ")).toStdString();
    auto* backend = backendFor(manager, QStringLiteral("hello.vcxproj"));
    if (backend == nullptr) {
        GTEST_SKIP() << "the MSVC plugin was not built (Windows only)";
    }

    const QTemporaryDir dir;
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("hello.vcxproj")),
                          QString::fromUtf8(kVcxprojFixture)));

    LightCpp::Plugin::ProjectSummary summary;
    QString error;
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("hello.vcxproj")),
                                     &summary, &error))
        << error.toStdString();
    ASSERT_FALSE(backend->outputPath(QStringLiteral("Debug")).isEmpty());

    // A project with no build configuration is rejected. The failed open
    // must not leave the previous project answerable: outputPath() drives
    // Run Project and buildSteps() drives Build Project.
    const QString broken =
        dir.filePath(QStringLiteral("broken.vcxproj"));
    ASSERT_TRUE(writeFile(broken, QString::fromUtf8(kBrokenVcxprojFixture)));
    EXPECT_FALSE(backend->openProject(broken, &summary, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(backend->outputPath(QStringLiteral("Debug")).isEmpty());
    EXPECT_TRUE(backend->buildSteps(makeRequest(QStringLiteral("Debug")))
                    .isEmpty());

    // And it is not wedged: reopening the good project works again.
    ASSERT_TRUE(backend->openProject(dir.filePath(QStringLiteral("hello.vcxproj")),
                                     &summary, &error))
        << error.toStdString();
    EXPECT_FALSE(backend->outputPath(QStringLiteral("Debug")).isEmpty());
}

#endif  // Q_OS_WIN
