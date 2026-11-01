// GoBackend — Go module support for LightC++.
//
// A Go project is a directory with go.mod. There is no IDE-side build model
// for Go, so this backend parses only what it needs: the module path (for a
// display name and the output file) and the packages that declare
// `package main` (there is nothing to run otherwise). The build step is the
// real `go build`, with Release mapped to a stripped, trimpath binary.

#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"
#include "PluginSupport.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace LightCpp::Plugins::Go {

class GoBackend final : public QObject,
                        public LightCpp::Plugin::Plugin,
                        public LightCpp::Plugin::ProjectBackend {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.lightcpp.Plugin/1.0" FILE "metadata.json")
    Q_INTERFACES(LightCpp::Plugin::Plugin LightCpp::Plugin::ProjectBackend)

public:
    QString name() const override { return QStringLiteral("GoProject"); }
    QString version() const override { return QStringLiteral("1.0.0"); }
    QString publisher() const override { return QStringLiteral("LightCpp"); }

    bool initialize(LightCpp::Plugin::IdeServices* services) override
    {
        services_ = services;
        if (services_ != nullptr) {
            services_->log(tr("Go project support ready (go.mod)."));
        }
        return true;
    }

    void shutdown() override { services_ = nullptr; }

    // --- ProjectBackend ---------------------------------------------------

    bool canOpenProject(const QString& filePath) const override
    {
        return QFileInfo(filePath).fileName().compare(QLatin1String("go.mod"),
                                                      Qt::CaseInsensitive)
               == 0;
    }

    QString projectFilter() const override
    {
        // A literal file name is a valid wildcard pattern: Open Project then
        // only offers the descriptor this backend can actually read.
        return tr("Go module (go.mod)");
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
                *error = readError.isEmpty()
                             ? tr("go.mod is empty; a Go module needs a module directive.")
                             : readError;
            }
            return false;
        }

        QString modulePath;
        int sourceCount = 0;
        QStringList mainPackages = mainPackageDirectories(absolute, text, &sourceCount);
        for (const QString& line : Support::splitLines(text)) {
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith(QLatin1String("module "))) {
                modulePath = Support::unquote(trimmed.mid(7).trimmed());
                break;
            }
        }
        if (modulePath.isEmpty()) {
            if (error != nullptr) {
                *error = tr("go.mod has no module directive.");
            }
            return false;
        }

        const QString dir = QFileInfo(absolute).absolutePath();
        const QString fallbackName = QFileInfo(dir).fileName();
        QString baseName = Support::lastPathSegment(modulePath);
        if (baseName.isEmpty()) {
            baseName = fallbackName;
        }
        if (baseName.isEmpty()) {
            baseName = QStringLiteral("main");
        }

        // Prefer the module root when it is itself a main package — that is
        // the ordinary `go build .` layout. Otherwise take the first main
        // package directory (sorted, so the choice is stable) and build that;
        // several mains in one module are legitimate but only one can be the
        // Run target.
        QString mainPackage;
        if (mainPackages.contains(QStringLiteral("."))) {
            mainPackage = QStringLiteral(".");
        } else if (!mainPackages.isEmpty()) {
            mainPackage = mainPackages.first();
        }

        projectDir_ = dir;
        projectFile_ = absolute;
        modulePath_ = modulePath;
        baseName_ = baseName;
        mainPackage_ = mainPackage;
        sourceCount_ = sourceCount;
        open_ = true;

        const bool executable = !mainPackage_.isEmpty();
        const QString output = executable
                                   ? QDir(dir).filePath(baseName_
                                          + Support::hostExecutableSuffix())
                                   : QString();

        if (summary != nullptr) {
            *summary = {};
            summary->filePath = absolute;
            summary->projectDir = dir;
            summary->name = baseName;
            summary->formatName = tr("Go");
            summary->targetLabel = executable ? tr("Go executable") : tr("Go package");
            summary->outputPath = output;
            summary->unitCount = sourceCount;
            summary->buildUnitCount = sourceCount;
            summary->producesExecutable = executable;
        }

        if (services_ != nullptr) {
            services_->log(executable
                               ? tr("Opened Go module \"%1\" (main package %2).")
                                     .arg(modulePath_, mainPackage_)
                               : tr("Opened Go module \"%1\" (no main package; "
                                    "it builds as a library).")
                                     .arg(modulePath_));
        }
        return true;
    }

    QList<LightCpp::Plugin::BuildStep>
    configureSteps(const LightCpp::Plugin::BuildRequest& request) override
    {
        // `go build` resolves and downloads module dependencies itself, so
        // there is no separate configure action.
        Q_UNUSED(request);
        return {};
    }

    [[nodiscard]] QString outputPath(const QString& configuration) const override
    {
        Q_UNUSED(configuration);  // Go has no debug/release output layout.
        if (!open_ || mainPackage_.isEmpty()) {
            return {};
        }
        return QDir(projectDir_).filePath(baseName_
                                          + Support::hostExecutableSuffix());
    }

    QList<LightCpp::Plugin::BuildStep>
    buildSteps(const LightCpp::Plugin::BuildRequest& request) override
    {
        if (!open_) {
            return {};
        }
        const QString go = Support::findTool({QStringLiteral("go")});
        if (go.isEmpty()) {
            log(tr("Cannot build: the Go toolchain (go) was not found on PATH."));
            return {};
        }

        const bool release = request.configuration.contains(QLatin1String("release"),
                                                           Qt::CaseInsensitive);
        QStringList arguments;
        arguments << QStringLiteral("build");
        QString output;
        if (mainPackage_.isEmpty()) {
            // Compile every package without producing one linked binary —
            // the honest equivalent of building a library module.
            arguments << QStringLiteral("./...");
        } else {
            output = QDir(projectDir_).filePath(baseName_
                                                + Support::hostExecutableSuffix());
            if (release) {
                arguments << QStringLiteral("-trimpath")
                          << QStringLiteral("-ldflags=-s -w");
            }
            arguments << QStringLiteral("-o") << output;
            arguments << mainPackage_;
        }

        LightCpp::Plugin::BuildStep step;
        step.program = go;
        step.arguments = arguments;
        step.workingDirectory = projectDir_;
        step.label = mainPackage_.isEmpty()
                         ? tr("go build ./... (%1)").arg(request.configuration)
                         : tr("go build -o %1 (%2)")
                               .arg(QFileInfo(output).fileName(), request.configuration);
        return {step};
    }

private:
    [[nodiscard]] QStringList mainPackageDirectories(const QString& goModPath,
                                                     const QString& goModText,
                                                     int* sourceCount) const
    {
        Q_UNUSED(goModText);
        QStringList mains;
        const QString root = QFileInfo(goModPath).absolutePath();
        const QRegularExpression mainPattern(
            QStringLiteral("(?m)^\\s*package\\s+main\\b"));

        QDirIterator it(root, QDir::Files,
                        QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (it.hasNext()) {
            const QString path = it.next();
            if (!path.endsWith(QLatin1String(".go"), Qt::CaseInsensitive)
                || path.endsWith(QLatin1String("_test.go"), Qt::CaseInsensitive)) {
                continue;
            }
            const QString relative =
                QDir(root).relativeFilePath(path).replace(QLatin1Char('\\'),
                                                         QLatin1Char('/'));
            const QStringList parts =
                relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            bool skipped = false;
            for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
                if (Support::isSkippedDirectoryName(parts.at(i))) {
                    skipped = true;
                    break;
                }
            }
            if (skipped) {
                continue;
            }
            ++(*sourceCount);
            const QString text = Support::readTextFile(path);
            if (!mainPattern.match(text).hasMatch()) {
                continue;
            }
            QString packageDir = QFileInfo(path).absolutePath();
            if (packageDir == root) {
                packageDir = QStringLiteral(".");
            } else {
                packageDir = QDir(root).relativeFilePath(packageDir)
                                 .replace(QLatin1Char('\\'), QLatin1Char('/'));
                packageDir.prepend(QStringLiteral("./"));
            }
            if (!mains.contains(packageDir)) {
                mains.append(packageDir);
            }
        }
        mains.sort(Qt::CaseInsensitive);
        return mains;
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
        projectDir_.clear();
        projectFile_.clear();
        modulePath_.clear();
        baseName_.clear();
        mainPackage_.clear();
        sourceCount_ = 0;
    }

    LightCpp::Plugin::IdeServices* services_ = nullptr;
    bool open_ = false;
    QString projectFile_;
    QString projectDir_;
    QString modulePath_;
    QString baseName_;
    QString mainPackage_;
    int sourceCount_ = 0;
};

}  // namespace LightCpp::Plugins::Go

#include "GoBackend.moc"
