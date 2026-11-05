// WxWidgetsBackend — wxWidgets support for LightC++.
//
// wxWidgets has no single project format of its own; wxSmith (the editor
// bundled with Code::Blocks) writes Code::Blocks .cbp projects, so that is
// the descriptor this backend reads. It supports every .cbp project,
// reporting itself as "wxWidgets" when the file references wx libraries and
// as "Code::Blocks" otherwise. Building delegates to the Code::Blocks
// command line, which already knows the wx-specific compiler and linker
// settings stored in the file.

#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"
#include "PluginSupport.hpp"

#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QXmlStreamReader>

namespace LightCpp::Plugins::WxWidgets {

class WxWidgetsBackend final : public QObject,
                               public LightCpp::Plugin::Plugin,
                               public LightCpp::Plugin::ProjectBackend {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.lightcpp.Plugin/1.0" FILE "metadata.json")
    Q_INTERFACES(LightCpp::Plugin::Plugin LightCpp::Plugin::ProjectBackend)

public:
    QString name() const override { return QStringLiteral("WxWidgetsProject"); }
    QString version() const override { return QStringLiteral("1.0.0"); }
    QString publisher() const override { return QStringLiteral("LightCpp"); }

    bool initialize(LightCpp::Plugin::IdeServices* services) override
    {
        services_ = services;
        if (services_ != nullptr) {
            services_->log(tr("wxWidgets project support ready (Code::Blocks .cbp)."));
        }
        return true;
    }

    void shutdown() override { services_ = nullptr; }

    bool canOpenProject(const QString& filePath) const override
    {
        return QFileInfo(filePath).suffix().compare(QLatin1String("cbp"),
                                                    Qt::CaseInsensitive)
               == 0;
    }

    QString projectFilter() const override
    {
        return tr("wxWidgets / Code::Blocks (*.cbp)");
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
                *error = readError.isEmpty() ? tr("The .cbp file is empty.") : readError;
            }
            return false;
        }

        ParsedProject parsed = parseCbp(text);
        if (parsed.targets.isEmpty()) {
            // A .cbp with no target still opens: the build step would fail
            // with a clear Code::Blocks message, not an open-time rejection.
            parsed.title = parsed.title.isEmpty() ? QFileInfo(absolute).completeBaseName()
                                                  : parsed.title;
        }
        projectDir_ = QFileInfo(absolute).absolutePath();
        projectFile_ = absolute;
        projectTitle_ = parsed.title.isEmpty() ? QFileInfo(absolute).completeBaseName()
                                               : parsed.title;
        targets_ = parsed.targets;
        unitCount_ = parsed.unitCount;
        buildUnitCount_ = parsed.buildUnitCount;
        isWxWidgets_ = text.contains(QLatin1String("wxmsw"), Qt::CaseInsensitive)
                       || text.contains(QLatin1String("wxWidgets"), Qt::CaseInsensitive)
                       || text.contains(QLatin1String("wx-config"), Qt::CaseInsensitive)
                       || text.contains(QLatin1String("$(#wx"), Qt::CaseInsensitive);
        open_ = true;

        CbTarget target = selectTarget(QStringLiteral("Debug"));
        // No target at all means no named output: selectTarget() returns a
        // default-constructed CbTarget then, whose type would otherwise read
        // as "executable" and point Run Project at a file nothing builds.
        const bool executable = !targets_.isEmpty() && target.type == 0;
        if (summary != nullptr) {
            *summary = {};
            summary->filePath = absolute;
            summary->projectDir = projectDir_;
            summary->name = projectTitle_;
            summary->formatName = isWxWidgets_ ? tr("wxWidgets") : tr("Code::Blocks");
            if (executable) {
                summary->targetLabel = isWxWidgets_ ? tr("wxWidgets application")
                                                    : tr("Application");
            } else if (target.type == 1) {
                summary->targetLabel = tr("Static library");
            } else if (target.type == 2) {
                summary->targetLabel = tr("Dynamic library");
            } else {
                summary->targetLabel = tr("Project");
            }
            summary->outputPath = targetOutputPath(target, QStringLiteral("Debug"));
            summary->unitCount = unitCount_;
            summary->buildUnitCount = buildUnitCount_;
            summary->producesExecutable = executable;
        }

        log(tr("Opened %1 project \"%2\" (%3 target(s)).")
                .arg(isWxWidgets_ ? tr("wxWidgets") : tr("Code::Blocks"),
                     projectTitle_)
                .arg(targets_.size()));
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
        if (!open_ || targets_.isEmpty()) {
            return {};
        }
        const CbTarget target = selectTarget(configuration);
        return target.type == 0 ? targetOutputPath(target, configuration) : QString();
    }

    QList<LightCpp::Plugin::BuildStep>
    buildSteps(const LightCpp::Plugin::BuildRequest& request) override
    {
        if (!open_) {
            return {};
        }
        const QString codeblocks = findCodeBlocks();
        if (codeblocks.isEmpty()) {
            log(tr("Cannot build: Code::Blocks (codeblocks) was not found on PATH."));
            return {};
        }
        const CbTarget target = selectTarget(request.configuration);
        const QString targetTitle = target.title.isEmpty() ? QStringLiteral("Debug")
                                                           : target.title;

        LightCpp::Plugin::BuildStep step;
        step.program = codeblocks;
        step.arguments = {QStringLiteral("--build"),
                          QStringLiteral("--target=") + targetTitle, projectFile_};
        step.workingDirectory = projectDir_;
        step.label = tr("Code::Blocks: build target %1 (%2)")
                         .arg(targetTitle, request.configuration);
        return {step};
    }

private:
    struct CbTarget {
        QString title;
        QString output;
        QString workingDir;
        int type = 0;  // 0 executable, 1 static library, 2 dynamic library
    };

    struct ParsedProject {
        QString title;
        QList<CbTarget> targets;
        int unitCount = 0;
        int buildUnitCount = 0;
    };

    [[nodiscard]] static ParsedProject parseCbp(const QString& text)
    {
        ParsedProject parsed;
        QXmlStreamReader reader(text);
        CbTarget current;
        bool inTarget = false;
        bool inUnit = false;
        bool unitCompiles = true;

        while (!reader.atEnd()) {
            const QXmlStreamReader::TokenType token = reader.readNext();
            if (token == QXmlStreamReader::StartElement) {
                const QString name = reader.name().toString();
                const QXmlStreamAttributes attributes = reader.attributes();
                if (name == QLatin1String("Target")) {
                    current = {};
                    current.title =
                        attributes.value(QStringLiteral("title")).toString();
                    inTarget = true;
                } else if (name == QLatin1String("Option")) {
                    if (inUnit) {
                        if (attributes.hasAttribute(QStringLiteral("compile"))) {
                            unitCompiles =
                                attributes.value(QStringLiteral("compile")).toString()
                                != QLatin1String("0");
                        }
                    } else if (inTarget) {
                        if (attributes.hasAttribute(QStringLiteral("output"))) {
                            current.output =
                                attributes.value(QStringLiteral("output")).toString();
                        }
                        if (attributes.hasAttribute(QStringLiteral("working_dir"))) {
                            current.workingDir =
                                attributes.value(QStringLiteral("working_dir")).toString();
                        }
                        if (attributes.hasAttribute(QStringLiteral("type"))) {
                            current.type =
                                attributes.value(QStringLiteral("type")).toString().toInt();
                        }
                    } else if (attributes.hasAttribute(QStringLiteral("title"))
                               && parsed.title.isEmpty()) {
                        parsed.title =
                            attributes.value(QStringLiteral("title")).toString();
                    }
                } else if (name == QLatin1String("Unit")) {
                    inUnit = true;
                    unitCompiles = true;
                    ++parsed.unitCount;
                }
            } else if (token == QXmlStreamReader::EndElement) {
                const QString name = reader.name().toString();
                if (name == QLatin1String("Target")) {
                    if (!current.title.isEmpty()) {
                        parsed.targets.append(current);
                    }
                    current = {};
                    inTarget = false;
                } else if (name == QLatin1String("Unit")) {
                    if (unitCompiles) {
                        ++parsed.buildUnitCount;
                    }
                    inUnit = false;
                }
            }
        }
        return parsed;
    }

    [[nodiscard]] CbTarget selectTarget(const QString& configuration) const
    {
        if (targets_.isEmpty()) {
            return {};
        }
        for (const CbTarget& target : targets_) {
            if (target.title.contains(configuration, Qt::CaseInsensitive)) {
                return target;
            }
        }
        return targets_.first();
    }

    [[nodiscard]] QString targetOutputPath(const CbTarget& target,
                                           const QString& configuration) const
    {
        if (target.type != 0) {
            return {};
        }
        QString relative = target.output;
        if (relative.isEmpty()) {
            relative = QStringLiteral("bin/%1/%2").arg(configuration, projectTitle_);
        }
        QString path = QDir(projectDir_).filePath(relative);
        if (QFileInfo(path).suffix().isEmpty()) {
            path += Support::hostExecutableSuffix();
        }
        return path;
    }

    [[nodiscard]] static QString findCodeBlocks()
    {
        QString found = Support::findTool({QStringLiteral("codeblocks"),
                                           QStringLiteral("CodeBlocks")});
        if (!found.isEmpty()) {
            return found;
        }
#ifdef Q_OS_MACOS
        for (const QString& bundle : {QStringLiteral("/Applications/CodeBlocks.app/Contents/MacOS/CodeBlocks"),
                                      QStringLiteral("/Applications/Code::Blocks.app/Contents/MacOS/CodeBlocks")}) {
            if (QFileInfo(bundle).isExecutable()) {
                return bundle;
            }
        }
#endif
        return {};
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
        projectTitle_.clear();
        targets_.clear();
        unitCount_ = 0;
        buildUnitCount_ = 0;
        isWxWidgets_ = false;
    }

    LightCpp::Plugin::IdeServices* services_ = nullptr;
    bool open_ = false;
    bool isWxWidgets_ = false;
    QString projectFile_;
    QString projectDir_;
    QString projectTitle_;
    QList<CbTarget> targets_;
    int unitCount_ = 0;
    int buildUnitCount_ = 0;
};

}  // namespace LightCpp::Plugins::WxWidgets

#include "WxWidgetsBackend.moc"
