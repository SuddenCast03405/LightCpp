// QtCppBackend — Qt for C++ (qmake .pro) support for LightC++.
//
// qmake turns a .pro file into a Makefile; this backend runs it in a
// per-configuration shadow-build directory, then runs make/nmake there.
// The output path follows qmake's layout (Debug/Release subdirectories on
// Windows, a bundle on macOS, plain build directory elsewhere), and the
// backend reports it per configuration so Run Project picks the binary that
// was actually built.

#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"
#include "PluginSupport.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace LightCpp::Plugins::QtCpp {

class QtCppBackend final : public QObject,
                           public LightCpp::Plugin::Plugin,
                           public LightCpp::Plugin::ProjectBackend {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.lightcpp.Plugin/1.0" FILE "metadata.json")
    Q_INTERFACES(LightCpp::Plugin::Plugin LightCpp::Plugin::ProjectBackend)

public:
    QString name() const override { return QStringLiteral("QtCppProject"); }
    QString version() const override { return QStringLiteral("1.0.0"); }
    QString publisher() const override { return QStringLiteral("LightCpp"); }

    bool initialize(LightCpp::Plugin::IdeServices* services) override
    {
        services_ = services;
        if (services_ != nullptr) {
            services_->log(tr("Qt for C++ support ready (qmake .pro)."));
        }
        return true;
    }

    void shutdown() override { services_ = nullptr; }

    bool canOpenProject(const QString& filePath) const override
    {
        return QFileInfo(filePath).suffix().compare(QLatin1String("pro"),
                                                    Qt::CaseInsensitive)
               == 0;
    }

    QString projectFilter() const override { return tr("Qt C++ project (*.pro)"); }

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
                *error = readError.isEmpty() ? tr("The .pro file is empty.") : readError;
            }
            return false;
        }

        const QString dir = QFileInfo(absolute).absolutePath();
        const QString stem = QFileInfo(absolute).completeBaseName();
        ProFile parsed = parseProFile(text, dir);
        if (parsed.target.isEmpty()) {
            parsed.target = stem;
        }
        if (parsed.target.contains(QLatin1String("$$")) || parsed.target.isEmpty()) {
            parsed.target = stem;
        }
        if (parsed.target.isEmpty()) {
            if (error != nullptr) {
                *error = tr("The .pro file has no usable TARGET and no file name.");
            }
            return false;
        }

        projectDir_ = dir;
        projectFile_ = absolute;
        target_ = parsed.target;
        destDir_ = parsed.destDir;
        template_ = parsed.templateName;
        config_ = parsed.config;
        appBundle_ = parsed.appBundle;
        staticLib_ = parsed.staticLib;
        sourceCount_ = parsed.sourceCount;
        open_ = true;

        const bool subdirs = template_ == QLatin1String("subdirs");
        const bool executable = template_ == QLatin1String("app");
        if (summary != nullptr) {
            *summary = {};
            summary->filePath = absolute;
            summary->projectDir = dir;
            summary->name = target_;
            summary->formatName = tr("Qt (qmake)");
            if (subdirs) {
                summary->targetLabel = tr("Qt subdirs project");
            } else if (executable) {
                summary->targetLabel = tr("Qt C++ application");
            } else if (template_ == QLatin1String("lib")) {
                summary->targetLabel = staticLib_ ? tr("Qt static library")
                                                  : tr("Qt shared library");
            } else {
                summary->targetLabel = tr("Qt project");
            }
            summary->outputPath = executable ? outputPath(currentConfiguration())
                                             : QString();
            summary->unitCount = sourceCount_;
            summary->buildUnitCount = sourceCount_;
            summary->producesExecutable = executable && !subdirs;
        }

        log(tr("Opened Qt project \"%1\" (TEMPLATE=%2, CONFIG=%3).")
                .arg(target_, template_, config_.join(QLatin1Char(' '))));
        return true;
    }

    QList<LightCpp::Plugin::BuildStep>
    configureSteps(const LightCpp::Plugin::BuildRequest& request) override
    {
        if (!open_) {
            return {};
        }
        const QString qmake = findQMake();
        if (qmake.isEmpty()) {
            log(tr("Cannot configure: qmake was not found on PATH. Add the Qt "
                   "bin directory to PATH or select a Qt kit."));
            return {};
        }
        return {qmakeStep(qmake, request.configuration)};
    }

    [[nodiscard]] QString outputPath(const QString& configuration) const override
    {
        if (!open_ || template_ != QLatin1String("app")) {
            return {};
        }
        const QString buildDir = buildDirectory(configuration);
        QString outputDir;
        if (!destDir_.isEmpty()) {
            const QString dest = expandForProject(destDir_, buildDir);
            outputDir = QDir::isAbsolutePath(dest) ? dest : QDir(buildDir).filePath(dest);
        } else {
#ifdef Q_OS_WIN
            // qmake's Windows specs place a single-config build in
            // debug/ or release/ under the build directory.
            outputDir = QDir(buildDir).filePath(configuration.toLower());
#else
            outputDir = buildDir;
#endif
        }

#ifdef Q_OS_MACOS
        if (appBundle_) {
            return QDir(outputDir).filePath(target_ + QStringLiteral(".app/Contents/MacOS/")
                                            + target_);
        }
#endif
        return QDir(outputDir).filePath(target_ + Support::hostExecutableSuffix());
    }

    QList<LightCpp::Plugin::BuildStep>
    buildSteps(const LightCpp::Plugin::BuildRequest& request) override
    {
        if (!open_) {
            return {};
        }
        const QString qmake = findQMake();
        if (qmake.isEmpty()) {
            log(tr("Cannot build: qmake was not found on PATH."));
            return {};
        }
        const QString make = findMake(request);
        if (make.isEmpty()) {
            log(tr("Cannot build: make/nmake was not found on PATH."));
            return {};
        }

        QList<LightCpp::Plugin::BuildStep> steps;
        steps.append(qmakeStep(qmake, request.configuration));

        LightCpp::Plugin::BuildStep makeStep;
        makeStep.program = make;
        makeStep.arguments = request.extraMakeArgs;
        makeStep.workingDirectory = buildDirectory(request.configuration);
        makeStep.label = tr("make in %1 (%2)")
                             .arg(QDir::toNativeSeparators(makeStep.workingDirectory),
                                  request.configuration);
        steps.append(makeStep);
        return steps;
    }

private:
    struct ProFile {
        QString target;
        QString destDir;
        QString templateName = QStringLiteral("app");
        QStringList config;
        int sourceCount = 0;
        bool appBundle = false;
        bool staticLib = false;
    };

    [[nodiscard]] static QStringList tokenizeValues(const QString& value)
    {
        static const QRegularExpression pattern(
            QStringLiteral("(\"[^\"]*\"|'[^']*'|[^\\s]+)"));
        QStringList tokens;
        QRegularExpressionMatchIterator it = pattern.globalMatch(value);
        while (it.hasNext()) {
            tokens.append(Support::unquote(it.next().captured(1)));
        }
        return tokens;
    }

    [[nodiscard]] static ProFile parseProFile(const QString& text,
                                              const QString& projectDir)
    {
        ProFile parsed;
        static const QRegularExpression assignment(
            QStringLiteral("^\\s*([A-Za-z_][A-Za-z0-9_.]*)\\s*(\\+=|-=|\\*=|~=|\\|=|=)\\s*(.*)$"));

        QString logicalLine;
        const QStringList physical = Support::splitLines(text);
        for (const QString& raw : physical) {
            QString line = Support::stripHashComment(raw);
            // qmake folds a backslash continuation into one logical line, and a
            // logical line is exactly one statement. Gluing a complete line to
            // the next one would turn `TARGET = hello` into the prefix of
            // whatever follows, dropping the target.
            const bool continued = line.trimmed().endsWith(QLatin1Char('\\'));
            if (continued) {
                line = line.trimmed();
                line.chop(1);
            }
            logicalLine += line;
            if (continued) {
                continue;
            }
            const QString statement = logicalLine.trimmed();
            logicalLine.clear();
            if (statement.isEmpty()) {
                continue;
            }
            const QRegularExpressionMatch match = assignment.match(statement);
            if (!match.hasMatch()) {
                continue;
            }
            const QString variable = match.captured(1);
            const QString op = match.captured(2);
            const QStringList values = tokenizeValues(match.captured(3));

            if (variable == QLatin1String("TARGET") && op == QLatin1String("=")) {
                parsed.target = values.isEmpty() ? QString() : values.first();
            } else if (variable == QLatin1String("DESTDIR")
                       && (op == QLatin1String("=") || op == QLatin1String("+="))) {
                if (!values.isEmpty()) {
                    parsed.destDir = values.first();
                }
            } else if (variable == QLatin1String("TEMPLATE")
                       && op == QLatin1String("=")) {
                if (!values.isEmpty()) {
                    parsed.templateName = values.first().toLower();
                }
            } else if (variable == QLatin1String("CONFIG")) {
                if (op == QLatin1String("=")) {
                    parsed.config = values;
                } else if (op == QLatin1String("-=")) {
                    for (const QString& value : values) {
                        // QList::removeAll has no case-sensitivity overload in Qt 6.
                        // Remove matching CONFIG entries explicitly, from the end so
                        // duplicate values are handled without invalidating indices.
                        for (qsizetype index = parsed.config.size(); index > 0; --index) {
                            const qsizetype current = index - 1;
                            if (parsed.config.at(current).compare(
                                    value, Qt::CaseInsensitive) == 0) {
                                parsed.config.removeAt(current);
                            }
                        }
                    }
                } else {
                    parsed.config.append(values);
                }
            } else if (variable == QLatin1String("SOURCES")
                       && (op == QLatin1String("+=") || op == QLatin1String("="))) {
                for (const QString& value : values) {
                    if (value.contains(QLatin1String("$$"))
                        || value.contains(QLatin1Char('*'))) {
                        continue;
                    }
                    const QString path = QDir(projectDir).filePath(value);
                    if (QFileInfo(path).isFile()) {
                        ++parsed.sourceCount;
                    }
                }
            }
        }

        if (parsed.sourceCount == 0) {
            parsed.sourceCount = Support::countFilesWithSuffix(
                projectDir, {QStringLiteral("cpp"), QStringLiteral("c"),
                             QStringLiteral("cc"), QStringLiteral("cxx")});
        }
        parsed.config.removeDuplicates();
        for (QString& value : parsed.config) {
            value = value.toLower();
        }
        parsed.appBundle = parsed.config.contains(QStringLiteral("app_bundle"));
        parsed.staticLib = parsed.config.contains(QStringLiteral("staticlib"));
        return parsed;
    }

    /// Expands the qmake path variables that commonly appear in DESTDIR.
    [[nodiscard]] QString expandForProject(const QString& value,
                                           const QString& buildDir) const
    {
        QString expanded = value;
        expanded.replace(QLatin1String("$$_PRO_FILE_PWD_"), projectDir_);
        expanded.replace(QLatin1String("$${_PRO_FILE_PWD_}"), projectDir_);
        expanded.replace(QLatin1String("$$PWD"), projectDir_);
        expanded.replace(QLatin1String("$${PWD}"), projectDir_);
        expanded.replace(QLatin1String("$$OUT_PWD"), buildDir);
        expanded.replace(QLatin1String("$${OUT_PWD}"), buildDir);
        return expanded;
    }

    [[nodiscard]] QString buildDirectory(const QString& configuration) const
    {
        return QDir(projectDir_).filePath(QStringLiteral("build/")
                                          + configuration.toLower());
    }

    [[nodiscard]] QString findQMake() const
    {
        return Support::findTool({QStringLiteral("qmake6"), QStringLiteral("qmake-qt6"),
                                  QStringLiteral("qmake")});
    }

    [[nodiscard]] static QString findMake(
        const LightCpp::Plugin::BuildRequest& request)
    {
        const QString compilerName = QFileInfo(request.compiler).fileName().toLower();
        if (compilerName == QLatin1String("cl.exe") || compilerName == QLatin1String("cl")) {
            const QString nmake = Support::findTool({QStringLiteral("nmake")});
            if (!nmake.isEmpty()) {
                return nmake;
            }
        }
        if (!request.make.trimmed().isEmpty()
            && QFileInfo::exists(request.make)) {
            return request.make;
        }
        return Support::findTool({QStringLiteral("make"), QStringLiteral("gmake"),
                                  QStringLiteral("mingw32-make")});
    }

    [[nodiscard]] QString currentConfiguration() const { return QStringLiteral("Debug"); }

    [[nodiscard]] LightCpp::Plugin::BuildStep
    qmakeStep(const QString& qmake, const QString& configuration) const
    {
        const QString config = configuration.toLower();
        const QString other = config == QLatin1String("release")
                                  ? QStringLiteral("debug")
                                  : QStringLiteral("release");
        LightCpp::Plugin::BuildStep step;
        step.program = qmake;
        step.arguments = {QStringLiteral("-o"), QStringLiteral("Makefile"),
                          projectFile_, QStringLiteral("CONFIG+=") + config,
                          QStringLiteral("CONFIG-=") + other,
                          QStringLiteral("CONFIG-=debug_and_release")};
        step.workingDirectory = buildDirectory(configuration);
        step.label = tr("qmake (%1)").arg(configuration);
        return step;
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
        target_.clear();
        destDir_.clear();
        template_ = QStringLiteral("app");
        config_.clear();
        appBundle_ = false;
        staticLib_ = false;
        sourceCount_ = 0;
    }

    LightCpp::Plugin::IdeServices* services_ = nullptr;
    bool open_ = false;
    QString projectFile_;
    QString projectDir_;
    QString target_;
    QString destDir_;
    QString template_;
    QStringList config_;
    bool appBundle_ = false;
    bool staticLib_ = false;
    int sourceCount_ = 0;
};

}  // namespace LightCpp::Plugins::QtCpp

#include "QtCppBackend.moc"
