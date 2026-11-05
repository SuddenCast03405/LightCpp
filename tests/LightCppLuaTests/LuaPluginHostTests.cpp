// LuaPluginHostTests — scripts become plugins: metadata, lifecycle hooks,
// the `ide` API and the sandbox budgets applying to callbacks.

#include "LightCpp/Lua/LuaPluginHost.hpp"

#include "LightCpp/Plugin/PluginManager.hpp"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    /// Owner names passed to removePluginCommands(), in order. A script that
    /// fails mid-load must be rolled back under its file stem: its command
    /// callbacks capture internals that die with the load scope, so a menu
    /// entry left behind points at freed memory.
    QStringList removedCommands;
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
    void removePluginCommands(const QString& pluginName) override
    {
        removedCommands.append(pluginName);
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

TEST(LuaPluginHost, ExposesTheIdeApiWithDotInvocation)
{
    StubServices services;
    const QTemporaryDir dir;

    const QString path = writeScript(dir, "api.lua", R"lua(
plugin = { name = "api" }
function plugin.onInit()
    -- The shipped language pack's idiom: alias the namespace first, then call
    -- through the alias. Both hops are Lua table reads — the members are raw
    -- fields of the namespace table — so this has to keep working.
    local ui = ide.ui
    ui.setMenuTitle("menuFile", "文件")
    ui.setActionText("actionNew", "新建")
    ide.editor.insert("/* inserted */")
    local who = ide.editor.activePath()
    ide.log("path=" .. who)
    ide.log("title=" .. ui.menuTitle("menuFile"))
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
    EXPECT_TRUE(services.logs.contains(QStringLiteral("title=文件")));
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
    // The rollback: a script that dies mid-load may already have registered
    // menu entries (LuaPluginHost.cpp's `services->removePluginCommands(stem)`),
    // and those callbacks point into the load scope that is about to go away.
    // The stub used to be silent here, so this half of the fix had no
    // assertion at all.
    ASSERT_EQ(services.removedCommands.size(), 1);
    EXPECT_EQ(services.removedCommands.first(), QStringLiteral("broken"));

    const QString runtime = writeScript(
        dir, "runtime.lua",
        "plugin = { name = 'x' }\nerror('boom at load time')\n");
    auto failed = LightCpp::Lua::LuaPluginHost::loadPlugin(
        runtime, &services, tightLimits());
    EXPECT_TRUE(failed.plugin == nullptr);
    EXPECT_TRUE(failed.error.contains(QStringLiteral("boom at load time")));
    // Rolled back under the *file* stem, not under plugin.name — the name is
    // only known after the script has run, and this one never got that far.
    ASSERT_EQ(services.removedCommands.size(), 2);
    EXPECT_EQ(services.removedCommands.at(1), QStringLiteral("runtime"));
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

TEST(LuaPluginHost, AMemberThatIsNotThereReadsAsNil)
{
    // The language pack probes for capabilities it may not have:
    // src/LightCppPlugins/lua/zh-cn.lua does `if type(ui.setWidgetTexts) ~= "function"
    // then ... log ... end`. The `ide` namespaces are LuaBridge namespaces,
    // whose __index is a metamethod, so that probe depends on the metamethod
    // answering nil instead of raising — a raise would turn every language
    // pack's capability check into a load failure.
    //
    // The "present" half is what keeps this test honest: an empty `ide.ui`
    // would satisfy a missing-member check all by itself.
    StubServices services;
    const QTemporaryDir dir;

    const QString path = writeScript(dir, "probe.lua", R"lua(
plugin = { name = "probe" }
function plugin.onInit()
    local ui = ide.ui
    ide.log("absent=" .. tostring(ui.setWidgetTexts))
    ide.log("present=" .. type(ui.setMenuTitle))
    ide.log("top=" .. type(ide.noSuchMember))
    ide.log("log=" .. type(ide.log))
end
)lua");

    auto loaded =
        LightCpp::Lua::LuaPluginHost::loadPlugin(path, &services,
                                                 tightLimits());
    ASSERT_TRUE(loaded.error.isEmpty()) << loaded.error.toStdString();
    ASSERT_TRUE(loaded.plugin->initialize(&services));

    EXPECT_TRUE(services.logs.contains(QStringLiteral("absent=nil")))
        << services.logs.join(QStringLiteral(" | ")).toStdString();
    EXPECT_TRUE(services.logs.contains(QStringLiteral("present=function")));
    EXPECT_TRUE(services.logs.contains(QStringLiteral("top=nil")));
    EXPECT_TRUE(services.logs.contains(QStringLiteral("log=function")));
}

TEST(LuaPluginHost, AnArgumentOfTheWrongTypeFailsTheScriptNotTheProcess)
{
    // LuaBridge raises a bad argument with lua_error, i.e. a longjmp out of
    // the C function, so it only stays inside the sandbox because every
    // entry point runs under pcall. That is the property this test holds
    // down: a script that hands `ide` a table gets a script error with a
    // line number, and the host survives to load the next plugin.
    //
    // It is also the one visible behaviour change of using typed arguments:
    // the hand-written closures ran every value through luaL_tolstring, so
    // this call used to succeed and quietly retitle a menu whose id was the
    // string "table: 0x...".
    StubServices services;
    const QTemporaryDir dir;

    const QString tableArg = writeScript(dir, "table_arg.lua", R"lua(
plugin = { name = "tableArg" }
ide.ui.setMenuTitle({}, "x")
)lua");
    auto first = LightCpp::Lua::LuaPluginHost::loadPlugin(
        tableArg, &services, tightLimits());
    EXPECT_TRUE(first.plugin == nullptr);
    EXPECT_TRUE(first.error.contains(QStringLiteral("script failed")))
        << first.error.toStdString();
    // The IDE was not touched on the way out.
    EXPECT_EQ(services.menuFileTitle, QStringLiteral("&File"));

    const QString notAFunction = writeScript(dir, "not_fn.lua", R"lua(
plugin = { name = "notFn" }
ide.commands.register("id", "label", 42)
)lua");
    auto second = LightCpp::Lua::LuaPluginHost::loadPlugin(
        notAFunction, &services, tightLimits());
    EXPECT_TRUE(second.plugin == nullptr);
    EXPECT_TRUE(second.error.contains(
        QStringLiteral("expects (id, label, function)")))
        << second.error.toStdString();
    EXPECT_TRUE(services.commandIds.isEmpty());
}

TEST(LuaPluginHost, WritingToTheIdeNamespaceIsAScriptError)
{
    // A LuaBridge namespace answers __newindex with an error instead of
    // storing the value. That is a deliberate difference from the plain
    // tables this surface used to be: a typo such as
    // `ide.ui.setMenuTitel = ...` would otherwise create a field nobody
    // reads and fail silently. The error stays inside the sandbox's pcall,
    // so the script is rejected and the IDE keeps running.
    StubServices services;
    const QTemporaryDir dir;

    const QString path = writeScript(dir, "write.lua", R"lua(
plugin = { name = "write" }
ide.notARealMember = 1
)lua");

    auto loaded =
        LightCpp::Lua::LuaPluginHost::loadPlugin(path, &services,
                                                 tightLimits());
    EXPECT_TRUE(loaded.plugin == nullptr);
    EXPECT_TRUE(loaded.error.contains(QStringLiteral("script failed")))
        << loaded.error.toStdString();
}

// --- the language pack that ships with the IDE -----------------------------

TEST(LuaPluginHost, TheShippedLanguagePackLoadsAndRenamesTheUi)
{
    // Everything else in this file writes its own toy script, so nothing
    // proved that the file the build actually stages into dist/plugins/lua
    // loads at all — `zh-cn.lua` was only audited by reading it and by
    // running an equivalent script. It is the one plugin every user of the
    // Chinese build runs, and it is the only caller of the contract this
    // suite pins (the `local ui = ide.ui` alias, feature-detected
    // `type(ui.setWidgetTexts) ~= "function"`, menu/action ids by objectName).
    const QDir pluginDir(QStringLiteral(LIGHTCPP_LUA_PLUGIN_DIR));
    const QString path = pluginDir.filePath(QStringLiteral("zh-cn.lua"));
    ASSERT_TRUE(QFileInfo::exists(path))
        << "the language pack is missing from the source tree: "
        << path.toStdString();

    StubServices services;
    auto loaded =
        LightCpp::Lua::LuaPluginHost::loadPlugin(path, &services,
                                                 tightLimits());
    ASSERT_TRUE(loaded.error.isEmpty()) << loaded.error.toStdString();
    ASSERT_NE(loaded.plugin, nullptr);
    EXPECT_EQ(loaded.plugin->name(), QStringLiteral("zh-CN 简体中文"));
    EXPECT_EQ(loaded.plugin->version(), QStringLiteral("1.1.0"));

    ASSERT_TRUE(loaded.plugin->initialize(&services));

    // The two ids the stub knows about: an untranslated title here means
    // onInit ran but its writes went nowhere.
    EXPECT_EQ(services.menuFileTitle, QStringLiteral("文件(&F)"));
    EXPECT_EQ(services.actionNewText, QStringLiteral("新建(&N)"));
    EXPECT_TRUE(services.logs.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("简体中文语言包已加载")))
        << services.logs.join(QLatin1Char('\n')).toStdString();

    // The settings-dialog table must be skipped, not fatal: this build has no
    // `ide.ui.setWidgetTexts`, which is exactly what the pack feature-detects.
    EXPECT_TRUE(services.logs.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("setWidgetTexts")))
        << services.logs.join(QLatin1Char('\n')).toStdString();
}
