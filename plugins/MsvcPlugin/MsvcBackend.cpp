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

#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/PluginTypes.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"
#include "LightCpp/Project/DspProject.hpp"
#include "LightCpp/Project/MsvcProject.hpp"

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
        return QStringLiteral("1.0.0");
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
               || suffix == QLatin1String("dsp")
               || suffix == QLatin1String("dsw");
    }

    [[nodiscard]] QString projectFilter() const override
    {
        return QStringLiteral(
            "MSVC project (*.vcxproj *.vcproj *.dsp *.dsw)");
    }

    bool openProject(const QString& filePath,
                     LightCpp::Plugin::ProjectSummary* summary,
                     QString* error) override
    {
        const QString suffix = QFileInfo(filePath).suffix().toLower();
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

private:
    // --- opening -----------------------------------------------------------

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

        const LightCpp::Project::DspConfiguration* config =
            dspProject_.defaultConfiguration();
        if (summary != nullptr) {
            summary->filePath = filePath;
            summary->projectDir = QFileInfo(dspPath).absolutePath();
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
        msvcProject_ = std::move(*project);
        msvcOpen_ = true;
        hasDsp_ = false;
        open_ = true;

        // The configuration a plain "build this" uses: Debug, else Release,
        // else the first — the same default the model answers for output
        // paths, so the banner and the build agree.
        const LightCpp::Project::MsvcConfiguration* config =
            msvcProject_.defaultConfiguration();

        if (summary != nullptr) {
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
            msvcProject_.filePath,
            QStringLiteral("/p:Configuration=%1").arg(config.name),
            QStringLiteral("/p:Platform=%1").arg(config.platform),
            // Single-project semantics: msbuild leaves SolutionDir
            // undefined when no .sln is involved, which breaks the
            // OutDir=$(SolutionDir)$(Configuration) templates. Pinning it
            // to the project directory gives the same output location the
            // user would get from building the project inside an IDE.
            QStringLiteral("/p:SolutionDir=%1\\").arg(msvcProject_.projectDir),
            QStringLiteral("/m"),
            QStringLiteral("/nologo"),
            QStringLiteral("/v:m"),
        };
        step.workingDirectory = msvcProject_.projectDir;
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
            msvcProject_.filePath,
            QStringLiteral("%1|%2").arg(config.name, config.platform),
        };
        step.workingDirectory = msvcProject_.projectDir;
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

    [[nodiscard]] QString probeMsBuild() const
    {
        const QString programFilesX86 =
            qEnvironmentVariable("ProgramFiles(x86)");
        if (programFilesX86.isEmpty()) {
            return {};
        }
        const QString vswhere = QDir(programFilesX86).filePath(
            QStringLiteral("Microsoft Visual Studio/Installer/vswhere.exe"));
        if (!QFileInfo::exists(vswhere)) {
            return {};
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
            return {};
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
        return {};
    }

    /// vcbuild.exe of a VS2005/2008 install, where MSVC projects of that era
    /// are buildable at all. Path probing rather than vswhere: those VS
    /// versions predate the installer database vswhere reads.
    [[nodiscard]] QString locateVcBuild() const
    {
        const QString programFilesX86 =
            qEnvironmentVariable("ProgramFiles(x86)");
        if (programFilesX86.isEmpty()) {
            return {};
        }
        static const char* kCandidates[] = {
            "Microsoft Visual Studio 9.0/VC/vcpackages/vcbuild.exe",
            "Microsoft Visual Studio 8.0/VC/vcpackages/vcbuild.exe",
        };
        for (const char* candidate : kCandidates) {
            const QString path =
                QDir(programFilesX86).filePath(QString::fromUtf8(candidate));
            if (QFileInfo::exists(path)) {
                return path;
            }
        }
        return {};
    }

    /// MSDEV.COM of a Visual Studio 6 install. The .COM host writes build
    /// output to the console (what the build log needs); the .EXE twin
    /// would just open the GUI instead of building.
    [[nodiscard]] QString locateMsDev() const
    {
        const QString programFilesX86 =
            qEnvironmentVariable("ProgramFiles(x86)");
        if (programFilesX86.isEmpty()) {
            return {};
        }
        const QString path = QDir(programFilesX86).filePath(QStringLiteral(
            "Microsoft Visual Studio/Common/MSDev98/Bin/MSDEV.COM"));
        return QFileInfo::exists(path) ? path : QString();
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
};

}  // namespace LightCpp::Plugins::Msvc

#include "MsvcBackend.moc"
