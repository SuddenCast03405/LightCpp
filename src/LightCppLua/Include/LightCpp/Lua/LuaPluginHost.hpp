// LuaPluginHost — runs Lua scripts as first-class plugins.
//
// Each *.lua under <exe dir>/plugins/lua gets its own sandboxed Lua 5.4
// state (see LuaSandbox) and is adapted to the same Plugin interface the
// C++ plugins implement, so the PluginManager treats scripts and DLLs
// identically. The IDE's whitelisted surface is exposed as the global `ide`
// namespace, registered with LuaBridge 3 from free functions on LuaBridge
// namespaces — that is what makes the dot form below work; the colon form
// (`ide.ui:setMenuTitle(...)`) is not supported and fails as an argument
// error. Arguments are converted by LuaBridge, so strings and numbers are
// both accepted where a string is expected, while a table or nil is a script
// error. A member that does not exist reads as nil (scripts feature-detect
// that way); writing to one raises a script error.
//
//   ide.log(message, ...)            write a line to the Plugins channel
//   ide.editor.activePath()          the active tab's file path ("" if untitled)
//   ide.editor.text()                the whole buffer text
//   ide.editor.setText(text)         replace the buffer (marks it modified)
//   ide.editor.insert(text)          insert at the caret
//   ide.editor.gotoLine(line)        jump (0-based line)
//   ide.ui.actionText(id) / ide.ui.setActionText(id, text)
//   ide.ui.menuTitle(id) / ide.ui.setMenuTitle(id, title)
//   ide.commands.register(id, label, function)        menu entry
//   ide.commands.registerShortcut(shortcut, id, label, function)
//   ide.snippets.register(id, body)  a command inserting `body` at the caret
//
// Script contract: the file defines a `plugin` table (name/version) and
// optional lifecycle hooks `plugin.onInit()` / `plugin.onShutdown()`; the
// global `ide` is present from load time. Everything the script does —
// including load-time execution and every callback — runs inside the
// sandbox budgets.

#pragma once

#include "LightCpp/Lua/LuaSandbox.hpp"
#include "LightCpp/Plugin/Plugin.hpp"

#include <QString>
#include <QStringList>

#include <memory>

namespace LightCpp::Plugin {
class PluginManager;
}

namespace LightCpp::Lua {

/// One Lua script loaded as a plugin. Construction executes the script
/// (with budgets); initialize() runs plugin.onInit(), shutdown() runs
/// plugin.onShutdown() and then closes the state.
class LuaPlugin final : public ::LightCpp::Plugin::Plugin {
public:
    /// Defined in the cpp: the sandbox plus the `ide` binding objects.
    class Impl;

    ~LuaPlugin() override;

    [[nodiscard]] QString name() const override;
    [[nodiscard]] QString version() const override;
    [[nodiscard]] QString publisher() const override;
    bool initialize(::LightCpp::Plugin::IdeServices* services) override;
    void shutdown() override;

private:
    friend class LuaPluginHost;
    explicit LuaPlugin(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
    QString name_;
    QString version_;
    QString publisher_;
};

class LuaPluginHost {
public:
    struct LoadResult {
        std::unique_ptr<LuaPlugin> plugin;
        QString error;
    };

    /// Load one script: create its sandbox, register the `ide` API and
    /// execute the file. The plugin object is constructed even when
    /// initialize() later fails, so the manager can drop it uniformly.
    static LoadResult loadPlugin(const QString& filePath,
                                 ::LightCpp::Plugin::IdeServices* services,
                                 const SandboxLimits& limits = {});

    /// Load every *.lua in `directory` (sorted by file name) and register
    /// the successful ones with `manager`. Scripts whose bare file name is
    /// listed in `disabledFileNames` are skipped. Returns one error string
    /// per failed script.
    static QStringList loadPluginsFromDirectory(
        const QString& directory, ::LightCpp::Plugin::IdeServices* services,
        ::LightCpp::Plugin::PluginManager* manager,
        const SandboxLimits& limits = {},
        const QStringList& disabledFileNames = {});
};

}  // namespace LightCpp::Lua
