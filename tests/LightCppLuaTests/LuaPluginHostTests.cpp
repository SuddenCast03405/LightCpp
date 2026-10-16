// LuaPluginHostTests — scripts become plugins: metadata, lifecycle hooks,
// the `ide` API and the sandbox budgets applying to callbacks.

#include "LightCpp/Lua/LuaPluginHost.hpp"

#include "LightCpp/Plugin/PluginManager.hpp"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <functional>
#include <utility>

namespace {

/// Records everything a plugin does to the IDE — no widgets involved.
class StubServices final : public LightCpp::Plugin::IdeServices {
public:
    QStringList logs;
    QList<QString> commandIds;
    std::vector<std::function<void()>> callbacks;

    QString editorPath = QStringLiteral("E:/tmp/main.cpp");
    QString editorBuffer = QStringLiteral("int main() {}\n");
    QString menuFileTitle = QStringLiteral("&File");
    QString actionNewText = QStringLiteral("&New");

    // --- IdeServices -------------------------------------------------------
    void log(const QString& message) override { logs.append(message); }
    [[nodiscard]] QString activeEditorPath() const override { return editorPath; }
    [[nodiscard]] QString editorText() const override { return editorBuffer; }
    void setEditorText(const QString& text) override { editorBuffer = text; }
    void insertAtCursor(const QString& text) override { editorBuffer += text; }
    void gotoLine(int) override {}
    [[nodiscard]] QStringList actionIds() const override
    {
        return {QStringLiteral("actionNew")};
    }
    [[nodiscard]] QString actionText(const QString& id) const override
    {
        return id == QStringLiteral("actionNew") ? actionNewText : QString();
    }
    void setActionText(const QString& id, const QString& text) override
    {
        if (id == QStringLiteral("actionNew")) {
            actionNewText = text;
        }
    }
    [[nodiscard]] QStringList menuIds() const override
    {
        return {QStringLiteral("menuFile")};
    }
    [[nodiscard]] QString menuTitle(const QString& id) const override
    {
        return id == QStringLiteral("menuFile") ? menuFileTitle : QString();
    }
    void setMenuTitle(const QString& id, const QString& title) override
    {
        if (id == QStringLiteral("menuFile")) {
            menuFileTitle = title;
        }
    }
    void registerCommand(const QString&, const QString& id, const QString&,
                         const QString&,
                         std::function<void()> callback) override
    {
        commandIds.append(id);
        callbacks.push_back(std::move(callback));
    }
};

/// Writes a script into a per-test directory and returns its full path.
QString writeScript(const QTemporaryDir& dir, const QString& fileName,
                    const QString& code)
{
    const QString path = dir.filePath(fileName);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return {};
    }
    file.write(code.toUtf8());
    return path;
}

LightCpp::Lua::SandboxLimits tightLimits()
{
    LightCpp::Lua::SandboxLimits limits;
    limits.instructionBudget = 1'000'000;
    limits.timeBudgetMs = 5'000;
    return limits;
}

}  // namespace

TEST(LuaPluginHost, ReadsMetadataAndRunsOnInit)
{
    StubServices services;
    const QTemporaryDir dir;

    const QString path = writeScript(dir, "meta.lua", R"lua(
plugin = { name = "TestPlugin", version = "9.9" }
function plugin.onInit()
    ide.log("hello from lua")
end
)lua");

    auto loaded = LightCpp::Lua::LuaPluginHost::loadPlugin(path, &services,
                                                           tightLimits());
    ASSERT_TRUE(loaded.error.isEmpty())
        << loaded.error.toStdString();
    ASSERT_NE(loaded.plugin, nullptr);
    EXPECT_EQ(loaded.plugin->name(), QStringLiteral("TestPlugin"));
    EXPECT_EQ(loaded.plugin->version(), QStringLiteral("9.9"));

    // onInit runs at initialize, and ide.log landed in the stub.
    EXPECT_TRUE(loaded.plugin->initialize(&services));
    EXPECT_TRUE(services.logs.contains(QStringLiteral("hello from lua")));
}

TEST(LuaPluginHost, ExposesIdeApiThroughClosures)
{
    StubServices services;
    const QTemporaryDir dir;

    const QString path = writeScript(dir, "api.lua", R"lua(
plugin = { name = "api" }
function plugin.onInit()
    ide.ui.setMenuTitle("menuFile", "文件")
    ide.ui.setActionText("actionNew", "新建")
    ide.editor.insert("/* inserted */")
    local who = ide.editor.activePath()
    ide.log("path=" .. who)
    ide.commands.register("greet", "Greet", function()
        ide.log("greeted")
    end)
end
)lua");

    auto loaded = LightCpp::Lua::LuaPluginHost::loadPlugin(path, &services,
                                                           tightLimits());
    ASSERT_TRUE(loaded.error.isEmpty()) << loaded.error.toStdString();
    ASSERT_TRUE(loaded.plugin->initialize(&services));

    EXPECT_EQ(services.menuFileTitle, QStringLiteral("文件"));
    EXPECT_EQ(services.actionNewText, QStringLiteral("新建"));
    EXPECT_TRUE(services.editorBuffer.contains(QStringLiteral("/* inserted */")));
    EXPECT_TRUE(services.editorBuffer.contains(QStringLiteral("int main()")));
    ASSERT_EQ(services.commandIds.size(), 1);
    EXPECT_EQ(services.commandIds.first(), QStringLiteral("greet"));

    // Triggering the registered command runs the Lua callback, sandboxed.
    services.callbacks.front()();
    EXPECT_TRUE(services.logs.contains(QStringLiteral("greeted")));
}

TEST(LuaPluginHost, SnippetsRegisterInsertingCommands)
{
    StubServices services;
    const QTemporaryDir dir;

    const QString path = writeScript(dir, "snippets.lua", R"lua(
plugin = { name = "snip" }
function plugin.onInit()
    ide.snippets.register("forloop", "for i = 1, 10 do\n\t\nend\n")
end
)lua");

    auto loaded = LightCpp::Lua::LuaPluginHost::loadPlugin(path, &services,
                                                           tightLimits());
    ASSERT_TRUE(loaded.error.isEmpty()) << loaded.error.toStdString();
    ASSERT_TRUE(loaded.plugin->initialize(&services));
    ASSERT_EQ(services.commandIds.size(), 1);
    EXPECT_EQ(services.commandIds.first(), QStringLiteral("snippet.forloop"));

    services.callbacks.front()();
    EXPECT_TRUE(services.editorBuffer.contains(
        QStringLiteral("for i = 1, 10 do")));
}

TEST(LuaPluginHost, PrintingWithoutServicesIsHarmless)
{
    const QTemporaryDir dir;

    // print runs at load *and* from the lifecycle hook; both go through the
    // same sink. R2-49: the sink dereferenced the services pointer the host
    // was called with, unlike every other ide.* entry point, so a script
    // loaded without services died on its first print.
    const QString path = writeScript(dir, "noisy.lua", R"lua(
plugin = { name = "noisy" }
print("at load")
function plugin.onInit()
    print("during onInit", 42)
end
)lua");

    auto loaded = LightCpp::Lua::LuaPluginHost::loadPlugin(path, nullptr,
                                                           tightLimits());
    ASSERT_TRUE(loaded.error.isEmpty()) << loaded.error.toStdString();
    ASSERT_NE(loaded.plugin, nullptr);
    EXPECT_EQ(loaded.plugin->name(), QStringLiteral("noisy"));
    EXPECT_TRUE(loaded.plugin->initialize(nullptr));
    // The sandbox's own sink still has to work with services present.
    StubServices services;
    const QString other = writeScript(dir, "noisy2.lua", R"lua(
plugin = { name = "noisy2" }
print("with services")
)lua");
    auto second = LightCpp::Lua::LuaPluginHost::loadPlugin(other, &services,
                                                           tightLimits());
    ASSERT_TRUE(second.error.isEmpty()) << second.error.toStdString();
    EXPECT_TRUE(services.logs.contains(QStringLiteral("[lua print] with services")));
}

TEST(LuaPluginHost, SnippetsStopFiringAfterShutdown)
{
    StubServices services;
    const QTemporaryDir dir;

    const QString path = writeScript(dir, "snip.lua", R"lua(
plugin = { name = "snip" }
function plugin.onInit()
    ide.snippets.register("after", "/* after */")
end
)lua");

    auto loaded = LightCpp::Lua::LuaPluginHost::loadPlugin(path, &services,
                                                           tightLimits());
    ASSERT_TRUE(loaded.error.isEmpty()) << loaded.error.toStdString();
    ASSERT_TRUE(loaded.plugin->initialize(&services));
    ASSERT_EQ(services.callbacks.size(), 1);
    services.callbacks.front()();
    EXPECT_TRUE(services.editorBuffer.contains(QStringLiteral("/* after */")));

    // R2-49: a snippet callback needs nothing from the Lua state, so it kept
    // writing through the IDE services after the plugin was gone — the
    // command entry may still be resident when the host unloads.
    const qsizetype before = services.editorBuffer.size();
    loaded.plugin->shutdown();
    services.callbacks.front()();
    EXPECT_EQ(services.editorBuffer.size(), before);
}

TEST(LuaPluginHost, ASecondRewriteIsRestoredToTheFirst)
{
    StubServices services;
    const QTemporaryDir dir;

    // Two language packs, the second loaded while the first is still active.
    const QString packA = writeScript(dir, "pack_a.lua", R"lua(
plugin = { name = "packA" }
function plugin.onInit()
    ide.ui.setMenuTitle("menuFile", "File A")
    ide.ui.setActionText("actionNew", "New A")
end
)lua");
    const QString packB = writeScript(dir, "pack_b.lua", R"lua(
plugin = { name = "packB" }
function plugin.onInit()
    ide.ui.setMenuTitle("menuFile", "File B")
    ide.ui.setActionText("actionNew", "New B")
end
)lua");

    auto a = LightCpp::Lua::LuaPluginHost::loadPlugin(packA, &services,
                                                      tightLimits());
    ASSERT_TRUE(a.error.isEmpty()) << a.error.toStdString();
    ASSERT_TRUE(a.plugin->initialize(&services));
    EXPECT_EQ(services.menuFileTitle, QStringLiteral("File A"));

    auto b = LightCpp::Lua::LuaPluginHost::loadPlugin(packB, &services,
                                                      tightLimits());
    ASSERT_TRUE(b.error.isEmpty()) << b.error.toStdString();
    ASSERT_TRUE(b.plugin->initialize(&services));
    EXPECT_EQ(services.menuFileTitle, QStringLiteral("File B"));
    EXPECT_EQ(services.actionNewText, QStringLiteral("New B"));

    // Switching B off has to fall back to A, which is still loaded.
    // R2-49: B's rewrite was not recorded (the undo table remembered each id
    // only once), so unloading B left its texts in place — half the IDE in a
    // language pack that is no longer loaded.
    b.plugin->shutdown();
    EXPECT_EQ(services.menuFileTitle, QStringLiteral("File A"));
    EXPECT_EQ(services.actionNewText, QStringLiteral("New A"));

    // And A's own unload unwinds back to the original texts.
    a.plugin->shutdown();
    EXPECT_EQ(services.menuFileTitle, QStringLiteral("&File"));
    EXPECT_EQ(services.actionNewText, QStringLiteral("&New"));
}

TEST(LuaPluginHost, CallbacksRunInsideTheSandboxBudgets)
{
    StubServices services;
    const QTemporaryDir dir;

    LightCpp::Lua::SandboxLimits limits;
    limits.instructionBudget = 2'000'000;
    limits.timeBudgetMs = 30'000;  // the step counter is the bound here

    const QString path = writeScript(dir, "runaway.lua", R"lua(
plugin = { name = "runaway" }
function plugin.onInit()
    ide.commands.register("spin", "Spin", function()
        local t = 0
        while true do t = t + 1 end
    end)
end
)lua");

    auto loaded = LightCpp::Lua::LuaPluginHost::loadPlugin(path, &services,
                                                           limits);
    ASSERT_TRUE(loaded.error.isEmpty()) << loaded.error.toStdString();
    ASSERT_TRUE(loaded.plugin->initialize(&services));

    // The infinite loop must come back under budget control, and the error
    // is reported instead of taking the process down.
    services.callbacks.front()();
    bool budgetError = false;
    for (const QString& line : services.logs) {
        if (line.contains(QStringLiteral("budget exceeded"))) {
            budgetError = true;
        }
    }
    EXPECT_TRUE(budgetError);
}

TEST(LuaPluginHost, BrokenScriptsAreRejectedWithReasons)
{
    StubServices services;
    const QTemporaryDir dir;

    const QString syntax = writeScript(dir, "broken.lua", "this is not ) lua");
    auto loaded = LightCpp::Lua::LuaPluginHost::loadPlugin(
        syntax, &services, tightLimits());
    EXPECT_TRUE(loaded.plugin == nullptr);
    EXPECT_TRUE(loaded.error.contains(QStringLiteral("script failed")));

    const QString runtime = writeScript(
        dir, "runtime.lua",
        "plugin = { name = 'x' }\nerror('boom at load time')\n");
    auto failed = LightCpp::Lua::LuaPluginHost::loadPlugin(
        runtime, &services, tightLimits());
    EXPECT_TRUE(failed.plugin == nullptr);
    EXPECT_TRUE(failed.error.contains(QStringLiteral("boom at load time")));
}

TEST(LuaPluginHost, DirectoryScanRegistersEveryScript)
{
    StubServices services;
    const QTemporaryDir dir;

    writeScript(dir, "aaa.lua", "plugin = { name = 'first' }\n");
    writeScript(dir, "bbb.lua", "plugin = { name = 'second' }\n");
    writeScript(dir, "ccc.lua", "not even close)\n");
    writeScript(dir, "notes.txt", "not a script at all\n");

    LightCpp::Plugin::PluginManager manager;
    const QStringList errors =
        LightCpp::Lua::LuaPluginHost::loadPluginsFromDirectory(
            dir.path(), &services, &manager, tightLimits());

    EXPECT_EQ(manager.loaded().size(), 2);  // aaa + bbb; ccc failed, txt skipped
    ASSERT_EQ(errors.size(), 1);
    EXPECT_TRUE(errors.first().startsWith(QStringLiteral("ccc.lua")));
    EXPECT_EQ(manager.loaded().first().name, QStringLiteral("first"));
    EXPECT_EQ(manager.loaded().at(1).name, QStringLiteral("second"));
}
