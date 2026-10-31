#include "LightCpp/Project/MsvcProject.hpp"
#include "LightCpp/Project/ProjectPathUtils.hpp"

#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <functional>

#include "LightCpp/Project/ProjectText.hpp"

namespace LightCpp::Project {
namespace {

/// Resolves a project-relative file reference ("..\src\main.cpp") against the
/// project directory.
QString resolveFilePath(const QString& projectDir, const QString& reference)
{
    return Paths::resolveReference(projectDir, reference);
}

/// The key a resolved path is de-duplicated by.
///
/// Windows paths are case-insensitive, so ".\Main.cpp" and "main.cpp" name
/// the same file; comparing the raw strings kept both, so the file appeared
/// twice in the tree and its compile count doubled. QDir::cleanPath does not
/// fold case, hence the explicit lower-casing.
QString dedupKey(const QString& path)
{
#ifdef Q_OS_WIN
    return path.toCaseFolded();
#else
    return Paths::isWindowsDriveAbsolutePath(path) ? path.toCaseFolded()
                                                   : path;
#endif
}

/// Where a file reference lands in the source/header/resource/other split.
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
    if (suffix == QLatin1String("rc") || suffix == QLatin1String("rc2")) {
        return FileSlot::Resource;
    }
    return FileSlot::Other;
}

void appendProjectFile(MsvcProject& project, FileSlot slot, const QString& path)
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

MsvcTargetKind targetKindFromVcxproj(const QString& type)
{
    if (type == QLatin1String("DynamicLibrary")) {
        return MsvcTargetKind::DynamicLibrary;
    }
    if (type == QLatin1String("StaticLibrary")) {
        return MsvcTargetKind::StaticLibrary;
    }
    if (type == QLatin1String("Makefile")) {
        return MsvcTargetKind::Makefile;
    }
    return MsvcTargetKind::Application;
}

MsvcTargetKind targetKindFromVcproj(const QString& digit)
{
    // 0 = Makefile, 1 = Application, 2 = DynamicLibrary, 3 = StaticLibrary.
    if (digit == QLatin1String("2")) {
        return MsvcTargetKind::DynamicLibrary;
    }
    if (digit == QLatin1String("3")) {
        return MsvcTargetKind::StaticLibrary;
    }
    if (digit == QLatin1String("0")) {
        return MsvcTargetKind::Makefile;
    }
    return MsvcTargetKind::Application;
}

/// True when `condition` contains an `=='<value>'` (or `=="<value>"`) test —
/// MSBuild's textual equality comparison, in either quote style. A `!=`
/// comparison is deliberately not matched: the pattern includes the `==`.
bool conditionComparesTo(const QString& condition, const QString& value)
{
    return condition.contains(QStringLiteral("=='%1'").arg(value))
        || condition.contains(QStringLiteral("==\"%1\"").arg(value));
}

/// True when the Condition attribute selects exactly this configuration.
///
/// Matched textually rather than evaluated, because the condition shape is
/// fixed by the tooling. The full `'$(Configuration)|$(Platform)'=='Debug|Win32'`
/// form is the common one, but generators and hand-written projects also emit
/// the looser `'$(Configuration)'=='Debug'` and `'$(Platform)'=='Win32'`
/// conditions, and any of the three quotes may appear in either style — so all
/// are recognised. Anything else (a tool-version test, an `Exists(...)` probe)
/// selects no configuration and never matches.
bool conditionMatches(const QString& condition, const MsvcConfiguration& config)
{
    return conditionComparesTo(condition, config.name + u'|' + config.platform)
        || conditionComparesTo(condition, config.name)
        || conditionComparesTo(condition, config.platform);
}

/// True when an MSBuild condition names at least one of the project's known
/// configurations. Same shape as conditionMatches() — deliberately, so the two
/// never disagree about what a condition selects. Used to decide whether a
/// conditioned ItemGroup or a conditioned ExcludedFromBuild child still
/// describes the default compile set.
bool conditionMentionsAnyConfiguration(const QString& condition,
                                       const MsvcProject& project)
{
    for (const MsvcConfiguration& config : project.configurations) {
        if (conditionMatches(condition, config)) {
            return true;
        }
    }
    return false;
}

/// True when the ExcludedFromBuild child of a ClCompile/ResourceCompile
/// element turns compilation off for the whole project as this model can
/// tell: a true value without a condition excludes everywhere, and a
/// condition that names no known configuration is treated the same way.
/// A condition that keeps the file in at least one configuration leaves it
/// in the compile list — msbuild evaluates the real condition during the
/// build, the model only has to keep the tree and the unit counts sane.
bool excludedFromBuildEverywhere(const QDomElement& node,
                                 const MsvcProject& project)
{
    for (QDomElement excluded = node.firstChildElement(
             QStringLiteral("ExcludedFromBuild"));
         !excluded.isNull();
         excluded = excluded.nextSiblingElement(
             QStringLiteral("ExcludedFromBuild"))) {
        const QString value = excluded.text().trimmed().toLower();
        if (value != QLatin1String("true") && value != QLatin1String("1")) {
            continue;
        }
        const QString condition = excluded.attribute(QStringLiteral("Condition"));
        if (condition.isEmpty()
            || !conditionMentionsAnyConfiguration(condition, project)) {
            return true;
        }
    }
    return false;
}

QStringList splitSemicolonList(const QString& value)
{
    QStringList parts = value.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (QString& part : parts) {
        // MSVC writes these with native backslashes; the IDE's path handling
        // is forward-slash everywhere, so normalise on the way in.
        part = QDir::fromNativeSeparators(part.trimmed());
    }
    parts.removeAll(QString());
    return parts;
}

// --- .vcxproj ------------------------------------------------------------

/// Collects the conditioned properties of one PropertyGroup into `config`.
void readVcxprojPropertyGroup(const QDomElement& group,
                              MsvcConfiguration& config)
{
    for (QDomElement node = group.firstChildElement(); !node.isNull();
         node = node.nextSiblingElement()) {
        const QString text = node.text().trimmed();
        if (node.tagName() == QLatin1String("ConfigurationType") && !text.isEmpty()) {
            config.kind = targetKindFromVcxproj(text);
        } else if (node.tagName() == QLatin1String("OutDir") && !text.isEmpty()) {
            config.outDir = text;
        } else if (node.tagName() == QLatin1String("TargetName") && !text.isEmpty()) {
            config.targetName = text;
        }
    }
}

/// Reads an ItemGroup that lists files (ClCompile/ClInclude/...).
///
/// `groupCondition` is the ItemGroup's own Condition attribute, empty when
/// none. A condition naming none of the project's configurations demotes the
/// group's compilable files to otherFiles: they stay visible in the tree but
/// must not inflate the compile list. Files already listed elsewhere are
/// skipped — the same file regularly appears in an unconditioned group and
/// again in a conditioned one, and the duplicate used to be counted (and
/// shown) twice.
void readVcxprojFileGroup(const QDomElement& group, const QString& groupCondition,
                          MsvcProject& project, QSet<QString>& seenPaths)
{
    const bool groupBuildsNowhere =
        !groupCondition.isEmpty()
        && !conditionMentionsAnyConfiguration(groupCondition, project);
    for (QDomElement node = group.firstChildElement(); !node.isNull();
         node = node.nextSiblingElement()) {
        const QString include = node.attribute(QStringLiteral("Include"));
        if (include.isEmpty()) {
            continue;
        }
        // A single Include= may list several files separated by ';'.
        const QStringList listed = include.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString& entry : listed) {
            FileSlot slot = [&node, &entry] {
                if (node.tagName() == QLatin1String("ClCompile")) {
                    return FileSlot::Source;
                }
                if (node.tagName() == QLatin1String("ClInclude")) {
                    return FileSlot::Header;
                }
                if (node.tagName() == QLatin1String("ResourceCompile")) {
                    return FileSlot::Resource;
                }
                return slotForFile(entry);
            }();
            const QString path =
                resolveFilePath(project.projectDir, entry.trimmed());
            if (seenPaths.contains(dedupKey(path))) {
                continue;  // the other spelling already covered this file
            }
            seenPaths.insert(dedupKey(path));
            // ExcludedFromBuild (and a group condition nothing matches) keep
            // the file in the tree but out of the compile set.
            if ((slot == FileSlot::Source || slot == FileSlot::Resource)
                && (groupBuildsNowhere
                    || excludedFromBuildEverywhere(node, project))) {
                slot = FileSlot::Other;
            }
            appendProjectFile(project, slot, path);
        }
    }
}

/// True when the ItemGroup's children are ProjectConfiguration definitions.
bool isConfigurationsGroup(const QDomElement& group)
{
    QDomElement first = group.firstChildElement();
    return !first.isNull()
           && first.tagName() == QLatin1String("ProjectConfiguration");
}

/// Reads the ProjectConfigurations ItemGroup into the config list.
void readVcxprojConfigurations(const QDomElement& group, MsvcProject& project)
{
    for (QDomElement configElement =
             group.firstChildElement(QStringLiteral("ProjectConfiguration"));
         !configElement.isNull();
         configElement = configElement.nextSiblingElement(
             QStringLiteral("ProjectConfiguration"))) {
        MsvcConfiguration config;
        const QString include = configElement.attribute(QStringLiteral("Include"));
        QString configurationName;
        QString platform;
        for (QDomElement part = configElement.firstChildElement(); !part.isNull();
             part = part.nextSiblingElement()) {
            if (part.tagName() == QLatin1String("Configuration")) {
                configurationName = part.text().trimmed();
            } else if (part.tagName() == QLatin1String("Platform")) {
                platform = part.text().trimmed();
            }
        }
        if (include.isEmpty()) {
            continue;
        }
        config.name = configurationName;
        config.platform = platform;
        // "Debug|Win32" from Include= when the child elements are missing
        // (never seen in the wild, but harmless to honour).
        if (config.name.isEmpty() && include.contains(QLatin1Char('|'))) {
            const QStringList halves = include.split(QLatin1Char('|'));
            config.name = halves.value(0).trimmed();
            config.platform = halves.value(1).trimmed();
        }
        project.configurations.append(config);
    }
}

std::optional<MsvcProject> parseVcxproj(const QString& filePath,
                                        const QDomDocument& document)
{
    MsvcProject project;
    project.filePath = Paths::absoluteFilePath(filePath);
    project.projectDir = Paths::directoryForFile(project.filePath);
    project.isLegacyVcproj = false;

    // Project name: ProjectName in a configuration-less group, else the file
    // stem. RootNamespace is a namespace, not a name, and only serves as
    // fallback.
    QString namespaceFallback;

    // Every listed file, absolute — dedup across the whole project, since
    // the same path can arrive from an unconditioned group and a
    // conditioned one (or twice in one list).
    QSet<QString> seenPaths;

    // Two passes: the configurations must be known before the conditioned
    // PropertyGroups are matched against them, and nothing in the format
    // guarantees the ProjectConfigurations group comes first.
    for (QDomElement group = document.documentElement().firstChildElement();
         !group.isNull(); group = group.nextSiblingElement()) {
        if (group.tagName() == QLatin1String("ItemGroup")
            && isConfigurationsGroup(group)) {
            readVcxprojConfigurations(group, project);
        }
    }
    for (QDomElement group = document.documentElement().firstChildElement();
         !group.isNull(); group = group.nextSiblingElement()) {
        if (group.tagName() == QLatin1String("PropertyGroup")) {
            const QString condition = group.attribute(QStringLiteral("Condition"));
            // Configuration-less groups state defaults that msbuild applies
            // unless a conditioned group overrides them; conditioned ones
            // feed exactly their own configuration. Applying a conditioned
            // group after the unconditioned ones, in document order, gives
            // the later declaration the final word — which is what msbuild
            // does for the handful of properties this model tracks.
            for (MsvcConfiguration& config : project.configurations) {
                if (condition.isEmpty() || conditionMatches(condition, config)) {
                    readVcxprojPropertyGroup(group, config);
                }
            }
            if (condition.isEmpty()) {
                for (QDomElement node = group.firstChildElement(); !node.isNull();
                     node = node.nextSiblingElement()) {
                    const QString text = node.text().trimmed();
                    if (node.tagName() == QLatin1String("ProjectName") && !text.isEmpty()) {
                        project.name = text;
                    } else if (node.tagName() == QLatin1String("RootNamespace")
                               && !text.isEmpty()) {
                        namespaceFallback = text;
                    }
                }
            }
        } else if (group.tagName() == QLatin1String("ItemGroup")
                   && !isConfigurationsGroup(group)) {
            readVcxprojFileGroup(group,
                                 group.attribute(QStringLiteral("Condition")),
                                 project, seenPaths);
        }
    }

    if (project.name.isEmpty()) {
        project.name = namespaceFallback.isEmpty()
                           ? QFileInfo(project.filePath).completeBaseName()
                           : namespaceFallback;
    }
    return project;
}

// --- .vcproj -------------------------------------------------------------

std::optional<MsvcProject> parseVcproj(const QString& filePath,
                                       const QDomDocument& document)
{
    MsvcProject project;
    project.filePath = QFileInfo(filePath).absoluteFilePath();
    project.projectDir = QFileInfo(project.filePath).absolutePath();
    project.isLegacyVcproj = true;

    const QDomElement root = document.documentElement();
    project.name = root.attribute(QStringLiteral("Name"));
    if (project.name.isEmpty()) {
        project.name = QFileInfo(project.filePath).completeBaseName();
    }

    const QDomElement configurations =
        root.firstChildElement(QStringLiteral("Configurations"));
    for (QDomElement configElement =
             configurations.firstChildElement(QStringLiteral("Configuration"));
         !configElement.isNull();
         configElement =
             configElement.nextSiblingElement(QStringLiteral("Configuration"))) {
        MsvcConfiguration config;
        const QString fullName = configElement.attribute(QStringLiteral("Name"));
        if (fullName.contains(QLatin1Char('|'))) {
            const QStringList halves = fullName.split(QLatin1Char('|'));
            config.name = halves.value(0).trimmed();
            config.platform = halves.value(1).trimmed();
        } else {
            config.name = fullName.trimmed();
        }
        config.kind = targetKindFromVcproj(
            configElement.attribute(QStringLiteral("ConfigurationType")));
        config.outDir =
            configElement.attribute(QStringLiteral("OutputDirectory"));

        for (QDomElement tool = configElement.firstChildElement(QStringLiteral("Tool"));
             !tool.isNull(); tool = tool.nextSiblingElement(QStringLiteral("Tool"))) {
            if (tool.attribute(QStringLiteral("Name"))
                == QLatin1String("VCCLCompilerTool")) {
                config.defines = splitSemicolonList(
                    tool.attribute(QStringLiteral("PreprocessorDefinitions")));
                config.includeDirs = splitSemicolonList(
                    tool.attribute(QStringLiteral("AdditionalIncludeDirectories")));
            } else if (tool.attribute(QStringLiteral("Name"))
                       == QLatin1String("VCLinkerTool")) {
                const QString outputFile =
                    tool.attribute(QStringLiteral("OutputFile"));
                if (!outputFile.isEmpty()) {
                    config.outputFile = outputFile;
                }
            }
        }
        project.configurations.append(config);
    }

    // <Files> nests <Filter> directories with <File RelativePath="...">.
    // The same relative path can arrive twice (a file listed inside two
    // filters); count and show it once.
    QSet<QString> seenPaths;
    const std::function<void(const QDomElement&)> readFiles =
        [&project, &readFiles, &seenPaths](const QDomElement& parent) {
            for (QDomElement node = parent.firstChildElement(); !node.isNull();
                 node = node.nextSiblingElement()) {
                if (node.tagName() == QLatin1String("Filter")) {
                    readFiles(node);
                } else if (node.tagName() == QLatin1String("File")) {
                    const QString relative =
                        node.attribute(QStringLiteral("RelativePath"));
                    if (relative.isEmpty()) {
                        continue;
                    }
                    const QString path = resolveFilePath(
                        project.projectDir, relative.trimmed());
                    if (!seenPaths.contains(dedupKey(path))) {
                        seenPaths.insert(dedupKey(path));
                        appendProjectFile(project, slotForFile(relative), path);
                    }
                }
            }
        };
    readFiles(root.firstChildElement(QStringLiteral("Files")));

    return project;
}

}  // namespace

// --- macro expansion ------------------------------------------------------

QString expandMsvcMacros(const QString& value, const MsvcProject& project,
                         const MsvcConfiguration& config)
{
    QString expanded = value;
    const QString projectDirWithSlash =
        project.projectDir.endsWith(QLatin1Char('/'))
            ? project.projectDir
            : project.projectDir + QLatin1Char('/');

    // $(OutDir) references other macros (usually $(SolutionDir)$(Config)),
    // so expansion is iterative; a cycle or a pile of unknown macros simply
    // bottoms out after the passes.
    const QString targetExt = config.kind == MsvcTargetKind::DynamicLibrary
                                  ? QStringLiteral(".dll")
                                  : config.kind == MsvcTargetKind::StaticLibrary
                                        ? QStringLiteral(".lib")
                                        : QStringLiteral(".exe");
    for (int pass = 0; pass < 8 && expanded.contains(QStringLiteral("$(")); ++pass) {
        QString next = expanded;
        next.replace(QStringLiteral("$(ProjectDir)"), projectDirWithSlash);
        next.replace(QStringLiteral("$(ProjectName)"), project.name);
        next.replace(QStringLiteral("$(Configuration)"), config.name);
        next.replace(QStringLiteral("$(ConfigurationName)"), config.name);
        next.replace(QStringLiteral("$(Platform)"), config.platform);
        // Single-project semantics: the solution root is the project root,
        // with the trailing separator msbuild's own SolutionDir carries.
        next.replace(QStringLiteral("$(SolutionDir)"), projectDirWithSlash);
        if (!config.outDir.isEmpty()) {
            next.replace(QStringLiteral("$(OutDir)"), config.outDir);
        }
        if (!config.targetName.isEmpty()) {
            next.replace(QStringLiteral("$(TargetName)"), config.targetName);
        }
        next.replace(QStringLiteral("$(TargetExt)"), targetExt);
        next.replace(QStringLiteral("$(TargetFileName)"),
                     QStringLiteral("$(TargetName)$(TargetExt)"));
        if (next == expanded) {
            break;  // nothing changed — the rest are unknown macros
        }
        expanded = next;
    }
    // Unknown macros expand to empty, like msbuild does, rather than
    // surviving as "$(SomeProp)Debug\hello.exe" in a path nothing can open.
    while (true) {
        const qsizetype start = expanded.indexOf(QStringLiteral("$("));
        if (start < 0) {
            break;
        }
        const qsizetype end = expanded.indexOf(QLatin1Char(')'), start);
        if (end < 0) {
            break;
        }
        expanded.remove(start, end - start + 1);
    }
    // The IDE's path handling is forward-slash everywhere; $(OutDir) and
    // friends carry native backslashes, so normalise on the way out.
    return QDir::fromNativeSeparators(expanded);
}

// --- configuration selection ----------------------------------------------

QString msvcTargetLabel(MsvcTargetKind kind)
{
    switch (kind) {
    case MsvcTargetKind::DynamicLibrary:
        return QStringLiteral("Dynamic Library");
    case MsvcTargetKind::StaticLibrary:
        return QStringLiteral("Static Library");
    case MsvcTargetKind::Makefile:
        return QStringLiteral("Makefile");
    case MsvcTargetKind::Application:
        break;
    }
    return QStringLiteral("Application");
}

const MsvcConfiguration* MsvcProject::configurationFor(
    const QString& configName) const
{
    const MsvcConfiguration* first = nullptr;
    const MsvcConfiguration* win32 = nullptr;
    const MsvcConfiguration* x64 = nullptr;
    for (const MsvcConfiguration& config : configurations) {
        if (config.name.compare(configName, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (first == nullptr) {
            first = &config;
        }
        if (win32 == nullptr && config.platform.compare(QStringLiteral("Win32"), Qt::CaseInsensitive) == 0) {
            win32 = &config;
        }
        if (x64 == nullptr && config.platform.compare(QStringLiteral("x64"), Qt::CaseInsensitive) == 0) {
            x64 = &config;
        }
    }
    if (win32 != nullptr) {
        return win32;
    }
    if (x64 != nullptr) {
        return x64;
    }
    return first;
}

const MsvcConfiguration* MsvcProject::defaultConfiguration() const
{
    if (const MsvcConfiguration* debug = configurationFor(QStringLiteral("Debug"))) {
        return debug;
    }
    if (const MsvcConfiguration* release =
            configurationFor(QStringLiteral("Release"))) {
        return release;
    }
    return configurations.isEmpty() ? nullptr : &configurations.first();
}

QString MsvcProject::outputPathFor(const MsvcConfiguration& config) const
{
    if (config.kind == MsvcTargetKind::Makefile) {
        return {};
    }

    // Precedence, mirroring what the formats put where:
    //   .vcproj VCLinkerTool OutputFile (already carries $(OutDir)\x.exe)
    //   .vcxproj TargetName + the derived extension, inside OutDir
    //   fallback: project name
    QString relative;
    if (!config.outputFile.isEmpty()) {
        relative = expandMsvcMacros(config.outputFile, *this, config);
    } else {
        const QString outDir =
            expandMsvcMacros(config.outDir.isEmpty()
                                 ? QStringLiteral("$(Configuration)\\")
                                 : config.outDir,
                             *this, config);
        const QString target =
            expandMsvcMacros(config.targetName.isEmpty() ? name : config.targetName,
                             *this, config);
        const QString ext = config.kind == MsvcTargetKind::DynamicLibrary
                                ? QStringLiteral(".dll")
                                : config.kind == MsvcTargetKind::StaticLibrary
                                      ? QStringLiteral(".lib")
                                      : QStringLiteral(".exe");
        relative = outDir + target + ext;
    }

    const QString native = QDir::fromNativeSeparators(relative);
    if (QDir::isAbsolutePath(native)) {
        return QDir::cleanPath(native);
    }
    return QDir::cleanPath(QDir(projectDir).absoluteFilePath(native));
}

// --- dispatch --------------------------------------------------------------

std::optional<MsvcProject> parseMsvcProject(const QString& filePath,
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

    QDomDocument document;
    // setContent(QString) ignores the XML declaration's encoding attribute —
    // the bytes were already decoded by detection, which knows better than
    // the often-wrong "gb2312" these files declare. Default options leave
    // namespace processing off, so tagName() is the bare element name.
    const QDomDocument::ParseResult parsed = document.setContent(*text);
    if (!parsed) {
        return fail(QStringLiteral("XML parse error at line %1: %2")
                        .arg(parsed.errorLine)
                        .arg(parsed.errorMessage));
    }

    const QDomElement root = document.documentElement();
    const QString rootName = root.tagName();
    if (rootName == QLatin1String("VisualStudioProject")) {
        auto project = parseVcproj(filePath, document);
        if (!project.has_value()) {
            return std::nullopt;
        }
        if (!project->isValid()) {
            return fail(QStringLiteral("no configurations in %1").arg(filePath));
        }
        return project;
    }
    if (rootName == QLatin1String("Project")) {
        auto project = parseVcxproj(filePath, document);
        if (!project.has_value()) {
            return std::nullopt;
        }
        if (!project->isValid()) {
            return fail(QStringLiteral("no configurations in %1").arg(filePath));
        }
        return project;
    }
    return fail(QStringLiteral("%1 is not a .vcxproj or .vcproj file "
                               "(root element <%2>)")
                    .arg(filePath, rootName));
}

std::optional<MsvcProject> loadMsvcProject(const QString& filePath, QString* error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot open %1").arg(filePath);
        }
        return std::nullopt;
    }
    const QByteArray content = file.readAll();
    // A truncated read (locked file, dying disk) yields an empty or partial
    // document that would fail XML parsing with a misleading position —
    // catch the I/O error while the real cause is still on the file object.
    if (file.error() != QFileDevice::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("error reading %1: %2")
                         .arg(filePath)
                         .arg(file.error());
        }
        return std::nullopt;
    }
    return parseMsvcProject(filePath, content, error);
}

}  // namespace LightCpp::Project
