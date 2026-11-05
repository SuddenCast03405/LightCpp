// ProjectBackend — the extension point plugins use to teach the IDE a new
// project format.
//
// The IDE asks every loaded backend whether it can open a file; the first
// that claims it owns the open, and from then on answers for the build steps.
// Dev-C++ .dev support is the reference implementation and lives in its own
// plugin — the IDE core knows no concrete project format besides CMake.

#pragma once

#include "LightCpp/Plugin/PluginTypes.hpp"

#include <QList>
#include <QString>

namespace LightCpp::Plugin {

class ProjectBackend {
public:
    virtual ~ProjectBackend() = default;

    /// Cheap name-based check, no disk access: called for every candidate
    /// project file.
    [[nodiscard]] virtual bool canOpenProject(const QString& filePath) const = 0;

    /// The file-dialog filter this backend contributes to Open Project,
    /// e.g. "Dev-C++ (*.dev)".
    [[nodiscard]] virtual QString projectFilter() const = 0;

    /// Read and parse the descriptor. On success fills `summary` and keeps
    /// enough internal state to answer buildSteps() for this project — the
    /// backend holds the most recently opened project, which matches the
    /// IDE's one-open-project-at-a-time model.
    virtual bool openProject(const QString& filePath, ProjectSummary* summary,
                             QString* error) = 0;

    /// Steps to run for "Build Project". May include a configure step first.
    [[nodiscard]] virtual QList<BuildStep>
    buildSteps(const BuildRequest& request) = 0;

    /// Steps to run for "Configure Project" (empty for self-configured
    /// formats like .dev).
    [[nodiscard]] virtual QList<BuildStep>
    configureSteps(const BuildRequest& request) = 0;

    /// The file a build in `configuration` produces, or empty when the
    /// backend cannot name one. The summary's outputPath is a snapshot taken
    /// at open time; this answers for the configuration the user actually
    /// built, so Run Project never starts a stale binary from another profile.
    /// The default suits backends whose output does not depend on the
    /// configuration.
    [[nodiscard]] virtual QString outputPath(const QString& configuration) const
    {
        (void)configuration;
        return {};
    }
};

}  // namespace LightCpp::Plugin

Q_DECLARE_INTERFACE(LightCpp::Plugin::ProjectBackend,
                    "org.lightcpp.ProjectBackend/1.0")
