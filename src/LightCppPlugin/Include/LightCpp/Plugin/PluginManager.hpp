// PluginManager — loads C++ plugins once at start-up with QPluginLoader and
// keeps them for the whole process lifetime. There is deliberately no hot
// reload: plugins are unloaded only at shutdown, which keeps ownership rules
// trivial (every plugin pointer stays valid between load and shutdown).

#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

class QPluginLoader;

namespace LightCpp::Plugin {

class IdeServices;
class Plugin;
class ProjectBackend;

class PluginManager : public QObject {
    Q_OBJECT
public:
    explicit PluginManager(QObject* parent = nullptr);
    ~PluginManager() override;

    /// One plugin that came up successfully.
    struct Loaded {
        /// Where the plugin's code came from — the driver behind the
        /// kind tag in the plugin management panel.
        enum class Kind { Cpp, Lua };

        QString path;      // the dll / registration source
        QString name;
        QString version;
        QString publisher; // empty when the plugin declares none
        Kind kind = Kind::Cpp;
        bool hasBackend = false;  // also implements ProjectBackend
    };

    /// Scan `directory` for *.dll files and load each one, skipping the
    /// file names listed in `disabledRelativePaths` (paths relative to the
    /// plugins directory — for shared plugins that is the bare file name).
    /// A dll that does not carry the LightCpp plugin metadata is skipped
    /// with an error string, never fatal. `services` is handed to every
    /// plugin's initialize(); it must outlive the manager.
    QStringList loadSharedPlugins(
        const QString& directory, IdeServices* services,
        const QStringList& disabledRelativePaths = {});

    /// Register an already-constructed plugin that was not loaded from a
    /// shared library (the Lua host's script adapters). The manager takes
    /// ownership and destroys it at shutdownPlugins().
    void registerPlugin(std::unique_ptr<Plugin> plugin, IdeServices* services,
                        const QString& source);

    /// Call shutdown() on every loaded plugin, in reverse load order. Safe
    /// to call twice.
    void shutdownPlugins();

    /// Hot-unload one registered (non-shared, i.e. Lua) plugin by its
    /// source path: shutdown(), destroy, drop from the registry. Returns
    /// false when no registered plugin matches — shared-library plugins
    /// cannot be hot-unloaded, their disablement needs a restart.
    bool unloadRegistered(const QString& source);

    // --- project backends ---------------------------------------------------
    /// The first backend that claims `filePath`, or nullptr.
    [[nodiscard]] ProjectBackend* backendFor(const QString& filePath) const;
    [[nodiscard]] const QList<ProjectBackend*>& backends() const
    {
        return backends_;
    }
    /// All backends' file-dialog filters, in load order.
    [[nodiscard]] QStringList projectFilters() const;

    [[nodiscard]] const QList<Loaded>& loaded() const { return loaded_; }
    [[nodiscard]] const QStringList& lastErrors() const { return errors_; }

private:
    QList<Loaded> loaded_;
    QList<ProjectBackend*> backends_;
    QStringList errors_;
    IdeServices* services_ = nullptr;
    bool shutDown_ = false;

    struct SharedEntry {
        std::unique_ptr<QPluginLoader> loader;
        Plugin* plugin = nullptr;
    };
    // std::vector, not QList: both containers hold move-only entries (the
    // unique_ptr loader / owned plugin) and QList's growth path
    // instantiates the copy constructor regardless of use.
    std::vector<SharedEntry> sharedEntries_;
    /// An owned, non-shared plugin (the Lua host's script adapters) plus
    /// the source it was registered from — the key hot-unload works by.
    struct RegisteredEntry {
        std::unique_ptr<Plugin> plugin;
        QString source;
    };
    std::vector<RegisteredEntry> registeredEntries_;
};

}  // namespace LightCpp::Plugin
