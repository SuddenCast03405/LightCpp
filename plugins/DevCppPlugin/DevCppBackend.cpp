// DevCppBackend — the Dev-C++ (.dev) project backend, packaged as a
// LightC++ plugin.
//
// One plugin object carries two interfaces: LightCpp::Plugin::Plugin (so the
// manager lists it and gives it services) and LightCpp::Plugin::ProjectBackend
// (so the IDE asks it about project files). The full .dev model is parsed
// here and stays here — the IDE only ever receives a ProjectSummary and the
// build steps produced on request.

#include <QDir>
#include <QFileInfo>

#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/PluginTypes.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"
#include "LightCpp/Project/BuildConfiguration.hpp"
#include "LightCpp/Project/DevCppProject.hpp"

namespace LightCpp::Plugins::DevCpp {

class DevCppBackend final : public QObject,
                            public LightCpp::Plugin::Plugin,
                            public LightCpp::Plugin::ProjectBackend {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.lightcpp.Plugin/1.0" FILE "metadata.json")
    Q_INTERFACES(LightCpp::Plugin::Plugin LightCpp::Plugin::ProjectBackend)

public:
    // --- LightCpp::Plugin::Plugin -----------------------------------------
    [[nodiscard]] QString name() const override
    {
        return QStringLiteral("DevCppProject");
    }
    [[nodiscard]] QString version() const override
    {
        return QStringLiteral("1.0.0");
    }

    bool initialize(LightCpp::Plugin::IdeServices* services) override
    {
        services_ = services;
        if (services_ != nullptr) {
            services_->log(
                QStringLiteral("[DevCppProject] Dev-C++ (.dev) project "
                               "support registered."));
        }
        return true;
    }

    void shutdown() override { services_ = nullptr; }

    // --- LightCpp::Plugin::ProjectBackend ---------------------------------
    [[nodiscard]] bool canOpenProject(const QString& filePath) const override
    {
        // Name-based on purpose: cheap, no disk access, and the dialog also
        // uses it to build its filter.
        return QFileInfo(filePath).suffix().compare(QStringLiteral("dev"),
                                                    Qt::CaseInsensitive) == 0;
    }

    [[nodiscard]] QString projectFilter() const override
    {
        return QStringLiteral("Dev-C++ (*.dev)");
    }

    bool openProject(const QString& filePath, LightCpp::Plugin::ProjectSummary* summary,
                     QString* error) override
    {
        QString parseError;
        QStringList warnings;
        std::optional<LightCpp::Project::DevCppProject> project =
            LightCpp::Project::loadDevProject(filePath, &parseError, &warnings);
        if (!project.has_value()) {
            // A failed (re)open must not leave the previous project behind:
            // `open_` stayed true and `project_` kept the old units, so
            // buildSteps() would happily regenerate *that* project's makefile
            // while the user believes the file they just picked is open.
            project_ = LightCpp::Project::DevCppProject{};
            open_ = false;
            if (error != nullptr) {
                *error = parseError;
            }
            return false;
        }
        project_ = std::move(*project);
        open_ = true;
        // A load that silently lost units must not look like a clean one:
        // the file "opened" but the tree is smaller than what was written.
        for (const QString& warning : std::as_const(warnings)) {
            if (services_ != nullptr) {
                services_->log(QStringLiteral("[DevCppProject] %1: %2")
                                   .arg(QFileInfo(filePath).fileName(),
                                        warning));
            }
        }

        if (summary != nullptr) {
            summary->filePath = project_.filePath;
            summary->projectDir = project_.projectDir;
            summary->name = project_.name;
            summary->formatName = QStringLiteral("Dev-C++");
            summary->targetLabel = project_.targetLabel();
            summary->outputPath = project_.outputPath();
            summary->unitCount = project_.units.size();
            summary->buildUnitCount = project_.buildUnitPaths().size();
            summary->producesExecutable = project_.producesExecutable();
            summary->runArguments = project_.runArguments;
        }
        return true;
    }

    [[nodiscard]] QList<LightCpp::Plugin::BuildStep>
    buildSteps(const LightCpp::Plugin::BuildRequest& request) override
    {
        if (!open_) {
            return {};
        }
        using LightCpp::Project::BuildConfiguration;
        const BuildConfiguration config =
            LightCpp::Project::buildConfigurationFromName(request.configuration)
                .value_or(BuildConfiguration::Debug);

        if (project_.useCustomMakefile) {
            // The project builds through its own makefile; the IDE-resolved
            // make (the kit's when it names one) drives it.
            return {LightCpp::Plugin::BuildStep{
                request.make,
                LightCpp::Project::makeArguments(project_) + request.extraMakeArgs,
                project_.projectDir,
                tr("Running the project's own makefile: %1")
                    .arg(project_.customMakefile)}};
        }

        // The Dev-C++ way: regenerate the project's makefile, then let make
        // drive the per-unit compiles and the link. Everything make-related
        // (relative paths, the working directory) matches what the original
        // IDE produces, so a user can open the same project in both.
        LightCpp::Project::DevCppToolchain tools;
        tools.gxx = request.compiler;
        QString writeError;
        const LightCpp::Project::DevCppMakefilePlan plan =
            LightCpp::Project::devCppMakefilePlan(project_, config, tools);
        if (!LightCpp::Project::writeDevCppMakefile(plan, project_, &writeError)) {
            if (services_ != nullptr) {
                services_->log(QStringLiteral("[DevCppProject] %1")
                                   .arg(writeError));
            }
            // No makefile, no build: running make now would either use the
            // stale previous generation (invisible wrong rules) or fail
            // with a confusing "-f: No such file". Returning no steps makes
            // the IDE report the declined build, with the reason above in
            // the Plugins channel.
            return {};
        }
        if (services_ != nullptr) {
            services_->log(QStringLiteral(
                               "[DevCppProject] Generated makefile: %1")
                               .arg(plan.path));
        }

        LightCpp::Plugin::BuildStep step;
        step.program = request.make;
        step.arguments =
            QStringList{QStringLiteral("-f"), plan.path, QStringLiteral("all")}
            + request.extraMakeArgs;
        step.workingDirectory = project_.projectDir;
        step.label = tr("Generating makefile and building [%1]")
                         .arg(LightCpp::Project::toString(config));
        return {step};
    }

    [[nodiscard]] QList<LightCpp::Plugin::BuildStep>
    configureSteps(const LightCpp::Plugin::BuildRequest&) override
    {
        // A .dev project is fully described by its own file — nothing to
        // configure before building.
        return {};
    }

private:
    LightCpp::Plugin::IdeServices* services_ = nullptr;
    /// The most recently opened project, kept for buildSteps(). The IDE
    /// opens one project at a time and always opens before building.
    LightCpp::Project::DevCppProject project_;
    bool open_ = false;
};

}  // namespace LightCpp::Plugins::DevCpp

#include "DevCppBackend.moc"
