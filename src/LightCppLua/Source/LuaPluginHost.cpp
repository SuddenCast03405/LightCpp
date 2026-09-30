#include "LightCpp/Lua/LuaPluginHost.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#include "LightCpp/Plugin/PluginManager.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QStringList>

#include <string>
#include <vector>

namespace LightCpp::Lua {

// ---------------------------------------------------------------------------
// IDE-side binding objects. One set per script. They hold no Lua state of
// their own; their addresses are registered as the `ide` members and must
// stay stable for the plugin's lifetime (they live inside LuaPlugin::Impl).
// ---------------------------------------------------------------------------
class LogApi {
public:
    LogApi() : services(nullptr) {}
    ::LightCpp::Plugin::IdeServices* services;
};

class EditorApi {
public:
    EditorApi() : services(nullptr) {}
    std::string activePath() const;
    std::string text() const;
    void setText(const std::string& value);
    void insert(const std::string& value);
    void gotoLine(int line);

    ::LightCpp::Plugin::IdeServices* services;
};

/// The menu / action texts a script rewrote, together with the values they
/// had before the script ran. Hot-unloading a plugin (a language pack is the
/// obvious case) has to put them back: otherwise the IDE keeps half its
/// menus translated until the next start.
struct UiTextUndo {
    struct Entry {
        QString id;
        QString text;
    };
    QList<Entry> menuTitles;
    QList<Entry> actionTexts;
    QStringList menuTitleSeen;
    QStringList actionTextSeen;

    /// Remember `id`'s value once — the first rewrite is the one to undo.
    void noteMenuTitle(const QString& id, const QString& original)
    {
        if (!menuTitleSeen.contains(id)) {
            menuTitleSeen.append(id);
            menuTitles.append(Entry{id, original});
        }
    }
    void noteActionText(const QString& id, const QString& original)
    {
        if (!actionTextSeen.contains(id)) {
            actionTextSeen.append(id);
            actionTexts.append(Entry{id, original});
        }
    }
};

class UiApi {
public:
    UiApi() : services(nullptr), undo(nullptr) {}
    std::string actionText(const std::string& id) const;
    void setActionText(const std::string& id, const std::string& text);
    std::string menuTitle(const std::string& id) const;
    void setMenuTitle(const std::string& id, const std::string& title);

    ::LightCpp::Plugin::IdeServices* services;
    /// Where the rewrites are recorded; owned by the plugin's Impl.
    UiTextUndo* undo;
};

class CommandApi {
public:
    CommandApi() : services(nullptr), sandbox(nullptr), active(true) {}
    /// Register one callback — a LUA_REGISTRYINDEX reference to a Lua
    /// function — as a command. Failures are logged when triggered.
    void registerInternal(const QString& shortcut, const QString& id,
                          const QString& label, int callbackRef);
    /// Run callback `index` inside the sandbox budgets; failures are logged.
    void trigger(size_t index);

    ::LightCpp::Plugin::IdeServices* services;
    LuaSandbox* sandbox;
    /// The script file stem — the owner name for command ids until (and
    /// even after) plugin.name is known.
    QString pluginName;
    /// The registered callbacks, as LUA_REGISTRYINDEX references that keep
    /// the Lua functions alive. Only touched from the GUI thread;
    /// registration happens inside a sandboxed script call.
    std::vector<int> callbacks;
    /// False after shutdown — script callbacks stop firing.
    bool active;
};

class SnippetApi {
public:
    SnippetApi() : services(nullptr) {}
    void registerSnippet(const QString& id, const QString& body);

    ::LightCpp::Plugin::IdeServices* services;
    QString pluginName;
};

// ---------------------------------------------------------------------------
// LuaPlugin::Impl — the script's state plus its binding objects. Defined
// first so the registration helpers below can use a complete type.
// ---------------------------------------------------------------------------
class LuaPlugin::Impl {
public:
    ::LightCpp::Plugin::IdeServices* services = nullptr;
    std::unique_ptr<LuaSandbox> sandbox;

    // Stable addresses: the `ide` instances point at these members.
    LogApi log;
    EditorApi editor;
    UiApi ui;
    CommandApi commands;
    SnippetApi snippets;
    /// What the script changed in the IDE's menus — undone on unload.
    UiTextUndo uiUndo;

    ExecutionResult callLifecycle(const char* functionName)
    {
        lua_State* L = sandbox != nullptr ? sandbox->state() : nullptr;
        if (L == nullptr) {
            return {true, {}, false, false};
        }
        lua_getglobal(L, "plugin");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return {true, {}, false, false};
        }
        lua_getfield(L, -1, functionName);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            return {true, {}, false, false};
        }
        const int functionIndex = lua_gettop(L);
        const ExecutionResult result =
            sandbox->callFunction(functionIndex, 0);
        lua_pop(L, 2);  // the function reference and the plugin table
        return result;
    }

    /// Put back the menu / action texts the script rewrote. Called on unload
    /// — before the state goes away, so a disabled language pack stops
    /// translating the IDE immediately instead of at the next start.
    void restoreUiTexts()
    {
        if (services == nullptr) {
            return;
        }
        // Reverse order: a rename of a rename lands back on the original.
        for (auto it = uiUndo.menuTitles.crbegin();
             it != uiUndo.menuTitles.crend(); ++it) {
            services->setMenuTitle(it->id, it->text);
        }
        for (auto it = uiUndo.actionTexts.crbegin();
             it != uiUndo.actionTexts.crend(); ++it) {
            services->setActionText(it->id, it->text);
        }
        uiUndo = UiTextUndo();
    }

    void releaseState()
    {
        commands.active = false;
        // Unregister the callbacks while the state still lives.
        if (sandbox != nullptr) {
            if (lua_State* L = sandbox->state()) {
                for (int ref : commands.callbacks) {
                    luaL_unref(L, LUA_REGISTRYINDEX, ref);
                }
            }
        }
        commands.callbacks.clear();
        sandbox.reset();  // then close the state
    }
};

// ---------------------------------------------------------------------------
// Registration helpers
// ---------------------------------------------------------------------------
namespace {

int luaLogClosure(lua_State* L)
{
    auto* api = static_cast<LogApi*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (api == nullptr || api->services == nullptr) {
        return 0;
    }
    const int n = lua_gettop(L);
    QString line;
    for (int i = 1; i <= n; ++i) {
        size_t length = 0;
        const char* text = luaL_tolstring(L, i, &length);
        if (i > 1) {
            line += QLatin1Char('\t');
        }
        line += QString::fromUtf8(text, static_cast<qsizetype>(length));
        lua_pop(L, 1);  // luaL_tolstring pushed the string
    }
    api->services->log(line);
    return 0;
}

/// Reads any value as a QString (strings, numbers and booleans included).
QString luaQStringValue(lua_State* L, int index)
{
    size_t length = 0;
    const char* text = luaL_tolstring(L, index, &length);
    QString value = QString::fromUtf8(text, static_cast<qsizetype>(length));
    lua_pop(L, 1);  // luaL_tolstring pushed the string
    return value;
}

/// The ide methods support dot invocation (`ide.ui.setMenuTitle(...)`) —
/// the documented plugin contract — and, for convenience, colon invocation
/// (`ide.ui:setMenuTitle(...)`), where the instance table lands in slot 1
/// and the arguments shift by one.
int firstArg(lua_State* L)
{
    return lua_type(L, 1) == LUA_TTABLE ? 2 : 1;
}

/// Adds one closure carrying the binding object as its upvalue.
void addMethod(lua_State* L, const char* name, lua_CFunction fn, void* api)
{
    lua_pushlightuserdata(L, api);
    lua_pushcclosure(L, fn, 1);
    lua_setfield(L, -2, name);
}

void pushUtf8(lua_State* L, const std::string& value)
{
    lua_pushlstring(L, value.data(), value.size());
}

// --- ide.editor ------------------------------------------------------------
int editorActivePath(lua_State* L)
{
    auto* api = static_cast<EditorApi*>(lua_touserdata(L, lua_upvalueindex(1)));
    pushUtf8(L, api != nullptr ? api->activePath() : std::string());
    return 1;
}

int editorText(lua_State* L)
{
    auto* api = static_cast<EditorApi*>(lua_touserdata(L, lua_upvalueindex(1)));
    pushUtf8(L, api != nullptr ? api->text() : std::string());
    return 1;
}

int editorSetText(lua_State* L)
{
    auto* api = static_cast<EditorApi*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (api != nullptr) {
        api->setText(luaQStringValue(L, firstArg(L)).toStdString());
    }
    return 0;
}

int editorInsert(lua_State* L)
{
    auto* api = static_cast<EditorApi*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (api != nullptr) {
        api->insert(luaQStringValue(L, firstArg(L)).toStdString());
    }
    return 0;
}

int editorGotoLine(lua_State* L)
{
    auto* api = static_cast<EditorApi*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (api != nullptr) {
        api->gotoLine(static_cast<int>(luaL_checkinteger(L, firstArg(L))));
    }
    return 0;
}

// --- ide.ui ----------------------------------------------------------------
int uiActionText(lua_State* L)
{
    auto* api = static_cast<UiApi*>(lua_touserdata(L, lua_upvalueindex(1)));
    pushUtf8(L, api != nullptr
                    ? api->actionText(luaQStringValue(L, firstArg(L))
                                          .toStdString())
                    : std::string());
    return 1;
}

int uiSetActionText(lua_State* L)
{
    auto* api = static_cast<UiApi*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (api != nullptr) {
        const int arg = firstArg(L);
        api->setActionText(luaQStringValue(L, arg).toStdString(),
                           luaQStringValue(L, arg + 1).toStdString());
    }
    return 0;
}

int uiMenuTitle(lua_State* L)
{
    auto* api = static_cast<UiApi*>(lua_touserdata(L, lua_upvalueindex(1)));
    pushUtf8(L, api != nullptr
                    ? api->menuTitle(luaQStringValue(L, firstArg(L))
                                         .toStdString())
                    : std::string());
    return 1;
}

int uiSetMenuTitle(lua_State* L)
{
    auto* api = static_cast<UiApi*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (api != nullptr) {
        const int arg = firstArg(L);
        api->setMenuTitle(luaQStringValue(L, arg).toStdString(),
                          luaQStringValue(L, arg + 1).toStdString());
    }
    return 0;
}

// --- ide.commands ----------------------------------------------------------
int commandsRegister(lua_State* L)
{
    auto* api =
        static_cast<CommandApi*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (api == nullptr || !api->active || api->services == nullptr) {
        return 0;
    }
    const int arg = firstArg(L);
    if (!lua_isfunction(L, arg + 2)) {
        return luaL_error(
            L, "ide.commands.register expects (id, label, function)");
    }
    lua_pushvalue(L, arg + 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    api->registerInternal(QString(), luaQStringValue(L, arg),
                          luaQStringValue(L, arg + 1), ref);
    return 0;
}

int commandsRegisterShortcut(lua_State* L)
{
    auto* api =
        static_cast<CommandApi*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (api == nullptr || !api->active || api->services == nullptr) {
        return 0;
    }
    const int arg = firstArg(L);
    if (!lua_isfunction(L, arg + 3)) {
        return luaL_error(L, "ide.commands.registerShortcut expects "
                             "(shortcut, id, label, function)");
    }
    lua_pushvalue(L, arg + 3);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    api->registerInternal(luaQStringValue(L, arg), luaQStringValue(L, arg + 1),
                          luaQStringValue(L, arg + 2), ref);
    return 0;
}

// --- ide.snippets ----------------------------------------------------------
int snippetsRegister(lua_State* L)
{
    auto* api = static_cast<SnippetApi*>(lua_touserdata(L, lua_upvalueindex(1)));
    if (api != nullptr) {
        const int arg = firstArg(L);
        api->registerSnippet(luaQStringValue(L, arg),
                             luaQStringValue(L, arg + 1));
    }
    return 0;
}

/// Builds the whitelisted `ide` table for one state — plain tables of C
/// closures whose upvalue is the binding object. LuaBridge class instances
/// were tried first, but two of its traits clash with the plugin contract:
/// its namespace __newindex rejects unknown keys by raising outside any
/// pcall (process abort), and its member functions only support colon
/// invocation, while the documented contract is dot syntax
/// (`ide.ui.setMenuTitle(...)`).
void registerIdeApi(lua_State* L, LuaPlugin::Impl* impl)
{
    lua_newtable(L);  // ide

    lua_newtable(L);
    addMethod(L, "activePath", &editorActivePath, &impl->editor);
    addMethod(L, "text", &editorText, &impl->editor);
    addMethod(L, "setText", &editorSetText, &impl->editor);
    addMethod(L, "insert", &editorInsert, &impl->editor);
    addMethod(L, "gotoLine", &editorGotoLine, &impl->editor);
    lua_setfield(L, -2, "editor");  // ide is a plain table: raw assignment

    lua_newtable(L);
    addMethod(L, "actionText", &uiActionText, &impl->ui);
    addMethod(L, "setActionText", &uiSetActionText, &impl->ui);
    addMethod(L, "menuTitle", &uiMenuTitle, &impl->ui);
    addMethod(L, "setMenuTitle", &uiSetMenuTitle, &impl->ui);
    lua_setfield(L, -2, "ui");

    lua_newtable(L);
    addMethod(L, "register", &commandsRegister, &impl->commands);
    addMethod(L, "registerShortcut", &commandsRegisterShortcut,
              &impl->commands);
    lua_setfield(L, -2, "commands");

    lua_newtable(L);
    addMethod(L, "register", &snippetsRegister, &impl->snippets);
    lua_setfield(L, -2, "snippets");

    // ide.log(...) as a plain closure carrying the logging object.
    lua_pushlightuserdata(L, &impl->log);
    lua_pushcclosure(L, &luaLogClosure, 1);
    lua_setfield(L, -2, "log");

    lua_setglobal(L, "ide");
}

/// Reads plugin.<field> from the executed script, with a fallback.
QString metaString(lua_State* L, const char* field, const QString& fallback)
{
    lua_getglobal(L, "plugin");
    QString value = fallback;
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, field);
        if (const char* text = lua_tostring(L, -1)) {
            value = QString::fromUtf8(text);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return value;
}

}  // namespace

// ---------------------------------------------------------------------------
// Binding object implementations
// ---------------------------------------------------------------------------
std::string EditorApi::activePath() const
{
    return services != nullptr ? services->activeEditorPath().toStdString()
                               : std::string();
}

std::string EditorApi::text() const
{
    return services != nullptr ? services->editorText().toStdString()
                               : std::string();
}

void EditorApi::setText(const std::string& value)
{
    if (services != nullptr) {
        services->setEditorText(QString::fromStdString(value));
    }
}

void EditorApi::insert(const std::string& value)
{
    if (services != nullptr) {
        services->insertAtCursor(QString::fromStdString(value));
    }
}

void EditorApi::gotoLine(int line)
{
    if (services != nullptr) {
        services->gotoLine(line);
    }
}

std::string UiApi::actionText(const std::string& id) const
{
    return services != nullptr
               ? services->actionText(QString::fromStdString(id)).toStdString()
               : std::string();
}

void UiApi::setActionText(const std::string& id, const std::string& text)
{
    if (services == nullptr) {
        return;
    }
    const QString key = QString::fromStdString(id);
    if (undo != nullptr) {
        undo->noteActionText(key, services->actionText(key));
    }
    services->setActionText(key, QString::fromStdString(text));
}

std::string UiApi::menuTitle(const std::string& id) const
{
    return services != nullptr
               ? services->menuTitle(QString::fromStdString(id)).toStdString()
               : std::string();
}

void UiApi::setMenuTitle(const std::string& id, const std::string& title)
{
    if (services == nullptr) {
        return;
    }
    const QString key = QString::fromStdString(id);
    if (undo != nullptr) {
        undo->noteMenuTitle(key, services->menuTitle(key));
    }
    services->setMenuTitle(key, QString::fromStdString(title));
}

void CommandApi::registerInternal(const QString& shortcut,
                                  const QString& id, const QString& label,
                                  int callbackRef)
{
    if (!active || services == nullptr) {
        return;
    }
    callbacks.push_back(callbackRef);
    const size_t index = callbacks.size() - 1;
    services->registerCommand(pluginName, id, label, shortcut,
                              [this, index] { trigger(index); });
}

void CommandApi::trigger(size_t index)
{
    if (!active || services == nullptr || sandbox == nullptr
        || index >= callbacks.size()) {
        return;
    }
    lua_State* L = sandbox->state();
    if (L == nullptr) {
        return;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, callbacks[index]);
    const int functionIndex = lua_gettop(L);
    const ExecutionResult result = sandbox->callFunction(functionIndex, 0);
    lua_pop(L, 1);  // the pushed reference
    if (!result.ok) {
        services->log(
            QStringLiteral("[Lua %1] %2").arg(pluginName, result.error));
    }
}

void SnippetApi::registerSnippet(const QString& id, const QString& body)
{
    if (services == nullptr) {
        return;
    }
    services->registerCommand(
        pluginName, QStringLiteral("snippet.%1").arg(id), id, QString(),
        [services = this->services, body] {
            services->insertAtCursor(body);
        });
}

// ---------------------------------------------------------------------------
// LuaPlugin
// ---------------------------------------------------------------------------
LuaPlugin::LuaPlugin(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

LuaPlugin::~LuaPlugin()
{
    if (impl_ != nullptr) {
        impl_->releaseState();
    }
}

QString LuaPlugin::name() const
{
    return name_;
}

QString LuaPlugin::version() const
{
    return version_;
}

QString LuaPlugin::publisher() const
{
    return publisher_;
}

bool LuaPlugin::initialize(::LightCpp::Plugin::IdeServices* /*services*/)
{
    // Services were wired at load time: the script needs `ide` as soon as
    // it runs, which is before the manager's initialize round-trip. Here we
    // only run the lifecycle hook.
    const ExecutionResult result = impl_->callLifecycle("onInit");
    if (!result.ok) {
        if (impl_->services != nullptr) {
            impl_->services->log(QStringLiteral("[Lua %1] onInit failed: %2")
                                     .arg(name_, result.error));
        }
        return false;
    }
    return true;
}

void LuaPlugin::shutdown()
{
    impl_->callLifecycle("onShutdown");
    // The script is about to disappear: give the IDE its texts back.
    impl_->restoreUiTexts();
    impl_->releaseState();
}

// ---------------------------------------------------------------------------
// LuaPluginHost
// ---------------------------------------------------------------------------
LuaPluginHost::LoadResult LuaPluginHost::loadPlugin(
    const QString& filePath, ::LightCpp::Plugin::IdeServices* services,
    const SandboxLimits& limits)
{
    LoadResult result;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("cannot open the script");
        return result;
    }
    const QString code = QString::fromUtf8(file.readAll());
    const QFileInfo info(filePath);
    const QString stem = info.completeBaseName();

    auto impl = std::make_unique<LuaPlugin::Impl>();
    impl->services = services;
    impl->sandbox = std::make_unique<LuaSandbox>(limits);
    if (!impl->sandbox->valid()) {
        result.error = QStringLiteral("out of memory creating the Lua state");
        return result;
    }
    impl->log.services = services;
    impl->editor.services = services;
    impl->ui.services = services;
    impl->ui.undo = &impl->uiUndo;
    impl->commands.services = services;
    impl->commands.sandbox = impl->sandbox.get();
    // Commands registered at load time carry the file name as their owner —
    // plugin.name is only known after the script has run.
    impl->commands.pluginName = stem;
    impl->snippets.services = services;
    impl->snippets.pluginName = stem;
    impl->sandbox->setPrintSink([services](const QString& line) {
        services->log(QStringLiteral("[lua print] %1").arg(line));
    });

    registerIdeApi(impl->sandbox->state(), impl.get());

    // The "@" prefix makes Lua report error locations inside the file.
    const ExecutionResult run = impl->sandbox->runString(
        code, QStringLiteral("@%1").arg(info.absoluteFilePath()));
    if (!run.ok) {
        result.error = QStringLiteral("script failed: %1").arg(run.error);
        return result;
    }

    lua_State* L = impl->sandbox->state();
    const QString name = metaString(L, "name", stem);
    const QString version = metaString(L, "version", QStringLiteral("0.0"));

    auto plugin = std::unique_ptr<LuaPlugin>(new LuaPlugin(std::move(impl)));
    plugin->name_ = name;
    plugin->version_ = version;
    plugin->publisher_ = metaString(L, "publisher", QString());
    result.plugin = std::move(plugin);
    return result;
}

QStringList LuaPluginHost::loadPluginsFromDirectory(
    const QString& directory, ::LightCpp::Plugin::IdeServices* services,
    ::LightCpp::Plugin::PluginManager* manager, const SandboxLimits& limits,
    const QStringList& disabledFileNames)
{
    QStringList errors;
    const QDir pluginDir(directory);
    const QStringList scripts =
        pluginDir.entryList({QStringLiteral("*.lua")}, QDir::Files,
                            QDir::Name | QDir::IgnoreCase);
    for (const QString& file : scripts) {
        // `disabledFileNames` are the bare file names of the disabled
        // scripts (the caller derives them from the plugins-directory-
        // relative disable list).
        if (disabledFileNames.contains(file, Qt::CaseInsensitive)) {
            continue;
        }
        const QString path = pluginDir.filePath(file);
        LoadResult loaded = loadPlugin(path, services, limits);
        if (!loaded.error.isEmpty()) {
            errors.append(QStringLiteral("%1: %2").arg(file, loaded.error));
            continue;
        }
        manager->registerPlugin(std::move(loaded.plugin), services, path);
    }
    return errors;
}

}  // namespace LightCpp::Lua
