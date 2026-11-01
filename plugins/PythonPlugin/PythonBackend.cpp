// PythonBackend — Python project support for LightC++.
//
// Python has no link step, so "Build" is a source check (compileall) and
// "Run" starts the interpreter with the entry script. The descriptor is one
// of pyproject.toml, setup.py or requirements.txt; the first two also carry
// the project name, the last one only marks the directory as Python.

#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"
#include "PluginSupport.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace LightCpp::Plugins::Python {

class PythonBackend final : public QObject,
                            public LightCpp::Plugin::Plugin,
                            public LightCpp::Plugin::ProjectBackend {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.lightcpp.Plugin/1.0" FILE "metadata.json")
    Q_INTERFACES(LightCpp::Plugin::Plugin LightCpp::Plugin::ProjectBackend)

public:
    QString name() const override { return QStringLiteral("PythonProject"); }
    QString version() const override { return QStringLiteral("1.0.0"); }
    QString publisher() const override { return QStringLiteral("LightCpp"); }

    bool initialize(LightCpp::Plugin::IdeServices* services) override
    {
        services_ = services;
        if (services_ != nullptr) {
            services_->log(tr("Python project support ready "
                              "(pyproject.toml / setup.py / requirements.txt)."));
        }
        return true;
    }

    void shutdown() override { services_ = nullptr; }

    bool canOpenProject(const QString& filePath) const override
    {
        const QString fileName = QFileInfo(filePath).fileName();
        return fileName.compare(QLatin1String("pyproject.toml"), Qt::CaseInsensitive) == 0
               || fileName.compare(QLatin1String("setup.py"), Qt::CaseInsensitive) == 0
               || fileName.compare(QLatin1String("requirements.txt"),
                                   Qt::CaseInsensitive)
                      == 0;
    }

    QString projectFilter() const override
    {
        return tr("Python project (pyproject.toml setup.py requirements.txt)");
    }

    bool openProject(const QString& filePath,
                     LightCpp::Plugin::ProjectSummary* summary,
                     QString* error) override
    {
        reset();

        const QString absolute = QFileInfo(filePath).absoluteFilePath();
        const QString dir = QFileInfo(absolute).absolutePath();
        const QString fileName = QFileInfo(absolute).fileName();
        QString readError;
        const QString text = Support::readTextFile(absolute, &readError);
        if (text.isEmpty() && !readError.isEmpty()) {
            if (error != nullptr) {
                *error = readError;
            }
            return false;
        }

        QString projectName;
        if (fileName.compare(QLatin1String("pyproject.toml"), Qt::CaseInsensitive) == 0) {
            projectName = nameFromPyProject(text);
        } else if (fileName.compare(QLatin1String("setup.py"), Qt::CaseInsensitive) == 0) {
            const QRegularExpression pattern(
                QStringLiteral("name\\s*=\\s*(?:\"([^\"]+)\"|'([^']+)')"));
            const QRegularExpressionMatch match = pattern.match(text);
            if (match.hasMatch()) {
                projectName = !match.captured(1).isEmpty() ? match.captured(1)
                                                            : match.captured(2);
            }
        }
        const QString dirName = QFileInfo(dir).fileName();
        if (projectName.isEmpty()) {
            projectName = dirName;
        }

        const QString entry = findEntryScript(dir, projectName);
        const QString python = Support::findTool({QStringLiteral("python"),
                                                  QStringLiteral("python3"),
                                                  QStringLiteral("py")});

        projectDir_ = dir;
        projectFile_ = absolute;
        projectName_ = projectName;
        entryScript_ = entry;
        pythonProgram_ = python;
        sourceCount_ = Support::countFilesWithSuffix(dir, {QStringLiteral("py")});
        open_ = true;

        const bool runnable = !entryScript_.isEmpty() && !pythonProgram_.isEmpty();
        if (summary != nullptr) {
            *summary = {};
            summary->filePath = absolute;
            summary->projectDir = dir;
            summary->name = projectName;
            summary->formatName = tr("Python");
            summary->targetLabel = entryScript_.isEmpty() ? tr("Python project")
                                                          : tr("Python application");
            summary->outputPath = entryScript_;
            summary->unitCount = sourceCount_;
            summary->buildUnitCount = sourceCount_;
            summary->producesExecutable = runnable;
            summary->runProgram = runnable ? pythonProgram_ : QString();
            if (runnable) {
                summary->runArguments = {entryScript_};
            }
        }

        if (python.isEmpty()) {
            log(tr("Opened Python project \"%1\", but no python interpreter was "
                   "found on PATH; Run and Build stay unavailable.")
                    .arg(projectName));
        } else {
            log(tr("Opened Python project \"%1\" (entry: %2).")
                    .arg(projectName,
                         entryScript_.isEmpty() ? tr("none found") : entryScript_));
        }
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
        Q_UNUSED(configuration);
        return entryScript_;
    }

    QList<LightCpp::Plugin::BuildStep>
    buildSteps(const LightCpp::Plugin::BuildRequest& request) override
    {
        if (!open_) {
            return {};
        }
        if (pythonProgram_.isEmpty()) {
            log(tr("Cannot check sources: no python interpreter was found on PATH."));
            return {};
        }

        // compileall writes __pycache__ next to the sources and returns
        // non-zero on the first syntax error, which is exactly the signal a
        // Build action needs. The exclusion keeps virtualenvs, caches and
        // build output out of the check.
        QStringList arguments;
        arguments << QStringLiteral("-m") << QStringLiteral("compileall")
                  << QStringLiteral("-q")
                  << QStringLiteral("-x")
                  << QStringLiteral("(.*__pycache__|.*[.]venv|.*venv|.*node_modules|.*build|.*dist)")
                  << QStringLiteral(".");

        LightCpp::Plugin::BuildStep step;
        step.program = pythonProgram_;
        step.arguments = arguments;
        step.workingDirectory = projectDir_;
        step.label = tr("python -m compileall (%1)").arg(request.configuration);
        return {step};
    }

private:
    [[nodiscard]] static QString nameFromPyProject(const QString& text)
    {
        QString section;
        for (const QString& rawLine : Support::splitLines(text)) {
            const QString line = Support::stripHashComment(rawLine).trimmed();
            if (line.isEmpty()) {
                continue;
            }
            if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
                section = line;
                continue;
            }
            if (section != QLatin1String("[project]")
                && section != QLatin1String("[tool.poetry]")) {
                continue;
            }
            const qsizetype equals = line.indexOf(QLatin1Char('='));
            if (equals <= 0) {
                continue;
            }
            if (line.left(equals).trimmed() == QLatin1String("name")) {
                return Support::unquote(line.mid(equals + 1).trimmed());
            }
        }
        return {};
    }

    [[nodiscard]] static QString findEntryScript(const QString& dir,
                                                 const QString& projectName)
    {
        const QDir root(dir);
        const QStringList directCandidates = {
            QStringLiteral("main.py"), QStringLiteral("__main__.py"),
            QStringLiteral("app.py"), QStringLiteral("run.py"),
        };
        for (const QString& candidate : directCandidates) {
            const QString path = root.filePath(candidate);
            if (QFileInfo(path).isFile()) {
                return path;
            }
        }

        const QString package = Support::normalizedName(projectName, QString());
        if (!package.isEmpty()) {
            const QString packageMain = root.filePath(package + QStringLiteral("/__main__.py"));
            if (QFileInfo(packageMain).isFile()) {
                return packageMain;
            }
        }

        const QString srcMain = root.filePath(QStringLiteral("src/main.py"));
        if (QFileInfo(srcMain).isFile()) {
            return srcMain;
        }
        if (!package.isEmpty()) {
            const QString srcPackageMain =
                root.filePath(QStringLiteral("src/") + package + QStringLiteral("/__main__.py"));
            if (QFileInfo(srcPackageMain).isFile()) {
                return srcPackageMain;
            }
        }
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
        projectName_.clear();
        entryScript_.clear();
        pythonProgram_.clear();
        sourceCount_ = 0;
    }

    LightCpp::Plugin::IdeServices* services_ = nullptr;
    bool open_ = false;
    QString projectFile_;
    QString projectDir_;
    QString projectName_;
    QString entryScript_;
    QString pythonProgram_;
    int sourceCount_ = 0;
};

}  // namespace LightCpp::Plugins::Python

#include "PythonBackend.moc"
