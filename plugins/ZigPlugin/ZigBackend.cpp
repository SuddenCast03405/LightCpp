// ZigBackend — Zig build support for LightC++.
//
// build.zig is a program, not a declarative project file, so the backend
// reads just enough of it to know whether a runnable artifact is declared
// and what it is called. The build itself always runs `zig build`; the
// standard optimize option maps Debug/Release onto -Doptimize.

#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"
#include "PluginSupport.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace LightCpp::Plugins::Zig {

class ZigBackend final : public QObject,
                         public LightCpp::Plugin::Plugin,
                         public LightCpp::Plugin::ProjectBackend {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.lightcpp.Plugin/1.0" FILE "metadata.json")
    Q_INTERFACES(LightCpp::Plugin::Plugin LightCpp::Plugin::ProjectBackend)

public:
    QString name() const override { return QStringLiteral("ZigProject"); }
    QString version() const override { return QStringLiteral("1.0.0"); }
    QString publisher() const override { return QStringLiteral("LightCpp"); }

    bool initialize(LightCpp::Plugin::IdeServices* services) override
    {
        services_ = services;
        if (services_ != nullptr) {
            services_->log(tr("Zig project support ready (build.zig)."));
        }
        return true;
    }

    void shutdown() override { services_ = nullptr; }

    bool canOpenProject(const QString& filePath) const override
    {
        return QFileInfo(filePath).fileName().compare(QLatin1String("build.zig"),
                                                      Qt::CaseInsensitive)
               == 0;
    }

    QString projectFilter() const override
    {
        return tr("Zig project (build.zig)");
    }

    bool openProject(const QString& filePath,
                     LightCpp::Plugin::ProjectSummary* summary,
                     QString* error) override
    {
        reset();

        const QString absolute = QFileInfo(filePath).absoluteFilePath();
        QString readError;
        const QString text = Support::readTextFile(absolute, &readError);
        if (text.isEmpty()) {
            if (error != nullptr) {
                *error = readError.isEmpty() ? tr("build.zig is empty.") : readError;
            }
            return false;
        }

        const QString dir = QFileInfo(absolute).absolutePath();
        const QString dirName = QFileInfo(dir).fileName();

        QString artifactName;
        bool executable = false;
        bool library = false;
        parseArtifacts(text, &artifactName, &executable, &library);
        if (artifactName.isEmpty()) {
            artifactName = dirName.isEmpty() ? QStringLiteral("app") : dirName;
        }

        projectDir_ = dir;
        projectFile_ = absolute;
        artifactName_ = artifactName;
        hasExecutable_ = executable;
        sourceCount_ = Support::countFilesWithSuffix(dir, {QStringLiteral("zig")});
        open_ = true;

        if (summary != nullptr) {
            *summary = {};
            summary->filePath = absolute;
            summary->projectDir = dir;
            summary->name = artifactName;
            summary->formatName = tr("Zig");
            summary->targetLabel = executable ? tr("Zig executable")
                                              : (library ? tr("Zig library")
                                                         : tr("Zig project"));
            summary->outputPath = executable ? outputPath(QStringLiteral("Debug"))
                                             : QString();
            summary->unitCount = sourceCount_;
            summary->buildUnitCount = sourceCount_;
            summary->producesExecutable = executable;
        }

        log(tr("Opened Zig project \"%1\" (%2).")
                .arg(artifactName,
                     executable ? tr("executable") : tr("no executable declared")));
        return true;
    }

    QList<LightCpp::Plugin::BuildStep>
    configureSteps(const LightCpp::Plugin::BuildRequest& request) override
    {
        Q_UNUSED(request);
        return {};
    }

    [[nodiscard]] QString outputPath(const QString& configuration) const override
    {
        Q_UNUSED(configuration);  // zig-out/bin is shared by all optimize modes.
        if (!open_ || !hasExecutable_) {
            return {};
        }
        return QDir(projectDir_).filePath(QStringLiteral("zig-out/bin/") + artifactName_
                                          + Support::hostExecutableSuffix());
    }

    QList<LightCpp::Plugin::BuildStep>
    buildSteps(const LightCpp::Plugin::BuildRequest& request) override
    {
        if (!open_) {
            return {};
        }
        const QString zig = Support::findTool({QStringLiteral("zig")});
        if (zig.isEmpty()) {
            log(tr("Cannot build: the Zig compiler (zig) was not found on PATH."));
            return {};
        }

        const bool release = request.configuration.contains(QLatin1String("release"),
                                                            Qt::CaseInsensitive);
        QStringList arguments;
        arguments << QStringLiteral("build")
                  << (release ? QStringLiteral("-Doptimize=ReleaseFast")
                              : QStringLiteral("-Doptimize=Debug"));

        LightCpp::Plugin::BuildStep step;
        step.program = zig;
        step.arguments = arguments;
        step.workingDirectory = projectDir_;
        step.label = tr("zig build (%1)").arg(request.configuration);
        return {step};
    }

private:
    /// Finds the first addExecutable/addStaticLibrary/addSharedLibrary call and
    /// the artifact name it declares. Both the current struct-literal API and
    /// the older positional API are recognised.
    static void parseArtifacts(const QString& text, QString* name, bool* executable,
                               bool* library)
    {
        const QString newExecutable = QStringLiteral(
            "addExecutable\\s*\\([\\s\\S]{0,600}?\\.name\\s*=\\s*\"([^\"]+)\"");
        const QString oldExecutable =
            QStringLiteral("addExecutable\\s*\\(\\s*\"([^\"]+)\"");
        const QString newLibrary = QStringLiteral(
            "add(?:Static|Shared)?Library\\s*\\([\\s\\S]{0,600}?\\.name\\s*=\\s*\"([^\"]+)\"");
        const QString oldLibrary = QStringLiteral(
            "add(?:Static|Shared)?Library\\s*\\(\\s*\"([^\"]+)\"");

        const QRegularExpression::PatternOptions options =
            QRegularExpression::DotMatchesEverythingOption;

        for (const QString& patternText : {newExecutable, oldExecutable}) {
            const QRegularExpression pattern(patternText, options);
            const QRegularExpressionMatch match = pattern.match(text);
            if (match.hasMatch()) {
                *name = match.captured(1);
                *executable = true;
                return;
            }
        }
        for (const QString& patternText : {newLibrary, oldLibrary}) {
            const QRegularExpression pattern(patternText, options);
            const QRegularExpressionMatch match = pattern.match(text);
            if (match.hasMatch()) {
                *name = match.captured(1);
                *library = true;
                return;
            }
        }
    }

    void log(const QString& message)
    {
        if (services_ != nullptr) {
            services_->log(message);
        }
    }

    void reset()
    {
        open_ = false;
        projectFile_.clear();
        projectDir_.clear();
        artifactName_.clear();
        hasExecutable_ = false;
        sourceCount_ = 0;
    }

    LightCpp::Plugin::IdeServices* services_ = nullptr;
    bool open_ = false;
    QString projectFile_;
    QString projectDir_;
    QString artifactName_;
    bool hasExecutable_ = false;
    int sourceCount_ = 0;
};

}  // namespace LightCpp::Plugins::Zig

#include "ZigBackend.moc"
