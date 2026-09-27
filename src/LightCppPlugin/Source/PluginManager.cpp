#include "LightCpp/Plugin/PluginManager.hpp"

#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"

#include <QDir>
#include <QJsonObject>
#include <QPluginLoader>

namespace LightCpp::Plugin {

PluginManager::PluginManager(QObject* parent) : QObject(parent) {}

PluginManager::~PluginManager()
{
    shutdownPlugins();
    // The loaders are destroyed here, but the libraries stay in memory: a
    // plugin whose code still runs (e.g. from a queued signal) must not have
    // its library yanked out from under it. The process is on its way out
    // anyway — there is no hot reload to make room for.
}

QStringList PluginManager::loadSharedPlugins(
    const QString& directory, IdeServices* services,
    const QStringList& disabledRelativePaths)
{
    errors_.clear();
    services_ = services;

    const QDir dir(directory);
    const QStringList files =
        dir.entryList({QStringLiteral("*.dll")}, QDir::Files,
                      QDir::Name | QDir::IgnoreCase);
    if (files.isEmpty()) {
        return errors_;
    }

    for (const QString& file : files) {
        // The disable list holds paths relative to the plugins directory;
        // shared plugins sit directly in it, so the relative path is the
        // file name.
        if (disabledRelativePaths.contains(file, Qt::CaseInsensitive)) {
            continue;
        }
        const QString path = dir.filePath(file);
        auto loader = std::make_unique<QPluginLoader>(path);

        // Refuse plugins built against a different Qt or an incompatible
        // metadata version instead of loading them half-way.
        const QString loadError = loader->errorString();
        if (!loader->load()) {
            errors_.append(
                QStringLiteral("%1: %2").arg(file,
                                             loader->errorString().isEmpty()
                                                 ? loadError
                                                 : loader->errorString()));
            continue;
        }

        QObject* instance = loader->instance();
        auto* plugin = qobject_cast<Plugin*>(instance);
        if (plugin == nullptr) {
            errors_.append(QStringLiteral(
                               "%1: not a LightC++ plugin (missing "
                               "org.lightcpp.Plugin/1.0 interface)")
                               .arg(file));
            continue;
        }

        Loaded record;
        record.path = path;
        record.name = plugin->name();
        record.version = plugin->version();
        record.kind = Loaded::Kind::Cpp;
        // Publisher comes straight from the plugin's metadata.json.
        record.publisher =
            loader->metaData()
                .value(QStringLiteral("MetaData"))
                .toObject()
                .value(QStringLiteral("publisher"))
                .toString();

        if (auto* backend = qobject_cast<ProjectBackend*>(instance);
            backend != nullptr) {
            backends_.append(backend);
            record.hasBackend = true;
        }

        if (!plugin->initialize(services_)) {
            errors_.append(
                QStringLiteral("%1: initialize() failed").arg(file));
            if (record.hasBackend) {
                backends_.removeLast();
            }
            continue;
        }

        sharedEntries_.push_back(SharedEntry{std::move(loader), plugin});
        loaded_.append(record);
    }
    return errors_;
}

void PluginManager::registerPlugin(std::unique_ptr<Plugin> plugin,
                                   IdeServices* services,
                                   const QString& source)
{
    services_ = services;
    Loaded record;
    record.path = source;
    record.name = plugin->name();
    record.version = plugin->version();
    record.kind = Loaded::Kind::Lua;
    record.publisher = plugin->publisher();
    if (!plugin->initialize(services_)) {
        errors_.append(QStringLiteral("%1: initialize() failed").arg(source));
        return;  // unique_ptr drops the failed plugin here
    }
    // Note: dynamically registered plugins (the Lua host's script adapters)
    // never carry a ProjectBackend — project backends only come from shared
    // libraries, where qobject_cast across the plugin boundary works.
    registeredEntries_.push_back(RegisteredEntry{std::move(plugin), source});
    loaded_.append(record);
}

void PluginManager::shutdownPlugins()
{
    if (shutDown_) {
        return;
    }
    shutDown_ = true;
    // Reverse order: a later plugin may have built on an earlier one.
    for (auto it = registeredEntries_.rbegin();
         it != registeredEntries_.rend(); ++it) {
        it->plugin->shutdown();
    }
    for (auto it = sharedEntries_.rbegin(); it != sharedEntries_.rend(); ++it) {
        it->plugin->shutdown();
    }
    registeredEntries_.clear();
    backends_.clear();
    loaded_.clear();
}

bool PluginManager::unloadRegistered(const QString& source)
{
    if (shutDown_) {
        return false;
    }
    // Registered plugins (the Lua host's script adapters) can be torn down
    // safely: shutdown() runs plugin.onShutdown() and closes the state.
    // Shared-library plugins are not touchable this way — the disablement
    // of those needs a restart.
    for (auto it = registeredEntries_.rbegin();
         it != registeredEntries_.rend(); ++it) {
        if (it->source != source) {
            continue;
        }
        it->plugin->shutdown();
        for (auto lit = loaded_.begin(); lit != loaded_.end(); ++lit) {
            if (lit->kind == Loaded::Kind::Lua && lit->path == source) {
                loaded_.erase(lit);
                break;
            }
        }
        // rbegin() iterator -> forward erase position.
        registeredEntries_.erase(std::next(it).base());
        return true;
    }
    return false;
}

ProjectBackend* PluginManager::backendFor(const QString& filePath) const
{
    for (ProjectBackend* backend : backends_) {
        if (backend->canOpenProject(filePath)) {
            return backend;
        }
    }
    return nullptr;
}

QStringList PluginManager::projectFilters() const
{
    QStringList filters;
    for (ProjectBackend* backend : backends_) {
        const QString filter = backend->projectFilter();
        if (!filter.isEmpty()) {
            filters.append(filter);
        }
    }
    return filters;
}

}  // namespace LightCpp::Plugin
