// Plugin — the base interface every LightC++ plugin implements, and the
// IdeServices the IDE hands back to it at initialize time.
//
// C++ plugins are loaded once at start-up with QPluginLoader and stay loaded
// until the IDE exits — there is deliberately no hot reload. A plugin's
// initialize() receives the services object; everything it can do to the IDE
// goes through it, so the surface a plugin can touch is exactly what
// IdeServices declares.

#pragma once

#include <QObject>  // for Q_DECLARE_INTERFACE
#include <QString>
#include <QStringList>
#include <functional>

class QAction;

namespace LightCpp::Plugin {

class IdeServices;

class Plugin {
public:
    virtual ~Plugin() = default;

    /// Human-readable plugin name, shown in the Plugins output channel.
    [[nodiscard]] virtual QString name() const = 0;
    [[nodiscard]] virtual QString version() const = 0;
    /// Publisher shown as a tag in the plugin management panel. C++ plugins
    /// get it from their metadata.json ("publisher"); Lua scripts from
    /// plugin.publisher. Empty means "no publisher declared".
    [[nodiscard]] virtual QString publisher() const { return {}; }

    /// Called once, right after loading, before the main window is shown.
    /// Return false to refuse loading (the reason is returned as an error
    /// string to the host).
    virtual bool initialize(IdeServices* services) = 0;

    /// Called once when the IDE shuts down, while the window and its widgets
    /// are still alive. After this the plugin must not call services again.
    virtual void shutdown() = 0;
};

/// The IDE's face towards plugins. Implemented by MainWindow; every method
/// here is a whitelisted capability — plugins can do exactly this and
/// nothing else. The interface grows with the IDE; new capabilities are
/// added here, never by reaching into the window's internals.
class IdeServices {
public:
    virtual ~IdeServices() = default;

    // --- logging -----------------------------------------------------------
    /// Append a line to the Plugins output channel.
    virtual void log(const QString& message) = 0;

    // --- the active editor ---------------------------------------------------
    /// Path of the file in the active tab, or empty for an untitled buffer.
    [[nodiscard]] virtual QString activeEditorPath() const = 0;
    /// The active editor's whole text.
    [[nodiscard]] virtual QString editorText() const = 0;
    /// Replace the active editor's whole text (marks the buffer modified).
    virtual void setEditorText(const QString& text) = 0;
    /// Insert text at the caret without moving it.
    virtual void insertAtCursor(const QString& text) = 0;
    /// Jump the active editor's cursor to `line` (0-based).
    virtual void gotoLine(int line) = 0;

    // --- UI customization ----------------------------------------------------
    /// Every action's objectName (the ids the setters below accept).
    [[nodiscard]] virtual QStringList actionIds() const = 0;
    [[nodiscard]] virtual QString actionText(const QString& id) const = 0;
    /// Set an action's visible text (menus and toolbar update immediately).
    virtual void setActionText(const QString& id, const QString& text) = 0;
    /// Every top-level and nested menu's objectName.
    [[nodiscard]] virtual QStringList menuIds() const = 0;
    [[nodiscard]] virtual QString menuTitle(const QString& id) const = 0;
    virtual void setMenuTitle(const QString& id, const QString& title) = 0;

    // --- commands ------------------------------------------------------------
    /// Register a command: an entry under the Plugins menu that calls
    /// `callback` when triggered. An empty `shortcut` registers it without a
    /// keybinding. The id must be unique per plugin.
    virtual void registerCommand(const QString& pluginName, const QString& id,
                                 const QString& label, const QString& shortcut,
                                 std::function<void()> callback) = 0;
    /// Remove every command previously registered under `pluginName`.
    /// A plugin that fails to load or to initialize must call this: its
    /// callbacks point into memory that is about to be freed, and a menu
    /// entry that survives would crash the IDE when clicked. The default
    /// does nothing, so simple hosts and test stubs stay unaffected.
    virtual void removePluginCommands(const QString& pluginName)
    {
        (void)pluginName;
    }
};

}  // namespace LightCpp::Plugin

Q_DECLARE_INTERFACE(LightCpp::Plugin::Plugin, "org.lightcpp.Plugin/1.0")
