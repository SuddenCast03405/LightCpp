// RustBackend — Cargo support for LightC++.
//
// The descriptor is Cargo.toml. The backend reads the handful of keys that
// decide what to build and what to run ([package] name, [[bin]] names,
// [lib], [workspace]); everything else (profiles, dependencies, build
// scripts) is Cargo's business, so the build step is plain `cargo build`.

#include "LightCpp/Plugin/Plugin.hpp"
#include "LightCpp/Plugin/ProjectBackend.hpp"
#include "PluginSupport.hpp"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace LightCpp::Plugins::Rust {

class RustBackend final : public QObject,
                          public LightCpp::Plugin::Plugin,
                          public LightCpp::Plugin::ProjectBackend {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.lightcpp.Plugin/1.0" FILE "metadata.json")
    Q_INTERFACES(LightCpp::Plugin::Plugin LightCpp::Plugin::ProjectBackend)

public:
    QString name() const override { return QStringLiteral("RustProject"); }
    QString version() const override { return QStringLiteral("1.0.0"); }
    QString publisher() const override { return QStringLiteral("LightCpp"); }

    bool initialize(LightCpp::Plugin::IdeServices* services) override
    {
        services_ = services;
        if (services_ != nullptr) {
            services_->log(tr("Rust/Cargo project support ready (Cargo.toml)."));
        }
        return true;
    }

    void shutdown() override { services_ = nullptr; }

    bool canOpenProject(const QString& filePath) const override
    {
        return QFileInfo(filePath).fileName().compare(QLatin1String("Cargo.toml"),
                                                      Qt::CaseInsensitive)
               == 0;
    }

    QString projectFilter() const override
    {
        return tr("Rust package (Cargo.toml)");
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
                *error = readError.isEmpty() ? tr("Cargo.toml is empty.") : readError;
            }
            return false;
        }

        ParsedCargo parsed = parseCargoToml(text);
        const QString dir = QFileInfo(absolute).absolutePath();
        const QString dirName = QFileInfo(dir).fileName();
        if (parsed.packageName.isEmpty()) {
            parsed.packageName = dirName;
        }
        if (parsed.packageName.isEmpty()) {
            if (error != nullptr) {
                *error = tr("Cargo.toml names neither a package nor a usable directory.");
            }
            return false;
        }

        const bool hasSrcMain = QFileInfo(QDir(dir).filePath(QStringLiteral("src/main.rs"))).isFile();
        const bool hasBin = !parsed.binNames.isEmpty()
                            || (parsed.hasPackage && hasSrcMain);
        QString binName;
        if (!parsed.binNames.isEmpty()) {
            binName = parsed.binNames.first();
        } else if (hasSrcMain) {
            binName = parsed.packageName;
        }
        if (binName.isEmpty()) {
            binName = parsed.packageName;
        }

        const bool virtualWorkspace = parsed.hasWorkspace && !parsed.hasPackage;
        projectDir_ = dir;
        projectFile_ = absolute;
        packageName_ = parsed.packageName;
        binName_ = binName;
        hasBinary_ = hasBin && !virtualWorkspace;
        virtualWorkspace_ = virtualWorkspace;
        sourceCount_ = Support::countFilesWithSuffix(
            parsed.hasPackage ? QDir(dir).filePath(QStringLiteral("src")) : dir,
            {QStringLiteral("rs")});
        open_ = true;

        if (summary != nullptr) {
            *summary = {};
            summary->filePath = absolute;
            summary->projectDir = dir;
            summary->name = virtualWorkspace ? dirName : parsed.packageName;
            summary->formatName = tr("Rust (Cargo)");
            if (virtualWorkspace) {
                summary->targetLabel = tr("Cargo workspace");
            } else if (hasBinary_) {
                summary->targetLabel = tr("Rust binary");
            } else {
                summary->targetLabel = tr("Rust library");
            }
            summary->outputPath = hasBinary_ ? binaryPath(QStringLiteral("Debug")) : QString();
            summary->unitCount = sourceCount_;
            summary->buildUnitCount = sourceCount_;
            summary->producesExecutable = hasBinary_;
        }

        log(tr("Opened Cargo %1 \"%2\".")
                .arg(virtualWorkspace ? tr("workspace") : tr("package"),
                     summary != nullptr ? summary->name : parsed.packageName));
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
        return hasBinary_ ? binaryPath(configuration) : QString();
    }

    QList<LightCpp::Plugin::BuildStep>
    buildSteps(const LightCpp::Plugin::BuildRequest& request) override
    {
        if (!open_) {
            return {};
        }
        const QString cargo = Support::findTool({QStringLiteral("cargo")});
        if (cargo.isEmpty()) {
            log(tr("Cannot build: cargo was not found on PATH."));
            return {};
        }

        const bool release = request.configuration.contains(QLatin1String("release"),
                                                            Qt::CaseInsensitive);
        QStringList arguments;
        arguments << QStringLiteral("build");
        if (release) {
            arguments << QStringLiteral("--release");
        }
        if (virtualWorkspace_) {
            arguments << QStringLiteral("--workspace");
        }

        LightCpp::Plugin::BuildStep step;
        step.program = cargo;
        step.arguments = arguments;
        step.workingDirectory = projectDir_;
        step.label = virtualWorkspace_
                         ? tr("cargo build --workspace (%1)").arg(request.configuration)
                         : tr("cargo build %1 (%2)")
                               .arg(release ? QStringLiteral("--release") : QString(),
                                    request.configuration)
                               .trimmed();
        return {step};
    }

private:
    struct ParsedCargo {
        QString packageName;
        QStringList binNames;
        bool hasPackage = false;
        bool hasLib = false;
        bool hasWorkspace = false;
    };

    [[nodiscard]] static ParsedCargo parseCargoToml(const QString& text)
    {
        ParsedCargo parsed;
        QString section;
        for (const QString& rawLine : Support::splitLines(text)) {
            const QString line =
                Support::stripHashComment(rawLine).trimmed();
            if (line.isEmpty()) {
                continue;
            }
            if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
                section = line;
                if (section == QLatin1String("[package]")) {
                    parsed.hasPackage = true;
                } else if (section == QLatin1String("[lib]")) {
                    parsed.hasLib = true;
                } else if (section == QLatin1String("[workspace]")) {
                    parsed.hasWorkspace = true;
                }
                continue;
            }
            const qsizetype equals = line.indexOf(QLatin1Char('='));
            if (equals <= 0) {
                continue;
            }
            const QString key = line.left(equals).trimmed();
            const QString value = Support::unquote(line.mid(equals + 1).trimmed());
            if (key != QLatin1String("name")) {
                continue;
            }
            if (section == QLatin1String("[package]") && parsed.packageName.isEmpty()) {
                parsed.packageName = value;
            } else if (section == QLatin1String("[[bin]]")) {
                parsed.binNames.append(value);
            }
        }
        return parsed;
    }

    [[nodiscard]] QString binaryPath(const QString& configuration) const
    {
        const bool release = configuration.contains(QLatin1String("release"),
                                                    Qt::CaseInsensitive);
        const QString profile = release ? QStringLiteral("release") : QStringLiteral("debug");
        return QDir(projectDir_).filePath(QStringLiteral("target/") + profile
                                          + QLatin1Char('/') + binName_
                                          + Support::hostExecutableSuffix());
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
        packageName_.clear();
        binName_.clear();
        hasBinary_ = false;
        virtualWorkspace_ = false;
        sourceCount_ = 0;
    }

    LightCpp::Plugin::IdeServices* services_ = nullptr;
    bool open_ = false;
    QString projectFile_;
    QString projectDir_;
    QString packageName_;
    QString binName_;
    bool hasBinary_ = false;
    bool virtualWorkspace_ = false;
    int sourceCount_ = 0;
};

}  // namespace LightCpp::Plugins::Rust

#include "RustBackend.moc"
