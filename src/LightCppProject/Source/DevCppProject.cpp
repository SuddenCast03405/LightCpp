#include "LightCpp/Project/DevCppProject.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>

#include <utility>

namespace LightCpp::Project {
namespace {

/// Dev-C++ separates list entries with this marker rather than with spaces,
/// so an entry may itself contain spaces.
const QString kEntrySeparator = QStringLiteral("_@@_");

/// Source extensions this IDE will hand to a compiler. Headers are excluded on
/// purpose: Dev-C++ lists them as units, and passing `foo.h` to g++ would make
/// it emit a precompiled `foo.h.gch` into the user's source tree.
bool isCompilableSource(const QString& fileName)
{
    static const QStringList suffixes = {
        QStringLiteral("c"),  QStringLiteral("cpp"), QStringLiteral("cc"),
        QStringLiteral("cxx"), QStringLiteral("c++"), QStringLiteral("cp"),
    };
    return suffixes.contains(QFileInfo(fileName).suffix().toLower());
}

bool isDigits(const QString& text)
{
    if (text.isEmpty()) {
        return false;
    }
    for (const QChar ch : text) {
        if (!ch.isDigit()) {
            return false;
        }
    }
    return true;
}

bool keyIs(const QString& key, const QString& name)
{
    return key.compare(name, Qt::CaseInsensitive) == 0;
}

struct IniEntry {
    QString section;
    QString key;
    QString value;
};

/// Minimal INI walk. Order is preserved because the unit list is ordered by
/// its [UnitN] sections and Dev-C++ cares about that order when linking.
QList<IniEntry> parseIni(const QString& text)
{
    QList<IniEntry> entries;
    QString section;
    const QStringList rawLines = text.split(u'\n');
    for (const QString& rawLine : rawLines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty() || line.startsWith(u'#') || line.startsWith(u';')) {
            continue;
        }
        if (line.startsWith(u'[') && line.endsWith(u']')) {
            section = line.mid(1, line.size() - 2).trimmed();
            continue;
        }
        const qsizetype equals = line.indexOf(u'=');
        if (equals <= 0) {
            continue;
        }
        entries.append({section, line.left(equals).trimmed(),
                        line.mid(equals + 1).trimmed()});
    }
    return entries;
}

QString lookup(const QList<IniEntry>& entries, const QString& section, const QString& key)
{
    for (const IniEntry& e : entries) {
        if (e.section.compare(section, Qt::CaseInsensitive) == 0 && keyIs(e.key, key)) {
            return e.value;
        }
    }
    return {};
}

/// Convenience overload so the many literal key names stay readable.
QString lookup(const QList<IniEntry>& entries, const QString& section, const char* key)
{
    return lookup(entries, section, QString::fromLatin1(key));
}

QString absoluteFrom(const QString& base, const QString& path)
{
    const QString native = QDir::fromNativeSeparators(path);
    if (native.isEmpty()) {
        return {};
    }
    if (QFileInfo(native).isAbsolute()) {
        return QDir::cleanPath(native);
    }
    return QDir::cleanPath(QDir(base).filePath(native));
}

/// Dev-C++ writes Windows paths with backslashes. The compilers here take
/// forward slashes everywhere, and it keeps the build log readable.
QStringList normalisedPaths(const QStringList& paths)
{
    QStringList out;
    out.reserve(paths.size());
    for (const QString& path : paths) {
        out.append(QDir::fromNativeSeparators(path));
    }
    return out;
}

/// Turns a Linker= entry into something the driver understands: switches and
/// paths go through untouched (a Qt-era .dev happily lists absolute .a files
/// there), anything else is read as a library name.
QStringList linkerArguments(const QStringList& raw)
{
    QStringList out;
    for (const QString& item : raw) {
        if (item.startsWith(u'-')) {
            out.append(item);
            continue;
        }
        const QString native = QDir::fromNativeSeparators(item);
        const QString suffix = QFileInfo(native).suffix().toLower();
        const bool looksLikeAFile = native.contains(u'/') || suffix == "a"
                                    || suffix == "lib" || suffix == "o"
                                    || suffix == "obj" || suffix == "so"
                                    || suffix == "dll";
        out.append(looksLikeAFile ? native : QStringLiteral("-l") + item);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------
QString DevCppProject::targetLabel() const
{
    switch (type) {
    case kDevCppStaticLibrary:
        return QStringLiteral("static library");
    case kDevCppDynamicLibrary:
        return QStringLiteral("dynamic library");
    default:
        return QStringLiteral("application");
    }
}

bool DevCppProject::isValid() const noexcept
{
    if (filePath.isEmpty()) {
        return false;
    }
    for (const DevCppUnit& unit : units) {
        if (unit.compile && isCompilableSource(unit.fileName)) {
            return true;
        }
    }
    return false;
}

QStringList DevCppProject::buildUnitPaths() const
{
    QStringList out;
    for (const DevCppUnit& unit : units) {
        if (unit.compile && isCompilableSource(unit.fileName)) {
            out.append(absoluteFrom(projectDir, unit.fileName));
        }
    }
    return out;
}

QString DevCppProject::outputDirectory() const
{
    if (exeOutput.isEmpty()) {
        return projectDir;
    }
    return absoluteFrom(projectDir, exeOutput);
}

QString DevCppProject::outputPath() const
{
    QString fileName;
    if (overrideOutput && !overrideOutputName.isEmpty()) {
        fileName = overrideOutputName;
    } else {
        // The artifact's extension follows what the project produces, so even
        // the unsupported static library reports a sensible name.
        const QString suffix = (type == kDevCppDynamicLibrary) ? QStringLiteral(".dll")
                               : (type == kDevCppStaticLibrary) ? QStringLiteral(".a")
                                                                : QStringLiteral(".exe");
        fileName = name + suffix;
    }
    return absoluteFrom(outputDirectory(), fileName);
}

QString DevCppProject::makefilePath() const
{
    if (customMakefile.isEmpty()) {
        return {};
    }
    return absoluteFrom(projectDir, customMakefile);
}

QString DevCppProject::iconObjectPath() const
{
    const QFileInfo dev(filePath);
    return QDir(QDir(QDir::tempPath()).filePath(QStringLiteral("lightcpp-dev")))
        .filePath(dev.completeBaseName() + QStringLiteral("-icon.o"));
}

QString DevCppProject::iconResourcePath() const
{
    return QFileInfo(iconObjectPath()).absolutePath() + QStringLiteral("/")
           + QFileInfo(filePath).completeBaseName() + QStringLiteral("-icon.rc");
}

QByteArray DevCppProject::iconResourceScript() const
{
    // windres wants a resource script, and Dev-C++ stores only the .ico path.
    // The numeric id is what the default Dev-C++ projects use.
    const QString script = QStringLiteral("1 ICON \"%1\"\n")
                               .arg(QDir::fromNativeSeparators(iconPath));
    return script.toUtf8();
}

// ---------------------------------------------------------------------------
// Argument building
// ---------------------------------------------------------------------------
QStringList splitDevList(const QString& value)
{
    QStringList out;
    const QStringList pieces = value.split(kEntrySeparator, Qt::SkipEmptyParts);
    for (const QString& piece : pieces) {
        QString entry = piece.trimmed();
        // A few writers quote entries that contain spaces.
        if (entry.size() >= 2 && entry.startsWith(u'"') && entry.endsWith(u'"')) {
            entry = entry.mid(1, entry.size() - 2);
        }
        if (!entry.isEmpty()) {
            out.append(entry);
        }
    }
    return out;
}

QStringList splitFlagString(const QString& value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    if (trimmed.contains(kEntrySeparator)) {
        return splitDevList(trimmed);
    }
    // No separator: hand-written files from the 4.x era put everything on one
    // line, with double quotes around anything containing a space.
    QStringList out;
    QString current;
    bool inQuotes = false;
    for (const QChar ch : trimmed) {
        if (ch == u'"') {
            inQuotes = !inQuotes;
            continue;
        }
        if (!inQuotes && ch.isSpace()) {
            if (!current.isEmpty()) {
                out.append(current);
                current.clear();
            }
            continue;
        }
        current.append(ch);
    }
    if (!current.isEmpty()) {
        out.append(current);
    }
    return out;
}

QStringList gnuBuildArguments(const DevCppProject& project, BuildConfiguration config,
                              const QString& compiler, const QString& outputPath,
                              const QString& iconObject)
{
    Q_UNUSED(compiler)  // the caller picks gcc vs g++ from IsCpp; it is not an argument
    QStringList args;
    args << QStringLiteral("-Wall") << QStringLiteral("-Wextra");
    args << flagsFor(config, CompilerFamily::Gnu);
    // No -std= is forced: Dev-C++ records the standard the user chose straight
    // into CppCompiler= (-std=c++11), and that is the one to honour.
    for (const QString& define : project.defines) {
        args << (define.startsWith(u'-') ? define : QStringLiteral("-D") + define);
    }
    for (const QString& include : project.includeDirs) {
        args << QStringLiteral("-I") << include;
    }
    args << project.compilerFlags;
    args << project.buildUnitPaths();
    if (!iconObject.isEmpty()) {
        args << iconObject;
    }
    if (project.type == kDevCppDynamicLibrary) {
        args << QStringLiteral("-shared");
    }
    args << QStringLiteral("-o") << outputPath;
    for (const QString& library : project.libraryDirs) {
        args << QStringLiteral("-L") << library;
    }
    args << linkerArguments(project.linkerFlags);
    return args;
}

QStringList windresArguments(const DevCppProject& project, const QString& objectFile)
{
    // -O coff is the object format the MinGW linker consumes.
    return {QStringLiteral("-i"), project.iconResourcePath(),
            QStringLiteral("-O"), QStringLiteral("coff"),
            QStringLiteral("-o"), objectFile};
}

QStringList makeArguments(const DevCppProject& project)
{
    QStringList args{QStringLiteral("-f"), project.makefilePath()};
    // -C keeps the makefile's own relative paths working no matter what the
    // IDE's working directory happens to be.
    args << QStringLiteral("-C") << project.projectDir;
    return args;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
std::optional<DevCppProject> parseDevProject(const QString& filePath,
                                             const QByteArray& content,
                                             QString* error)
{
    const auto fail = [error](const QString& reason) -> std::optional<DevCppProject> {
        if (error != nullptr) {
            *error = reason;
        }
        return std::nullopt;
    };

    if (filePath.isEmpty()) {
        return fail(QStringLiteral("no project file"));
    }
    if (content.isEmpty()) {
        return fail(QStringLiteral("the project file is empty"));
    }

    // A UTF-8 BOM would otherwise glue itself onto the first key.
    QString text = QString::fromUtf8(content);
    if (text.startsWith(QChar(0xFEFF))) {
        text.remove(0, 1);
    }

    const QList<IniEntry> entries = parseIni(text);

    DevCppProject project;
    project.filePath = QFileInfo(filePath).absoluteFilePath();
    project.projectDir = QFileInfo(project.filePath).absolutePath();
    project.name = lookup(entries, QStringLiteral("Project"), "Name");
    if (project.name.isEmpty()) {
        project.name = QFileInfo(project.filePath).completeBaseName();
    }
    project.isCpp = lookup(entries, QStringLiteral("Project"), "IsCpp") != QStringLiteral("0");
    const QString type = lookup(entries, QStringLiteral("Project"), "Type");
    project.type = type.isEmpty() ? 0 : type.toInt();

    // C++ projects record their switches in CppCompiler=, C projects in
    // Compiler=; use whichever the flavour calls for and fall back to the
    // other so a file that fills in only one still builds the same way.
    const QString primary = project.isCpp ? QStringLiteral("CppCompiler")
                                          : QStringLiteral("Compiler");
    const QString secondary = project.isCpp ? QStringLiteral("Compiler")
                                            : QStringLiteral("CppCompiler");
    QString flags = lookup(entries, QStringLiteral("Project"), primary);
    if (flags.isEmpty()) {
        flags = lookup(entries, QStringLiteral("Project"), secondary);
    }
    project.compilerFlags = splitFlagString(flags);

    project.linkerFlags = splitFlagString(
        lookup(entries, QStringLiteral("Project"), "Linker"));
    project.defines = splitDevList(
        lookup(entries, QStringLiteral("Project"), "PreprocDefines"));
    project.includeDirs = normalisedPaths(splitDevList(
        lookup(entries, QStringLiteral("Project"), "Includes")));
    project.libraryDirs = normalisedPaths(splitDevList(
        lookup(entries, QStringLiteral("Project"), "Libs")));

    project.exeOutput = lookup(entries, QStringLiteral("Project"), "ExeOutput");
    project.objectOutput = lookup(entries, QStringLiteral("Project"), "ObjectOutput");
    project.overrideOutput =
        lookup(entries, QStringLiteral("Project"), "OverrideOutput") == QStringLiteral("1");
    project.overrideOutputName =
        lookup(entries, QStringLiteral("Project"), "OverrideOutputName");
    project.useCustomMakefile =
        lookup(entries, QStringLiteral("Project"), "UseCustomMakefile") == QStringLiteral("1");
    project.customMakefile =
        lookup(entries, QStringLiteral("Project"), "CustomMakefile");
    project.runArguments = splitFlagString(
        lookup(entries, QStringLiteral("Project"), "CommandLine"));

    const QString icon = lookup(entries, QStringLiteral("Project"), "Icon");
    if (!icon.isEmpty()) {
        project.iconPath = absoluteFrom(project.projectDir, icon);
    }

    // Units are numbered. They are listed either as [UnitN] sections (what a
    // real project file has) or as UnitN= keys inside [Project] (what a
    // template has), and the two spellings can appear together — so both go
    // into one table keyed by N and come out in unit order, whichever form
    // each entry arrived in.
    struct RawUnit {
        QString fileName;
        bool compile = true;
        bool link = true;
    };
    QMap<int, RawUnit> unitsByIndex;
    QSet<QString> seenPaths;

    const auto consider = [&](int index, const QString& fileName,
                              bool compile, bool link) {
        // Unit numbering starts at 0 in the IDE's own templates, so index 0
        // is a legitimate unit and only a negative one is nonsense.
        if (index < 0 || fileName.trimmed().isEmpty()) {
            return;
        }
        const QString key = absoluteFrom(project.projectDir, fileName).toLower();
        if (seenPaths.contains(key) || unitsByIndex.contains(index)) {
            return;  // the other spelling already covered this unit
        }
        seenPaths.insert(key);
        unitsByIndex.insert(index, RawUnit{fileName, compile, link});
    };

    QStringList unitSections;
    for (const IniEntry& e : entries) {
        if (!e.section.startsWith(QStringLiteral("Unit"), Qt::CaseInsensitive)
            || !isDigits(e.section.mid(4))) {
            continue;  // skips "[UnitTests]" and friends
        }
        if (!unitSections.contains(e.section, Qt::CaseInsensitive)) {
            unitSections.append(e.section);
        }
    }
    for (const QString& section : std::as_const(unitSections)) {
        QString fileName = lookup(entries, section, "FileName");
        if (fileName.isEmpty()) {
            // Templates name the file CppName=/CName= instead.
            fileName = lookup(entries, section, "CppName");
        }
        if (fileName.isEmpty()) {
            fileName = lookup(entries, section, "CName");
        }
        const QString compile = lookup(entries, section, "Compile");
        const QString link = lookup(entries, section, "Link");
        consider(section.mid(4).toInt(), fileName,
                 compile.isEmpty() || compile != QStringLiteral("0"),
                 link.isEmpty() || link != QStringLiteral("0"));
    }
    for (const IniEntry& e : entries) {
        if (e.section.compare(QStringLiteral("Project"), Qt::CaseInsensitive) != 0
            || !e.key.startsWith(QStringLiteral("Unit"), Qt::CaseInsensitive)
            || !isDigits(e.key.mid(4))) {
            continue;  // UnitCount= and friends
        }
        consider(e.key.mid(4).toInt(), e.value, true, true);
    }
    for (auto it = unitsByIndex.constBegin(); it != unitsByIndex.constEnd(); ++it) {
        project.units.append({it.value().fileName, it.value().compile,
                              it.value().link});
    }

    if (project.units.isEmpty()) {
        return fail(QStringLiteral("the project lists no source files"));
    }
    if (!project.isValid()) {
        return fail(QStringLiteral("the project lists no compilable source files"));
    }
    return project;
}

std::optional<DevCppProject> loadDevProject(const QString& filePath, QString* error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot read %1").arg(filePath);
        }
        return std::nullopt;
    }
    const QByteArray content = file.readAll();
    file.close();
    return parseDevProject(filePath, content, error);
}

}  // namespace LightCpp::Project
