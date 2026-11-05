// MsvcBackend — Microsoft C/C++ project support (.vcxproj / .vcproj) as a
// LightC++ plugin.
//
// One plugin object carries two interfaces: LightCpp::Plugin::Plugin (so the
// manager lists it and gives it services) and LightCpp::Plugin::ProjectBackend
// (so the IDE asks it about project files). The parsed project model lives in
// LightCppProject (unit-tested there); this file is the backend shell plus the
// toolchain location — msbuild for the MSBuild format, vcbuild for the
// VS2005-2008 one — which stays here because the IDE core itself does not
// know MSVC build tools.

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

#include <optional>
#include <utility>

#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/PluginTypes.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"
#include "LightCpp/Project/DspProject.hpp"
#include "LightCpp/Project/MsvcProject.hpp"
#include "PluginSupport.hpp"

namespace LightCpp::Plugins::Msvc {

class MsvcBackend final : public QObject,
                          public LightCpp::Plugin::Plugin,
                          public LightCpp::Plugin::ProjectBackend {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.lightcpp.Plugin/1.0" FILE "metadata.json")
    Q_INTERFACES(LightCpp::Plugin::Plugin LightCpp::Plugin::ProjectBackend)

public:
    // --- LightCpp::Plugin::Plugin -----------------------------------------
    [[nodiscard]] QString name() const override
    {
        return QStringLiteral("MsvcProject");
    }
    [[nodiscard]] QString version() const override
    {
        return QStringLiteral("1.1.0");
    }

    bool initialize(LightCpp::Plugin::IdeServices* services) override
    {
        services_ = services;
        if (services_ != nullptr) {
            services_->log(
                QStringLiteral("[MsvcProject] Microsoft C/C++ project support "
                               "(.vcxproj/.vcproj/.dsp/.dsw) registered."));
        }
        return true;
    }

    void shutdown() override { services_ = nullptr; }

    // --- LightCpp::Plugin::ProjectBackend ---------------------------------
    [[nodiscard]] bool canOpenProject(const QString& filePath) const override
    {
        const QString suffix = QFileInfo(filePath).suffix().toLower();
        return suffix == QLatin1String("vcxproj")
               || suffix == QLatin1String("vcproj")
               || suffix == QLatin1String("sln")
               || suffix == QLatin1String("dsp")
               || suffix == QLatin1String("dsw");
    }

    [[nodiscard]] QString projectFilter() const override
    {
        return QStringLiteral(
            "MSVC project (*.vcxproj *.vcproj *.sln *.dsp *.dsw)");
    }

    bool openProject(const QString& filePath,
                     LightCpp::Plugin::ProjectSummary* summary,
                     QString* error) override
    {
        // Every path out of an open failure leaves a closed backend: the IDE
        // would otherwise keep building the previously opened project.
        resetOpenState();
        const QString suffix = QFileInfo(filePath).suffix().toLower();
        if (suffix == QLatin1String("sln")) {
            return openSolution(filePath, summary, error);
        }
        if (suffix == QLatin1String("dsp")
            || suffix == QLatin1String("dsw")) {
            return openDspOrWorkspace(filePath, summary, error);
        }
        return openVcProject(filePath, summary, error);
    }

    [[nodiscard]] QList<LightCpp::Plugin::BuildStep>
    buildSteps(const LightCpp::Plugin::BuildRequest& request) override
    {
        if (!open_) {
            return {};
        }
        if (hasDsp_) {
            return dspBuildSteps(request);
        }
        if (msvcOpen_) {
            return vcBuildSteps(request);
        }
        return {};
    }

    [[nodiscard]] QList<LightCpp::Plugin::BuildStep>
    configureSteps(const LightCpp::Plugin::BuildRequest&) override
    {
        // MSVC projects are self-describing; msbuild imports the property
        // sheets itself.
        return {};
    }

    [[nodiscard]] QString outputPath(const QString& configuration) const override
    {
        if (!open_) {
            return {};
        }
        if (hasDsp_) {
            const LightCpp::Project::DspConfiguration* config =
                dspProject_.configurationFor(configuration);
            if (config == nullptr) {
                config = dspProject_.defaultConfiguration();
            }
            return config == nullptr ? QString() : dspProject_.outputPathFor(*config);
        }
        if (msvcOpen_) {
            const LightCpp::Project::MsvcConfiguration* config =
                msvcProject_.configurationFor(configuration);
            if (config == nullptr) {
                config = msvcProject_.defaultConfiguration();
            }
            if (config == nullptr) {
                return {};
            }
            // Projects opened through a .sln expand $(SolutionDir) with the
            // solution root that msbuild is actually given.
            return msvcProject_.outputPathFor(*config,
                                              hasSolution_ ? solutionDir_ : QString());
        }
        return {};
    }

private:
    // --- opening -----------------------------------------------------------

    /// .sln: open the solution's C++ project (the one named like the
    /// solution, else the first), but keep the solution as the build
    /// descriptor so msbuild sees the same configuration mapping VS uses.
    bool openSolution(const QString& filePath,
                      LightCpp::Plugin::ProjectSummary* summary,
                      QString* error)
    {
        const QString absolute = QFileInfo(filePath).absoluteFilePath();
        QString readError;
        const QString text = Support::readTextFile(absolute, &readError);
        if (text.isEmpty()) {
            if (error != nullptr) {
                *error = readError.isEmpty() ? QStringLiteral("the solution is empty")
                                             : readError;
            }
            return false;
        }

        struct SolutionProject {
            QString name;
            QString path;
        };
        QList<SolutionProject> projects;
        static const QRegularExpression projectLine(
            QStringLiteral("^Project\\s*\\(\\s*\"[^\"]*\"\\s*\\)\\s*=\\s*\"([^\"]+)\"\\s*,\\s*\"([^\"]+)\""));
        const QString solutionDir = QFileInfo(absolute).absolutePath();
        for (const QString& line : Support::splitLines(text)) {
            const QRegularExpressionMatch match = projectLine.match(line.trimmed());
            if (!match.hasMatch()) {
                continue;
            }
            const QString relative = match.captured(2);
            const QString suffix = QFileInfo(relative).suffix().toLower();
            if (suffix != QLatin1String("vcxproj")
                && suffix != QLatin1String("vcproj")) {
                continue;
            }
            projects.append({match.captured(1), QDir(solutionDir).filePath(relative)});
        }
        if (projects.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("the solution lists no C++ project "
                                        "(.vcxproj/.vcproj)");
            }
            return false;
        }

        const QString stem = QFileInfo(absolute).completeBaseName();
        SolutionProject chosen = projects.first();
        for (const SolutionProject& project : projects) {
            if (QFileInfo(project.path).completeBaseName().compare(
                    stem, Qt::CaseInsensitive)
                == 0) {
                chosen = project;
                break;
            }
        }
        if (!QFileInfo::exists(chosen.path)) {
            if (error != nullptr) {
                *error = QStringLiteral("the solution references %1, which does "
                                        "not exist")
                             .arg(QDir::toNativeSeparators(chosen.path));
            }
            return false;
        }

        if (!openVcProject(chosen.path, summary, error)) {
            return false;
        }

        hasSolution_ = true;
        solutionPath_ = absolute;
        solutionDir_ = solutionDir;
        descriptorPath_ = absolute;

        if (summary != nullptr) {
            summary->filePath = absolute;      // reopening goes through the .sln
            summary->projectDir = solutionDir; // Explorer/git root
            summary->name = stem.isEmpty() ? chosen.name : stem;
            summary->formatName = QStringLiteral("Visual Studio (solution)");
            const LightCpp::Project::MsvcConfiguration* config =
                msvcProject_.defaultConfiguration();
            if (config != nullptr) {
                summary->outputPath = msvcProject_.outputPathFor(*config, solutionDir);
            }
        }
        if (services_ != nullptr) {
            services_->log(QStringLiteral(
                               "[MsvcProject] Solution %1: building %2 (%3 C++ "
                               "project(s) listed).")
                               .arg(stem, QDir::toNativeSeparators(chosen.path))
                               .arg(projects.size()));
        }
        return true;
    }

    /// .dsp directly, or .dsw by opening the project it lists (the one
    /// named like the workspace, else the first — a workspace usually sits
    /// next to the project it was created with).
    bool openDspOrWorkspace(const QString& filePath,
                            LightCpp::Plugin::ProjectSummary* summary,
                            QString* error)
    {
        QString dspPath = filePath;
        QString parseError;
        if (QFileInfo(filePath).suffix().compare(QLatin1String("dsw"),
                                                 Qt::CaseInsensitive)
            == 0) {
            auto workspace = LightCpp::Project::loadDswWorkspace(filePath, &parseError);
            if (!workspace.has_value()) {
                if (error != nullptr) {
                    *error = parseError;
                }
                return false;
            }
            workspace_ = std::move(*workspace);
            hasWorkspace_ = true;
            const LightCpp::Project::DswProjectEntry* entry =
                workspace_.defaultProject();
            if (entry == nullptr) {
                if (error != nullptr) {
                    *error = QStringLiteral("the workspace lists no projects");
                }
                return false;
            }
            dspPath = entry->dspPath;
        }

        auto project = LightCpp::Project::loadDspProject(dspPath, &parseError);
        if (!project.has_value()) {
            if (error != nullptr) {
                *error = parseError;
            }
            return false;
        }
        dspProject_ = std::move(*project);
        msvcOpen_ = false;
        hasDsp_ = true;
        open_ = true;
        hasSolution_ = false;
        descriptorPath_ = filePath;
        solutionDir_ = QFileInfo(filePath).suffix().compare(QLatin1String("dsw"),
                                                            Qt::CaseInsensitive)
                           == 0
                           ? QFileInfo(filePath).absolutePath()
                           : QFileInfo(dspPath).absolutePath();

        const LightCpp::Project::DspConfiguration* config =
            dspProject_.defaultConfiguration();
        if (summary != nullptr) {
            *summary = {};
            summary->filePath = filePath;
            summary->projectDir = solutionDir_;
            summary->name = dspProject_.name;
            summary->formatName = QStringLiteral("Visual Studio 6");
            summary->targetLabel =
                config != nullptr
                    ? LightCpp::Project::dspTargetLabel(config->kind)
                    : QStringLiteral("unknown");
            summary->outputPath =
                config != nullptr ? dspProject_.outputPathFor(*config) : QString();
            summary->unitCount = dspProject_.sourceFiles.size()
                                 + dspProject_.headerFiles.size()
                                 + dspProject_.resourceFiles.size()
                                 + dspProject_.otherFiles.size();
            summary->buildUnitCount = dspProject_.sourceFiles.size()
                                      + dspProject_.resourceFiles.size();
            summary->producesExecutable =
                config == nullptr
                || config->kind == LightCpp::Project::DspTargetKind::Application;
            summary->runProgram.clear();
            summary->runArguments.clear();
        }
        return true;
    }

    bool openVcProject(const QString& filePath,
                       LightCpp::Plugin::ProjectSummary* summary,
                       QString* error)
    {
        QString parseError;
        std::optional<LightCpp::Project::MsvcProject> project =
            LightCpp::Project::loadMsvcProject(filePath, &parseError);
        if (!project.has_value()) {
            if (error != nullptr) {
                *error = parseError;
            }
            return false;
        }
        if (!project->isValid()) {
            if (error != nullptr) {
                *error = QStringLiteral("no build configurations in %1")
                             .arg(QDir::toNativeSeparators(filePath));
            }
            return false;
        }
        msvcProject_ = std::move(*project);
        msvcOpen_ = true;
        hasDsp_ = false;
        hasSolution_ = false;
        open_ = true;
        descriptorPath_ = msvcProject_.filePath;
        solutionDir_ = msvcProject_.projectDir;

        // The configuration a plain "build this" uses: Debug, else Release,
        // else the first — the same default the model answers for output
        // paths, so the banner and the build agree.
        const LightCpp::Project::MsvcConfiguration* config =
            msvcProject_.defaultConfiguration();

        if (summary != nullptr) {
            *summary = {};
            summary->filePath = msvcProject_.filePath;
            summary->projectDir = msvcProject_.projectDir;
            summary->name = msvcProject_.name;
            summary->formatName = QStringLiteral("Visual Studio");
            summary->targetLabel =
                config != nullptr
                    ? LightCpp::Project::msvcTargetLabel(config->kind)
                    : QStringLiteral("unknown");
            summary->outputPath =
                config != nullptr ? msvcProject_.outputPathFor(*config) : QString();
            summary->unitCount = msvcProject_.sourceFiles.size()
                                 + msvcProject_.headerFiles.size()
                                 + msvcProject_.resourceFiles.size()
                                 + msvcProject_.otherFiles.size();
            summary->buildUnitCount = msvcProject_.sourceFiles.size()
                                      + msvcProject_.resourceFiles.size();
            summary->producesExecutable =
                config == nullptr
                || config->kind == LightCpp::Project::MsvcTargetKind::Application;
            summary->runProgram.clear();
            summary->runArguments.clear();
        }
        return true;
    }

    // --- build steps ---------------------------------------------------------

    /// Steps for whatever is open: the MSBuild-format pair or the VS6 pair.
    QList<LightCpp::Plugin::BuildStep>
    vcBuildSteps(const LightCpp::Plugin::BuildRequest& request)
    {
        // Map the canonical configuration name onto one of the project's own
        // configurations (Win32 preferred over x64). A project that spells
        // its configuration something else entirely ("MyDebug") still gets
        // built — with its own default, because msbuild only accepts names
        // the project actually defines.
        const LightCpp::Project::MsvcConfiguration* config =
            msvcProject_.configurationFor(request.configuration);
        if (config == nullptr) {
            config = msvcProject_.defaultConfiguration();
        }
        if (config == nullptr) {
            return {};
        }

        if (msvcProject_.isLegacyVcproj) {
            return legacyBuildSteps(*config);
        }
        return msbuildSteps(*config);
    }

    QList<LightCpp::Plugin::BuildStep>
    dspBuildSteps(const LightCpp::Plugin::BuildRequest& request)
    {
        const LightCpp::Project::DspConfiguration* config =
            dspProject_.configurationFor(request.configuration);
        if (config == nullptr) {
            config = dspProject_.defaultConfiguration();
        }
        if (config == nullptr) {
            return {};
        }

        const QString msdev = locateMsDev();
        if (msdev.isEmpty()) {
            if (services_ != nullptr) {
                services_->log(QStringLiteral(
                    "[MsvcProject] %1 is a Visual Studio 6 project. No "
                    "msdev found (Visual Studio 6 is not installed) — "
                    "install it to build .dsp/.dsw projects here.")
                        .arg(dspProject_.filePath));
            }
            return {};
        }

        LightCpp::Plugin::BuildStep step;
        step.program = msdev;
        step.arguments = QStringList{
            dspProject_.filePath,
            QStringLiteral("/make"),
            config->fullName,
        };
        step.workingDirectory = dspProject_.projectDir;
        step.label = tr("msdev [%1]").arg(config->fullName);
        return {step};
    }
    // --- .vcxproj: MSBuild -------------------------------------------------

    [[nodiscard]] QList<LightCpp::Plugin::BuildStep>
    msbuildSteps(const LightCpp::Project::MsvcConfiguration& config)
    {
        const QString msbuild = locateMsBuild();
        if (msbuild.isEmpty()) {
            if (services_ != nullptr) {
                services_->log(QStringLiteral(
                    "[MsvcProject] No MSBuild found (vswhere reported "
                    "nothing) — install Visual Studio or Build Tools with "
                    "the C++ workload."));
            }
            return {};
        }

        LightCpp::Plugin::BuildStep step;
        step.program = msbuild;
        step.arguments = QStringList{
            descriptorPath_.isEmpty() ? msvcProject_.filePath : descriptorPath_,
            QStringLiteral("/p:Configuration=%1").arg(config.name),
            QStringLiteral("/p:Platform=%1").arg(config.platform),
            // msbuild leaves SolutionDir undefined when no .sln is involved,
            // which breaks the OutDir=$(SolutionDir)$(Configuration)
            // templates. Pin it to the solution root (or the project
            // directory for a standalone project) so the output lands where
            // the banner predicted.
            QStringLiteral("/p:SolutionDir=%1\\")
                .arg(hasSolution_ ? solutionDir_ : msvcProject_.projectDir),
            QStringLiteral("/m"),
            QStringLiteral("/nologo"),
            QStringLiteral("/v:m"),
        };
        step.workingDirectory =
            hasSolution_ ? solutionDir_ : msvcProject_.projectDir;
        step.label = tr("MSBuild [%1|%2]").arg(config.name, config.platform);
        return {step};
    }

    // --- .vcproj: vcbuild --------------------------------------------------

    [[nodiscard]] QList<LightCpp::Plugin::BuildStep>
    legacyBuildSteps(const LightCpp::Project::MsvcConfiguration& config)
    {
        const QString vcbuild = locateVcBuild();
        if (vcbuild.isEmpty()) {
            if (services_ != nullptr) {
                services_->log(QStringLiteral(
                    "[MsvcProject] %1 is a VS2005-2008 project. No vcbuild.exe "
                    "found (Visual Studio 2005/2008 is not installed) and "
                    "modern MSBuild cannot build this format. Convert the "
                    "project with Visual Studio to build it here.")
                        .arg(msvcProject_.filePath));
            }
            return {};
        }

        LightCpp::Plugin::BuildStep step;
        step.program = vcbuild;
        step.arguments = QStringList{
            descriptorPath_.isEmpty() ? msvcProject_.filePath : descriptorPath_,
            QStringLiteral("%1|%2").arg(config.name, config.platform),
        };
        step.workingDirectory =
            hasSolution_ ? solutionDir_ : msvcProject_.projectDir;
        step.label = tr("vcbuild [%1|%2]").arg(config.name, config.platform);
        return {step};
    }

    // --- toolchain location -------------------------------------------------

    /// The newest MSBuild vswhere knows about. -prerelease matches what the
    /// IDE's own Msvc::detect does (preview installs do ship msbuild), and
    /// `-find` with a glob walks MSBuild\Current and the odd years where the
    /// directory layout differed, without any per-version guesswork here.
    ///
    /// Cached: the plugin interface is synchronous by contract (buildSteps()
    /// returns the list), so this lookup runs on the GUI thread, and vswhere
    /// costs its full 5s+15s timeouts on a machine where it is missing or
    /// wedged. Paying that once per session instead of on every build is the
    /// most this side can do without an asynchronous build-step interface.
    [[nodiscard]] QString locateMsBuild() const
    {
        if (msbuildProbed_) {
            return msbuildPath_;
        }
        msbuildProbed_ = true;
        msbuildPath_ = probeMsBuild();
        return msbuildPath_;
    }

    [[nodiscard]] static QStringList programFilesRoots()
    {
        QStringList roots;
        for (const char* variable : {"ProgramFiles(x86)", "ProgramFiles", "ProgramW6432"}) {
            const QString root = qEnvironmentVariable(variable);
            if (!root.isEmpty() && !roots.contains(root)) {
                roots.append(root);
            }
        }
        return roots;
    }

    [[nodiscard]] QString probeMsBuild() const
    {
        for (const QString& programFiles : programFilesRoots()) {
            const QString vswhere = QDir(programFiles).filePath(
                QStringLiteral("Microsoft Visual Studio/Installer/vswhere.exe"));
            if (!QFileInfo::exists(vswhere)) {
                continue;
            }

            QProcess process;
            process.start(vswhere,
                          QStringList{
                              QStringLiteral("-prerelease"),
                              QStringLiteral("-latest"),
                              QStringLiteral("-products"),
                              QStringLiteral("*"),
                              QStringLiteral("-requires"),
                              QStringLiteral("Microsoft.Component.MSBuild"),
                              QStringLiteral("-find"),
                              QStringLiteral("MSBuild\\**\\Bin\\MSBuild.exe"),
                          });
            if (!process.waitForStarted(5000) || !process.waitForFinished(15000)) {
                process.kill();
                continue;
            }
            const QStringList lines =
                QString::fromUtf8(process.readAllStandardOutput().trimmed())
                    .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (QString line : lines) {
                line = line.trimmed();
                if (!line.isEmpty() && QFileInfo::exists(line)) {
                    return line;
                }
            }
        }
        return {};
    }

    /// vcbuild.exe of a VS2005/2008 install, where MSVC projects of that era
    /// are buildable at all. Path probing rather than vswhere: those VS
    /// versions predate the installer database vswhere reads.
    [[nodiscard]] QString locateVcBuild() const
    {
        static const char* kCandidates[] = {
            "Microsoft Visual Studio 9.0/VC/vcpackages/vcbuild.exe",
            // VS2005's directory is "Microsoft Visual Studio 8", not 8.0.
            "Microsoft Visual Studio 8/VC/vcpackages/vcbuild.exe",
            "Microsoft Visual Studio 8.0/VC/vcpackages/vcbuild.exe",
        };
        for (const QString& programFiles : programFilesRoots()) {
            for (const char* candidate : kCandidates) {
                const QString path =
                    QDir(programFiles).filePath(QString::fromUtf8(candidate));
                if (QFileInfo::exists(path)) {
                    return path;
                }
            }
        }
        return {};
    }

    /// MSDEV.COM of a Visual Studio 6 install. The .COM host writes build
    /// output to the console (what the build log needs); the .EXE twin
    /// would just open the GUI instead of building.
    [[nodiscard]] QString locateMsDev() const
    {
        for (const QString& programFiles : programFilesRoots()) {
            const QString path = QDir(programFiles).filePath(QStringLiteral(
                "Microsoft Visual Studio/Common/MSDev98/Bin/MSDEV.COM"));
            if (QFileInfo::exists(path)) {
                return path;
            }
        }
        return {};
    }

    /// Drops every trace of a previously opened project. Any failure path in
    /// openProject leaves the backend closed, so buildSteps() cannot answer
    /// for a project the user already replaced.
    void resetOpenState()
    {
        msvcProject_ = {};
        dspProject_ = {};
        workspace_ = {};
        hasWorkspace_ = false;
        hasDsp_ = false;
        msvcOpen_ = false;
        open_ = false;
        hasSolution_ = false;
        solutionPath_.clear();
        solutionDir_.clear();
        descriptorPath_.clear();
    }

    LightCpp::Plugin::IdeServices* services_ = nullptr;
    /// Memoised MSBuild lookup (see locateMsBuild): the probe answers once
    /// per session, failures included, so a missing MSBuild is not paid for
    /// again on every build.
    mutable QString msbuildPath_;
    mutable bool msbuildProbed_ = false;
    /// The most recently opened project, kept for buildSteps(). The IDE
    /// opens one project at a time and always opens before building. The
    /// two shapes are mutually exclusive; hasDsp_ says which is live.
    LightCpp::Project::MsvcProject msvcProject_;
    LightCpp::Project::DspProject dspProject_;
    /// The .dsw the user actually opened, when there was one — its own
    /// directory is the workspace root the Explorer shows.
    LightCpp::Project::DswWorkspace workspace_;
    bool hasWorkspace_ = false;
    bool hasDsp_ = false;
    bool msvcOpen_ = false;
    bool open_ = false;
    /// A .sln opened as a wrapper around one of its C++ projects. The
    /// solution (not the project) is then what msbuild/vcbuild is pointed at,
    /// and $(SolutionDir) expands to the solution's directory.
    bool hasSolution_ = false;
    QString solutionPath_;
    QString solutionDir_;
    /// The file build steps name: the .sln when there is one, else the
    /// project itself.
    QString descriptorPath_;
};

}  // namespace LightCpp::Plugins::Msvc

#include "MsvcBackend.moc"
