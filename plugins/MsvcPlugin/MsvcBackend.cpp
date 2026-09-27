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
                               "(.vcxproj/.vcproj) registered."));
        }
        return true;
    }

    void shutdown() override { services_ = nullptr; }

    // --- LightCpp::Plugin::ProjectBackend ---------------------------------
    [[nodiscard]] bool canOpenProject(const QString& filePath) const override
    {
        const QString suffix =
            QFileInfo(filePath).suffix().toLower();
        return suffix == QLatin1String("vcxproj")
               || suffix == QLatin1String("vcproj");
    }

    [[nodiscard]] QString projectFilter() const override
    {
        return QStringLiteral("MSVC project (*.vcxproj *.vcproj)");
    }

    bool openProject(const QString& filePath,
                     LightCpp::Plugin::ProjectSummary* summary,
                     QString* error) override
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
        project_ = std::move(*project);
        open_ = true;

        // The configuration a plain "build this" uses: Debug, else Release,
        // else the first — the same default the model answers for output
        // paths, so the banner and the build agree.
        const LightCpp::Project::MsvcConfiguration* config =
            project_.defaultConfiguration();

        if (summary != nullptr) {
            summary->filePath = project_.filePath;
            summary->projectDir = project_.projectDir;
            summary->name = project_.name;
            summary->targetLabel =
                config != nullptr
                    ? LightCpp::Project::msvcTargetLabel(config->kind)
                    : QStringLiteral("unknown");
            summary->outputPath =
                config != nullptr ? project_.outputPathFor(*config) : QString();
            summary->unitCount = project_.sourceFiles.size()
                                 + project_.headerFiles.size()
                                 + project_.resourceFiles.size()
                                 + project_.otherFiles.size();
            summary->buildUnitCount = project_.sourceFiles.size()
                                      + project_.resourceFiles.size();
            summary->producesExecutable =
                config == nullptr
                || config->kind == LightCpp::Project::MsvcTargetKind::Application;
            summary->runArguments.clear();
            summary->formatName = QStringLiteral("Visual Studio");
        }
        return true;
    }

    [[nodiscard]] QList<LightCpp::Plugin::BuildStep>
    buildSteps(const LightCpp::Plugin::BuildRequest& request) override
    {
        if (!open_) {
            return {};
        }

        // Map the canonical configuration name onto one of the project's own
        // configurations (Win32 preferred over x64). A project that spells
        // its configuration something else entirely ("MyDebug") still gets
        // built — with its own default, because msbuild only accepts names
        // the project actually defines.
        const LightCpp::Project::MsvcConfiguration* config =
            project_.configurationFor(request.configuration);
        if (config == nullptr) {
            config = project_.defaultConfiguration();
        }
        if (config == nullptr) {
            return {};
        }

        if (project_.isLegacyVcproj) {
            return legacyBuildSteps(*config);
        }
        return msbuildSteps(*config);
    }

    [[nodiscard]] QList<LightCpp::Plugin::BuildStep>
    configureSteps(const LightCpp::Plugin::BuildRequest&) override
    {
        // MSVC projects are self-describing; msbuild imports the property
        // sheets itself.
        return {};
    }

private:
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
            project_.filePath,
            QStringLiteral("/p:Configuration=%1").arg(config.name),
            QStringLiteral("/p:Platform=%1").arg(config.platform),
            // Single-project semantics: msbuild leaves SolutionDir
            // undefined when no .sln is involved, which breaks the
            // OutDir=$(SolutionDir)$(Configuration) templates. Pinning it
            // to the project directory gives the same output location the
            // user would get from building the project inside an IDE.
            QStringLiteral("/p:SolutionDir=%1\\").arg(project_.projectDir),
            QStringLiteral("/m"),
            QStringLiteral("/nologo"),
            QStringLiteral("/v:m"),
        };
        step.workingDirectory = project_.projectDir;
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
                        .arg(project_.filePath));
            }
            return {};
        }

        LightCpp::Plugin::BuildStep step;
        step.program = vcbuild;
        step.arguments = QStringList{
            project_.filePath,
            QStringLiteral("%1|%2").arg(config.name, config.platform),
        };
        step.workingDirectory = project_.projectDir;
        step.label = tr("vcbuild [%1|%2]").arg(config.name, config.platform);
        return {step};
    }

    // --- toolchain location -------------------------------------------------

    /// The newest MSBuild vswhere knows about. -prerelease matches what the
    /// IDE's own Msvc::detect does (preview installs do ship msbuild), and
    /// `-find` with a glob walks MSBuild\Current and the odd years where the
    /// directory layout differed, without any per-version guesswork here.
    [[nodiscard]] QString locateMsBuild() const
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

    LightCpp::Plugin::IdeServices* services_ = nullptr;
    /// The most recently opened project, kept for buildSteps(). The IDE
    /// opens one project at a time and always opens before building.
    LightCpp::Project::MsvcProject project_;
    bool open_ = false;
};

}  // namespace LightCpp::Plugins::Msvc

#include "MsvcBackend.moc"
