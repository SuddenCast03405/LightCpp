#include "LightCpp/Project/DspProject.hpp"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QRegularExpression>

#include "LightCpp/Project/ProjectText.hpp"

namespace LightCpp::Project {
namespace {

QString stripQuotes(const QString& value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.size() >= 2 && trimmed.startsWith(QLatin1Char('"'))
        && trimmed.endsWith(QLatin1Char('"'))) {
        return trimmed.mid(1, trimmed.size() - 2);
    }
    return trimmed;
}

DspTargetKind targetKindFromTargType(const QString& hexCode)
{
    // The documented Win32 TARGTYPE numbers; everything else that exists
    // (wizard-generated shapes) produces an exe too, so it lands here as
    // well. 0x0102 is the DLL, 0x0104 the static library.
    if (hexCode == QLatin1String("0x0102")) {
        return DspTargetKind::DynamicLibrary;
    }
    if (hexCode == QLatin1String("0x0104")) {
        return DspTargetKind::StaticLibrary;
    }
    return DspTargetKind::Application;
}

/// Where a file reference lands, by its own extension — the group name in
/// the .dsp says the same thing ("Source Files" vs "Header Files") but the
/// extension is the part that decides what msdev does with the file.
enum class FileSlot { Source, Header, Resource, Other };

FileSlot slotForFile(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("cpp") || suffix == QLatin1String("cxx")
        || suffix == QLatin1String("cc") || suffix == QLatin1String("c")) {
        return FileSlot::Source;
    }
    if (suffix == QLatin1String("h") || suffix == QLatin1String("hpp")
        || suffix == QLatin1String("hh") || suffix == QLatin1String("hxx")
        || suffix == QLatin1String("inl")) {
        return FileSlot::Header;
    }
    if (suffix == QLatin1String("rc") || suffix == QLatin1String("rc2")
        || suffix == QLatin1String("ico") || suffix == QLatin1String("bmp")
        || suffix == QLatin1String("cur")) {
        return FileSlot::Resource;
    }
    return FileSlot::Other;
}

void appendProjectFile(DspProject& project, FileSlot slot, const QString& path)
{
    switch (slot) {
    case FileSlot::Source:
        project.sourceFiles.append(path);
        break;
    case FileSlot::Header:
        project.headerFiles.append(path);
        break;
    case FileSlot::Resource:
        project.resourceFiles.append(path);
        break;
    case FileSlot::Other:
        project.otherFiles.append(path);
        break;
    }
}

QString resolveFilePath(const QString& projectDir, const QString& reference)
{
    const QString native = QDir::fromNativeSeparators(stripQuotes(reference));
    return QDir::cleanPath(QDir(projectDir).absoluteFilePath(native));
}

/// Splits a `# ADD CPP` argument line into its /D "NAME" definitions and
/// /I "DIR" include directories. VS6 writes flags either quoted after the
/// switch (`/D "WIN32"`) or glued (`/DWIN32`); both are honoured, the
/// switch-name flags (/YX, /FD, ...) are skipped.
void readAddCppArguments(const QString& arguments, DspConfiguration& config)
{
    static const QRegularExpression tokenSplit(QStringLiteral("\\s+(?=/)"));
    const QStringList tokens =
        arguments.trimmed().split(tokenSplit, Qt::SkipEmptyParts);
    for (QString token : tokens) {
        token = token.trimmed();
        if (!token.startsWith(QLatin1Char('/'))) {
            continue;
        }
        QString switchName;
        QString value;
        const qsizetype space = token.indexOf(QLatin1Char(' '));
        if (space < 0) {
            switchName = token.mid(1);
        } else {
            switchName = token.mid(1, space - 1);
            value = stripQuotes(token.mid(space + 1));
        }
        if (switchName.compare(QLatin1String("D"), Qt::CaseInsensitive) == 0
            && !value.isEmpty()) {
            config.defines.append(value);
        } else if (switchName.compare(QLatin1String("I"), Qt::CaseInsensitive) == 0
                   && !value.isEmpty()) {
            config.includeDirs.append(QDir::fromNativeSeparators(value));
        } else if (value.isEmpty()) {
            // Glued form: /DWIN32, /I"..\include" — no space before the
            // value. No other cl.exe switch starts with D or I, so a bare
            // prefix match is unambiguous.
            const QChar head = switchName.at(0).toLower();
            const QString rest = switchName.mid(1);
            if (!rest.isEmpty()) {
                if (head == QLatin1Char('d')) {
                    config.defines.append(stripQuotes(rest));
                } else if (head == QLatin1Char('i')) {
                    config.includeDirs.append(
                        QDir::fromNativeSeparators(stripQuotes(rest)));
                }
            }
        }
    }
}

/// The /out:"..." of a `# ADD LINK32` line, when present.
QString readLinkOutputFile(const QString& arguments)
{
    static const QRegularExpression outPattern(
        QStringLiteral("/out:\\s*\"([^\"]+)\""),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = outPattern.match(arguments);
    if (!match.hasMatch()) {
        return {};
    }
    return match.captured(1);
}

}  // namespace

std::optional<DspProject> parseDspProject(const QString& filePath,
                                          const QByteArray& content,
                                          QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };

    auto text = decodeProjectText(content, error);
    if (!text.has_value()) {
        return std::nullopt;
    }

    DspProject project;
    project.filePath = QFileInfo(filePath).absoluteFilePath();
    project.projectDir = QFileInfo(project.filePath).absolutePath();

    DspTargetKind pendingKind = DspTargetKind::Application;
    DspConfiguration* current = nullptr;
    // Configuration blocks (!IF "$(CFG)" == ...) live between "# Begin
    // Project" and "# Begin Target". A !IF inside the target section wraps
    // SOURCE= lines of a hand-rolled or tool-generated file and has nothing
    // to do with configurations — treating it as one used to manufacture a
    // ghost configuration in the drop-down.
    bool inTargetSection = false;
    for (const QString& rawLine : text->split(QLatin1Char('\n'))) {
        QString line = rawLine;
        while (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        line = line.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        if (line.startsWith(QLatin1String("# Begin Target"))) {
            inTargetSection = true;
            continue;
        }
        if (line.startsWith(QLatin1String("# End Target"))) {
            inTargetSection = false;
            continue;
        }

        // Project name from the file header: `# Microsoft Developer Studio
        // Project File - Name="hello" - Package Owner=<4>`.
        if (line.startsWith(QLatin1String("# Microsoft Developer Studio"))) {
            static const QRegularExpression namePattern(
                QStringLiteral("Name=\"([^\"]+)\""));
            const QRegularExpressionMatch match = namePattern.match(line);
            if (match.hasMatch()) {
                project.name = match.captured(1);
            }
            continue;
        }

        // `# TARGTYPE "Win32 (x86) Console Application" 0x0103` — the kind
        // applies to every configuration of the project. The line sits in
        // the file header, before any !IF block, but configurations are
        // also updated here so a file that broke that convention still
        // parses.
        if (line.startsWith(QLatin1String("# TARGTYPE"))) {
            static const QRegularExpression kindPattern(
                QStringLiteral("0x[0-9A-Fa-f]{4}"));
            const QRegularExpressionMatch match = kindPattern.match(line);
            if (match.hasMatch()) {
                const DspTargetKind kind =
                    targetKindFromTargType(match.captured(0).toLower());
                for (DspConfiguration& config : project.configurations) {
                    config.kind = kind;
                }
                pendingKind = kind;
            }
            continue;
        }

        // `CFG=lib - Win32 Debug` — the developer's last-selected
        // configuration. When the header Name= line is absent (hand-edited
        // or truncated files), its project half is a better name source
        // than the .dsp file stem, because the CFG name is what msdev
        // prints and matches.
        if (line.startsWith(QLatin1String("CFG=")) && project.name.isEmpty()) {
            const QString full = line.mid(4).trimmed();
            const qsizetype separator = full.lastIndexOf(QStringLiteral(" - "));
            if (separator > 0) {
                project.name = full.left(separator).trimmed();
            }
            continue;
        }

        // Configuration block openers: `!IF "$(CFG)" == "hello - Win32
        // Debug"` and `!ELSEIF ...`. Each starts one configuration — but
        // only outside the target section, where the SOURCE= lists live.
        if (!inTargetSection
            && (line.startsWith(QLatin1String("!IF"))
                || line.startsWith(QLatin1String("!ELSEIF")))) {
            static const QRegularExpression cfgPattern(
                QStringLiteral("\"([^\"]+-[^\"]+)\""));
            const QRegularExpressionMatch match = cfgPattern.match(line);
            if (!match.hasMatch()) {
                continue;
            }
            DspConfiguration config;
            config.kind = pendingKind;
            config.fullName = match.captured(1).trimmed();
            // "hello - Win32 Debug": the project name is the part before
            // the last " - ", the platform/name after it.
            const qsizetype separator = config.fullName.lastIndexOf(
                QStringLiteral(" - "));
            if (separator > 0) {
                const QString tail = config.fullName.mid(separator + 3);
                const QStringList halves = tail.split(QLatin1Char(' '));
                config.platform = halves.value(0).trimmed();
                config.name = halves.mid(1).join(QLatin1Char(' ')).trimmed();
            }
            if (config.name.isEmpty()) {
                config.name = config.fullName;
            }
            project.configurations.append(config);
            current = &project.configurations.last();
            continue;
        }

        if (line.startsWith(QLatin1String("!ENDIF"))) {
            current = nullptr;
            continue;
        }

        // The file list lives in the "# Begin Target" section, outside the
        // configuration blocks — extension decides the slot, not the group.
        if (line.startsWith(QLatin1String("SOURCE="))) {
            appendProjectFile(
                project, slotForFile(line),
                resolveFilePath(project.projectDir, line.mid(7)));
            continue;
        }

        if (current == nullptr) {
            continue;
        }

        // Inside a configuration block.
        if (line.startsWith(QLatin1String("# PROP Output_Dir"))) {
            // Space-separated: # PROP Output_Dir "Release"
            current->outDir =
                stripQuotes(line.mid(QString(QLatin1String("# PROP Output_Dir")).size()));
            continue;
        }
        if (line.startsWith(QLatin1String("# ADD CPP"))) {
            readAddCppArguments(line.mid(QString(QLatin1String("# ADD CPP")).size()),
                                *current);
            continue;
        }
        if (line.startsWith(QLatin1String("# ADD LINK32"))) {
            const QString output =
                readLinkOutputFile(line.mid(QString(QLatin1String("# ADD LINK32")).size()));
            if (!output.isEmpty()) {
                current->outputFile = output;
            }
            continue;
        }
    }

    if (project.name.isEmpty()) {
        project.name = QFileInfo(project.filePath).completeBaseName();
    }
    if (!project.isValid()) {
        return fail(
            QStringLiteral("no configurations found in %1").arg(filePath));
    }
    return project;
}

std::optional<DspProject> loadDspProject(const QString& filePath, QString* error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot open %1").arg(filePath);
        }
        return std::nullopt;
    }
    const QByteArray content = file.readAll();
    // A truncated read would surface as "no configurations found" — catch
    // the I/O error while the real cause is still on the file object.
    if (file.error() != QFileDevice::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("error reading %1: %2")
                         .arg(filePath)
                         .arg(file.error());
        }
        return std::nullopt;
    }
    return parseDspProject(filePath, content, error);
}

std::optional<DswWorkspace> parseDswWorkspace(const QString& filePath,
                                              const QByteArray& content,
                                              QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return std::nullopt;
    };

    auto text = decodeProjectText(content, error);
    if (!text.has_value()) {
        return std::nullopt;
    }

    DswWorkspace workspace;
    workspace.filePath = QFileInfo(filePath).absoluteFilePath();
    workspace.projectDir = QFileInfo(workspace.filePath).absolutePath();

    // `Project: "hello"=.\hello\hello.dsp - Package Owner=<4>` — one line
    // per member project; everything else is package boilerplate. The path
    // ends at the " - Package Owner" tail; paths with spaces exist in the
    // wild, so a greedy \S+ capture would truncate them — capture lazily
    // up to the tail, falling back to a bare token when the tail is
    // missing.
    static const QRegularExpression projectPattern(
        QStringLiteral("^Project:\\s*\"([^\"]*)\"\\s*=\\s*\"?([^\"]+?)\"?"
                       "\\s*(?:-\\s*Package Owner.*)?$"));
    for (const QString& rawLine : text->split(QLatin1Char('\n'))) {
        QString line = rawLine;
        while (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        const QRegularExpressionMatch match = projectPattern.match(line.trimmed());
        if (!match.hasMatch()) {
            continue;
        }
        DswProjectEntry entry;
        entry.name = match.captured(1);
        entry.dspPath = QDir::cleanPath(
            QDir(workspace.projectDir).absoluteFilePath(
                QDir::fromNativeSeparators(stripQuotes(match.captured(2)))));
        workspace.projects.append(entry);
    }

    if (!workspace.isValid()) {
        return fail(
            QStringLiteral("no projects listed in %1").arg(filePath));
    }
    return workspace;
}

std::optional<DswWorkspace> loadDswWorkspace(const QString& filePath, QString* error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot open %1").arg(filePath);
        }
        return std::nullopt;
    }
    const QByteArray content = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("error reading %1: %2")
                         .arg(filePath)
                         .arg(file.error());
        }
        return std::nullopt;
    }
    return parseDswWorkspace(filePath, content, error);
}

const DspConfiguration* DspProject::configurationFor(const QString& configName) const
{
    for (const DspConfiguration& config : configurations) {
        if (config.name.compare(configName, Qt::CaseInsensitive) == 0) {
            return &config;
        }
    }
    return nullptr;
}

const DspConfiguration* DspProject::defaultConfiguration() const
{
    // The CFG= line ("hello - Win32 Debug") names the configuration the
    // developer last had selected in msdev; that is the closest thing to a
    // default the format has.
    if (const DspConfiguration* debug = configurationFor(QStringLiteral("Debug"))) {
        return debug;
    }
    if (const DspConfiguration* release =
            configurationFor(QStringLiteral("Release"))) {
        return release;
    }
    return configurations.isEmpty() ? nullptr : &configurations.first();
}

QString DspProject::outputPathFor(const DspConfiguration& config) const
{
    // VS6 puts the output in <Output_Dir>\<project>.exe (or .dll/.lib by
    // TARGTYPE), unless the linker line overrides it with /out:.
    QString relative = config.outputFile;
    if (relative.isEmpty()) {
        const QString outDir =
            config.outDir.isEmpty() ? config.name : config.outDir;
        const QString ext = config.kind == DspTargetKind::DynamicLibrary
                                ? QStringLiteral(".dll")
                                : config.kind == DspTargetKind::StaticLibrary
                                      ? QStringLiteral(".lib")
                                      : QStringLiteral(".exe");
        relative = outDir + QLatin1Char('/') + name + ext;
    }
    const QString native = QDir::fromNativeSeparators(relative);
    if (QDir::isAbsolutePath(native)) {
        return QDir::cleanPath(native);
    }
    return QDir::cleanPath(QDir(projectDir).absoluteFilePath(native));
}

const DswProjectEntry* DswWorkspace::defaultProject() const
{
    const QString ownStem = QFileInfo(filePath).completeBaseName();
    for (const DswProjectEntry& entry : projects) {
        if (QFileInfo(entry.dspPath).completeBaseName().compare(ownStem,
                                                                Qt::CaseInsensitive)
            == 0) {
            return &entry;
        }
    }
    return projects.isEmpty() ? nullptr : &projects.first();
}

QString dspTargetLabel(DspTargetKind kind)
{
    switch (kind) {
    case DspTargetKind::DynamicLibrary:
        return QStringLiteral("Dynamic Library");
    case DspTargetKind::StaticLibrary:
        return QStringLiteral("Static Library");
    case DspTargetKind::Application:
        break;
    }
    return QStringLiteral("Application");
}

}  // namespace LightCpp::Project
