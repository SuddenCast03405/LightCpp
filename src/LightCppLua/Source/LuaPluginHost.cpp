#include "LightCpp/Lua/LuaPluginHost.hpp"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// After the Lua headers: LuaBridge refuses to be included before them.
#include "LuaBridge/LuaBridge.h"

#include "LightCpp/Plugin/PluginManager.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QStringList>

#include <memory>
#include <string>
#include <vector>

namespace LightCpp::Lua {

// ---------------------------------------------------------------------------
// IDE-side binding objects. One set per script. They hold no Lua state of
// their own, and the `ide` functions registered with LuaBridge reach them
// through the plugin's Impl (the lambdas capture it), so they must stay put
// for the plugin's lifetime — they live inside LuaPlugin::Impl, which is
// heap-allocated and never moved.
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
///
/// EVERY rewrite is recorded, not just the first per id. Two language packs
/// loaded one after the other both rewrite the same menu; if the second one
/// kept no record, switching it off would leave its text in place instead of
/// falling back to the pack that is still loaded. Restoring in reverse order
/// unwinds a chain of renames back to the original.
struct UiTextUndo {
    struct Entry {
        QString id;
        QString text;
    };
    QList<Entry> menuTitles;
    QList<Entry> actionTexts;

    void noteMenuTitle(const QString& id, const QString& original)
    {
        menuTitles.append(Entry{id, original});
    }
    void noteActionText(const QString& id, const QString& original)
    {
        actionTexts.append(Entry{id, original});
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
    /// The strings arrive as UTF-8 from Lua and turn into QString here, the
    /// same boundary the editor and ui objects convert at.
    void registerInternal(const std::string& shortcut, const std::string& id,
                          const std::string& label, int callbackRef);
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
    void registerSnippet(const std::string& id, const std::string& body);

    ::LightCpp::Plugin::IdeServices* services;
    QString pluginName;
    /// False after shutdown. A snippet callback does not need the Lua state,
    /// so unlike a command it would keep working after the script is gone —
    /// writing through IDE services that are on their way out with the
    /// plugin. The callbacks share this flag (rather than a pointer back
    /// into the plugin) so they can tell that on their own: a raw `this` or
    /// a raw services pointer would both dangle, in opposite directions.
    std::shared_ptr<bool> active = std::make_shared<bool>(true);
    /// Switch the snippet callbacks off (they share the flag).
    void resetActive() { *active = false; }
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
        // Reverse order: the rewrites chain, so unwinding them from the last
        // one back lands on the original value.
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
        snippets.resetActive();
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
// The `ide` surface
//
// Registered with LuaBridge 3. Every member is a free function on a LuaBridge
// namespace, which is what makes the documented dot invocation work
// (`ide.ui.setMenuTitle(...)`). A LuaBridge *class* member would only be
// reachable through an instance userdata's __index, i.e. as colon invocation
// (`ide.ui:setMenuTitle(...)`); the old closures took both forms, and the
// colon form went with them — the contract never promised it.
//
// Argument conversion is LuaBridge's: strings and numbers arrive as
// std::string (LUABRIDGE_STRICT_STACK_CONVERSIONS is left at upstream's 0),
// while a table or nil where an id belongs is a Lua error. Reading a member
// that is not there still yields nil — the language pack probes features with
// `type(ui.setWidgetTexts) ~= "function"` — and writing one raises a
// catchable error instead of silently growing a field.
//
// Nothing here may throw a C++ exception: upstream's default configuration
// (LUABRIDGE_SAFE_LUA_C_EXCEPTION_HANDLING is 0) lets an exception unwind
// straight through a Lua C frame, so a rejected argument goes out through
// raise_lua_error, which is the same longjmp path as the luaL_error it
// replaced.
// ---------------------------------------------------------------------------
namespace {

/// `ide.log(...)` — variadic, and it stringifies whatever it is handed
/// (numbers, booleans, nil, even tables). That is the one member a typed
/// signature cannot describe, so it reads the stack itself; LuaBridge still
/// owns how the function is reached and how a failure is raised.
void logArguments(::LightCpp::Plugin::IdeServices* services, lua_State* L)
{
    if (services == nullptr) {
        return;
    }
    const int count = lua_gettop(L);  // before the loop: luaL_tolstring pushes
    QString line;
    for (int i = 1; i <= count; ++i) {
        size_t length = 0;
        const char* text = luaL_tolstring(L, i, &length);
        if (i > 1) {
            line += QLatin1Char('\t');
        }
        line += QString::fromUtf8(text, static_cast<qsizetype>(length));
        lua_pop(L, 1);  // the string luaL_tolstring pushed
    }
    services->log(line);
}

/// Keeps the callback alive as a LUA_REGISTRYINDEX reference and hands that
/// reference to the command registry. A non-function argument is a script
/// bug, so it is reported against the calling line rather than leaving the
/// plugin with a command that can never fire.
void registerCommandCallback(CommandApi& api, const char* expected,
                             const std::string& shortcut, const std::string& id,
                             const std::string& label,
                             const luabridge::LuaRef& callback)
{
    if (!api.active || api.services == nullptr) {
        // Unloading: no registry left to own the function, and nothing that
        // could still be triggered.
        return;
    }
    lua_State* L = callback.state();
    if (L == nullptr || !callback.isFunction()) {
        luabridge::raise_lua_error(L, "%s", expected);
    }
    callback.push();
    api.registerInternal(shortcut, id, label, luaL_ref(L, LUA_REGISTRYINDEX));
}

/// Builds the whitelisted `ide` table for one state. The lambdas capture the
/// plugin's Impl: it is heap-allocated and outlives the Lua state (the state
/// is closed in releaseState(), before Impl itself goes away), so the pointer
/// stays valid for as long as any closure can be called.
void registerIdeApi(lua_State* L, LuaPlugin::Impl* impl)
{
    luabridge::getGlobalNamespace(L)
        .beginNamespace("ide")
            .addFunction("log", [impl](lua_State* state) {
                logArguments(impl->log.services, state);
            })
            .beginNamespace("editor")
                .addFunction("activePath", [impl] {
                    return impl->editor.activePath();
                })
                .addFunction("text", [impl] { return impl->editor.text(); })
                .addFunction("setText", [impl](const std::string& value) {
                    impl->editor.setText(value);
                })
                .addFunction("insert", [impl](const std::string& value) {
                    impl->editor.insert(value);
                })
                .addFunction("gotoLine", [impl](int line) {
                    impl->editor.gotoLine(line);
                })
            .endNamespace()
            .beginNamespace("ui")
                .addFunction("actionText", [impl](const std::string& id) {
                    return impl->ui.actionText(id);
                })
                .addFunction("setActionText",
                             [impl](const std::string& id,
                                    const std::string& text) {
                                 impl->ui.setActionText(id, text);
                             })
                .addFunction("menuTitle", [impl](const std::string& id) {
                    return impl->ui.menuTitle(id);
                })
                .addFunction("setMenuTitle",
                             [impl](const std::string& id,
                                    const std::string& title) {
                                 impl->ui.setMenuTitle(id, title);
                             })
            .endNamespace()
            .beginNamespace("commands")
                .addFunction("register",
                             [impl](const std::string& id,
                                    const std::string& label,
                                    const luabridge::LuaRef& callback) {
                                 registerCommandCallback(
                                     impl->commands,
                                     "ide.commands.register expects "
                                     "(id, label, function)",
                                     std::string(), id, label, callback);
                             })
                .addFunction("registerShortcut",
                             [impl](const std::string& shortcut,
                                    const std::string& id,
                                    const std::string& label,
                                    const luabridge::LuaRef& callback) {
                                 registerCommandCallback(
                                     impl->commands,
                                     "ide.commands.registerShortcut expects "
                                     "(shortcut, id, label, function)",
                                     shortcut, id, label, callback);
                             })
            .endNamespace()
            .beginNamespace("snippets")
                .addFunction("register",
                             [impl](const std::string& id,
                                    const std::string& body) {
                                 impl->snippets.registerSnippet(id, body);
                             })
            .endNamespace()
        .endNamespace();
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

void CommandApi::registerInternal(const std::string& shortcut,
                                  const std::string& id, const std::string& label,
                                  int callbackRef)
{
    if (!active || services == nullptr) {
        return;
    }
    callbacks.push_back(callbackRef);
    const size_t index = callbacks.size() - 1;
    services->registerCommand(pluginName, QString::fromStdString(id),
                              QString::fromStdString(label),
                              QString::fromStdString(shortcut),
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

void SnippetApi::registerSnippet(const std::string& id, const std::string& body)
{
    if (!*active || services == nullptr) {
        return;
    }
    const QString key = QString::fromStdString(id);
    const QString text = QString::fromStdString(body);
    // The callback holds the lifetime token, not the plugin's storage: the
    // command registry may fire it long after the script was unloaded, and
    // it has to become a no-op rather than write through a dead plugin.
    services->registerCommand(
        pluginName, QStringLiteral("snippet.%1").arg(key), key, QString(),
        [services = this->services, alive = active, text] {
            if (!*alive || services == nullptr) {
                return;  // unloaded: nothing to insert into any more
            }
            services->insertAtCursor(text);
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
            // The script may have rewritten menu/action texts before
            // failing — put them back, or the IDE keeps showing a language
            // pack that is not actually loaded (and cannot be disabled,
            // since it never made it onto the plugin list).
            impl_->restoreUiTexts();
            // initialize() returning false drops this plugin (and its Lua
            // state with it); the commands the script registered capture
            // this Impl, so their menu entries must not outlive it.
            impl_->services->removePluginCommands(impl_->commands.pluginName);
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
    impl->sandbox->setPrintSink([impl = impl.get()](const QString& line) {
        // Loaded scripts run with or without services (a headless load is
        // legal), and every other ide.* entry point checks before it calls:
        // print has to as well, or the first print of such a script is a
        // null dereference.
        if (impl->services == nullptr) {
            return;
        }
        impl->services->log(QStringLiteral("[lua print] %1").arg(line));
    });

    registerIdeApi(impl->sandbox->state(), impl.get());

    // The "@" prefix makes Lua report error locations inside the file.
    const ExecutionResult run = impl->sandbox->runString(
        code, QStringLiteral("@%1").arg(info.absoluteFilePath()));
    if (!run.ok) {
        // The script may have rewritten menu/action texts before the error.
        if (services != nullptr) {
            impl->restoreUiTexts();
            // Commands registered mid-script capture `impl` internals that
            // die with this scope: the menu entries must go now or clicking
            // them would use freed memory.
            services->removePluginCommands(stem);
        }
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
