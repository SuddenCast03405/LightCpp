#include "LightCpp/Project/DevCppProject.hpp"
#include "LightCpp/Project/ProjectPathUtils.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>

#include <utility>

#include "LightCpp/Text/Converter.hpp"
#include "LightCpp/Text/Encoding.hpp"

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
    return Paths::resolveReference(base, path);
}

/// Dev-C++ writes Windows paths with backslashes. The compilers here take
/// forward slashes everywhere, and it keeps the build log readable.
QStringList normalisedPaths(const QStringList& paths)
{
    QStringList out;
    out.reserve(paths.size());
    for (const QString& path : paths) {
        out.append(Paths::normalizeSeparators(path));
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
        const QString native = Paths::normalizeSeparators(item);
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
                               .arg(Paths::normalizeSeparators(iconPath));
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
// Makefile generation (Dev-C++ compatible)
// ---------------------------------------------------------------------------
namespace {

bool isHeaderSuffix(const QString& suffix)
{
    static const QStringList suffixes = {
        QStringLiteral("h"),  QStringLiteral("hpp"), QStringLiteral("hh"),
        QStringLiteral("hxx"), QStringLiteral("h++"),
    };
    return suffixes.contains(suffix);
}

/// ChangeFileExt: replace the last extension, wherever it sits.
QString withExtension(const QString& fileName, const QString& extension)
{
    const qsizetype slash = std::max(fileName.lastIndexOf(u'/'),
                                     fileName.lastIndexOf(u'\\'));
    const qsizetype dot = fileName.lastIndexOf(u'.');
    return dot > slash + 1 ? fileName.left(dot) + extension
                           : fileName + extension;
}

/// GenMakePath1: forward slashes, quoted when it contains a space.
QString makeQuotedPath(QString path)
{
    path = Paths::normalizeSeparators(path);
    if (path.contains(u' ')) {
        return QChar(u'"') + path + QChar(u'"');
    }
    return path;
}

/// GenMakePath2: forward slashes, spaces escaped for use as a make target.
QString makeEscapedPath(QString path)
{
    return Paths::normalizeSeparators(path).replace(QChar(u' '),
                                                    QStringLiteral("\\ "));
}

QString pathRelativeTo(const QString& base, const QString& path)
{
    return Paths::relativePath(base, path);
}

/// The object file a unit compiles to, relative to the project directory —
/// either next to the source or collected under ObjectOutput=, like the
/// original IDE's WriteMakeDefines does.
QString objectFileFor(const DevCppProject& project, const DevCppUnit& unit)
{
    const QString source = pathRelativeTo(
        project.projectDir, absoluteFrom(project.projectDir, unit.fileName));
    const QFileInfo info(source);
    const QString base = info.fileName();
    // Generating the object name drops the source's own extension, so a.c and
    // a.cpp both want a.o: their file names differ but their targets do not.
    const QString objectName = withExtension(base, QStringLiteral(".o"));
    const bool collected = !project.objectOutput.trimmed().isEmpty();

    // Count the units whose object target would equal ours, and how many of
    // those live in our own folder — a folded sub-path only separates units
    // that sit apart. Without ObjectOutput= the objects stay next to their
    // sources, so units in other folders never meet; only same-folder clashes
    // count there.
    int collisions = 0;
    int sameFolder = 0;
    for (const DevCppUnit& other : project.units) {
        if (&other == &unit || (!other.compile && !other.link)) {
            continue;  // not an object participant
        }
        const QString otherSuffix =
            QFileInfo(other.fileName).suffix().toLower();
        if (isHeaderSuffix(otherSuffix) || otherSuffix == QLatin1String("rc")) {
            continue;
        }
        const QFileInfo otherInfo(pathRelativeTo(
            project.projectDir,
            absoluteFrom(project.projectDir, other.fileName)));
        const bool sameDir =
            QString::compare(otherInfo.path(), info.path(), Qt::CaseInsensitive)
            == 0;
        if (!collected && !sameDir) {
            continue;
        }
        if (QString::compare(
                withExtension(otherInfo.fileName(), QStringLiteral(".o")),
                objectName, Qt::CaseInsensitive)
            != 0) {
            continue;
        }
        ++collisions;
        if (sameDir) {
            ++sameFolder;
        }
    }

    // ObjectOutput= collects every unit in one directory, so two sources with
    // the same target would overwrite each other's object file. Only the
    // colliding units fold their sub-path into the name (src/util.cpp ->
    // src_util.o): everything else keeps the original IDE's classic flat name,
    // so existing projects build identically.
    QString name = objectName;
    if (collisions > 0) {
        QString prefix;
        if (collected) {
            QString dir = info.path();
            if (dir != QStringLiteral(".")) {
                prefix = dir.replace(QChar(u'/'), QChar(u'_')) + u'_';
            }
        }
        if (sameFolder > 0) {
            // Same folder: the folded path is identical for both units, so it
            // separates nothing — fold the source's own name too, keeping a.c
            // and a.cpp from both becoming a.o (src/a.c -> src_a.c.o).
            name = prefix + base + QStringLiteral(".o");
        } else {
            name = prefix + name;
        }
    }
    if (!collected) {
        const QString dir = info.path();
        return dir == QStringLiteral(".") ? name : dir + u'/' + name;
    }
    return Paths::normalizeSeparators(
               pathRelativeTo(project.projectDir,
                              absoluteFrom(project.projectDir, project.objectOutput)))
        + u'/' + name;
}

/// One line of makefile text: a name padded to the classic column, then the
/// value. Dev-C++ lines the '=' up, and the output stays diff-friendly
/// against files the original IDE wrote.
QString makeVariable(const QString& name, const QString& value)
{
    return name.leftJustified(9) + QChar(u'=') + QChar(u' ') + value;
}

/// Recent MSYS2 toolchains ship g++/cc1plus with a UTF-8 active-code-page
/// manifest while ld/ar/collect2 have none — the linker chain decodes the
/// ANSI bytes of a Chinese output name as UTF-8 and the file lands on disk
/// as U+FFFD mush (probed: only g++/cc1plus carry activeCodePage). make
/// handing the bytes around does not fix that, so a non-ASCII target is
/// produced under an ASCII name first and moved by `cmd /c move` — cmd is a
/// Unicode program reached through the ANSI command line, where the GBK
/// bytes do round-trip.
bool isAsciiOnly(const QString& text)
{
    for (const QChar c : text) {
        if (c.unicode() >= 0x80) {
            return false;
        }
    }
    return true;
}

/// The ASCII stand-in a non-ASCII target is produced as. Fixed name so the
/// makefile stays reproducible; make overwrites it on every run.
QString intermediateTarget(const QString& suffix)
{
    return QStringLiteral("_lightcpp_link.") + suffix;
}

/// The target of a `cmd /c move` line: native slashes and always quoted, so
/// neither make's recipe handling nor cmd's switch parsing mangles it.
QString cmdQuotedPath(QString path)
{
    return QChar(u'"') + QDir::toNativeSeparators(path) + QChar(u'"');
}

/// The shared body of CXXFLAGS/CFLAGS: the warning level the IDE builds with,
/// the configuration's optimisation, the project's defines and switches.
QString flagTextFor(const DevCppProject& project, BuildConfiguration config)
{
    QStringList parts{QStringLiteral("-Wall"), QStringLiteral("-Wextra")};
    parts << flagsFor(config, CompilerFamily::Gnu);
    for (const QString& define : project.defines) {
        parts << (define.startsWith(u'-') ? define
                                          : QStringLiteral("-D") + define);
    }
    parts << project.compilerFlags;
    return parts.join(QChar(u' '));
}

}  // namespace

DevCppMakefilePlan devCppMakefilePlan(const DevCppProject& project,
                                      BuildConfiguration config,
                                      const DevCppToolchain& tools)
{
    DevCppMakefilePlan plan;
    plan.path = QDir(project.projectDir).filePath(QStringLiteral("Makefile.win"));

    // The tools named in the makefile. Dev-C++ writes absolute paths from its
    // compiler set; we use the kit's, falling back to bare names so a
    // hand-edited makefile still runs wherever PATH provides the tools.
    const QString gxx = tools.gxx.isEmpty()
                            ? QStringLiteral("g++")
                            : Paths::normalizeSeparators(tools.gxx);
    const QString gcc = tools.gcc.isEmpty()
                            ? (tools.gxx.isEmpty()
                                   ? QStringLiteral("gcc")
                                   : QFileInfo(gxx).dir().filePath(
                                         QStringLiteral("gcc.exe")))
                            : Paths::normalizeSeparators(tools.gcc);
    const QString windres = tools.windres.isEmpty()
                                ? (tools.gxx.isEmpty()
                                       ? QStringLiteral("windres.exe")
                                       : QFileInfo(gxx).dir().filePath(
                                             QStringLiteral("windres.exe")))
                                : Paths::normalizeSeparators(tools.windres);

    // --- objects ---------------------------------------------------------
    // The same list is needed in two forms, because make and the recipe shell
    // disagree about what a quote means. make matches targets and
    // prerequisites by name and does not strip quotes — measured for R2-27,
    // `OBJ = "my out/main.o"` against a rule target `my\ out/main.o` makes make
    // report `No rule to make target '"my', needed by 'hello.exe'` — so make's
    // own syntax needs the backslash-escaped form. The recipes are handed to
    // cmd.exe (measured: sh.exe is absent here and a recipe's `\ ` survives to
    // the command line untouched), which does not consume backslashes, so they
    // need the quoted form. The two forms are identical unless a path contains
    // a space, so both lists are built and only the affected projects differ.
    QStringList objects;
    QStringList objectsMake;
    QStringList linkObjects;
    for (const DevCppUnit& unit : project.units) {
        if (!unit.compile && !unit.link) {
            continue;  // not part of the build at all
        }
        const QString suffix = QFileInfo(unit.fileName).suffix().toLower();
        if (isHeaderSuffix(suffix) || suffix == QLatin1String("rc")) {
            continue;  // headers and resource scripts are not object sources
        }
        // A unit the project tells us not to compile is one we cannot build,
        // so it must not be named after an object file we never generate and
        // never taught make how to make — that turned a `Compile=0` .res unit
        // into a reference to a `foo.o` nothing could produce (R2-28). Link the
        // file the project names instead; only for a *source* file can
        // "Compile=0, Link=1" mean "link the object another build produced".
        const QString path =
            unit.compile || isCompilableSource(unit.fileName)
                ? objectFileFor(project, unit)
                : pathRelativeTo(project.projectDir,
                                 absoluteFrom(project.projectDir, unit.fileName));
        objects.append(makeQuotedPath(path));
        objectsMake.append(makeEscapedPath(path));
        if (unit.link) {
            linkObjects.append(makeQuotedPath(path));
        }
    }

    // --- resource --------------------------------------------------------
    // A PrivateResource= (an .rc inside the project, which Dev-C++ itself
    // writes for the project icon) wins; otherwise the Icon= becomes a
    // generated script in temp. Both compile to a .res the build links in.
    QString resSource;
    QString resObject;
    bool generatedResource = false;
    if (!project.privateResource.trimmed().isEmpty()) {
        resSource = pathRelativeTo(
            project.projectDir,
            absoluteFrom(project.projectDir, project.privateResource));
        resObject = project.objectOutput.trimmed().isEmpty()
                        ? withExtension(resSource, QStringLiteral(".res"))
                        : Paths::normalizeSeparators(pathRelativeTo(
                              project.projectDir,
                              absoluteFrom(project.projectDir,
                                           withExtension(
                                               project.objectOutput
                                                   + u'/'
                                                   + QFileInfo(resSource)
                                                         .fileName(),
                                               QStringLiteral(".res")))));
    } else if (!project.iconPath.isEmpty()) {
        resSource = project.iconResourcePath();
        resObject = project.iconObjectPath();
        generatedResource = true;
    }

    // --- variables -------------------------------------------------------
    // `OBJ`/`BIN` are what the recipes use (quoted); `OBJ_MK`/`BIN_MK` are the
    // make-syntax twins (escaped). See the object loop above for why both
    // exist; a project without a space in any path has them byte-identical.
    QStringList lines;
    lines << QStringLiteral("# Project: %1").arg(project.name);
    lines << QStringLiteral("# Makefile generated by LightC++ for Dev-C++");
    lines << QString();
    lines << makeVariable(QStringLiteral("CPP"), makeQuotedPath(gxx));
    lines << makeVariable(QStringLiteral("CC"), makeQuotedPath(gcc));
    lines << makeVariable(QStringLiteral("WINDRES"), makeQuotedPath(windres));
    if (!resObject.isEmpty()) {
        lines << makeVariable(QStringLiteral("RES"),
                              makeQuotedPath(resObject));
        lines << makeVariable(QStringLiteral("OBJ"),
                              objects.join(QChar(u' ')) + QStringLiteral(" $(RES)"));
        lines << makeVariable(QStringLiteral("OBJ_MK"),
                              objectsMake.join(QChar(u' ')) + QChar(u' ')
                                  + makeEscapedPath(resObject));
        lines << makeVariable(QStringLiteral("LINKOBJ"),
                              linkObjects.join(QChar(u' ')) + QStringLiteral(" $(RES)"));
    } else {
        lines << makeVariable(QStringLiteral("OBJ"),
                              objects.join(QChar(u' ')));
        lines << makeVariable(QStringLiteral("OBJ_MK"),
                              objectsMake.join(QChar(u' ')));
        lines << makeVariable(QStringLiteral("LINKOBJ"),
                              linkObjects.join(QChar(u' ')));
    }
    QStringList libs;
    for (const QString& dir : project.libraryDirs) {
        libs << QStringLiteral("-L") + makeQuotedPath(dir);
    }
    // A switch goes through as a switch, a file path by itself, and anything
    // else reads as a library name — the same rules gnuBuildArguments uses.
    for (QString entry : linkerArguments(project.linkerFlags)) {
        entry = Paths::normalizeSeparators(entry);
        if (entry.startsWith(u'-')) {
            libs << entry;
        } else {
            libs << makeQuotedPath(entry);
        }
    }
    lines << makeVariable(QStringLiteral("LIBS"), libs.join(QChar(u' ')));
    QStringList incs;
    for (const QString& dir : project.includeDirs) {
        incs << QStringLiteral("-I") + makeQuotedPath(dir);
    }
    const QString incText = incs.join(QChar(u' '));
    lines << makeVariable(QStringLiteral("INCS"), incText);
    lines << makeVariable(QStringLiteral("CXXINCS"), incText);
    const QString binPath = pathRelativeTo(project.projectDir,
                                           project.outputPath());
    lines << makeVariable(QStringLiteral("BIN"), makeQuotedPath(binPath));
    // The same value in make's own syntax: `$(BIN_MK)` is what the rule is
    // called and depended on as, `$(BIN)` is what the recipes pass to a
    // program. Both point at one file.
    lines << makeVariable(QStringLiteral("BIN_MK"), makeEscapedPath(binPath));
    // Whether the final target needs the ASCII-intermediate route (see
    // isAsciiOnly above).
    const bool routedTarget = !isAsciiOnly(binPath);
    // A dynamic library additionally produces these two next to it.
    QString defPath;
    QString staticPath;
    const QString flags = flagTextFor(project, config);
    lines << makeVariable(QStringLiteral("CXXFLAGS"),
                          QStringLiteral("$(CXXINCS) ") + flags);
    lines << makeVariable(QStringLiteral("CFLAGS"),
                          QStringLiteral("$(INCS) ") + flags);
    lines << makeVariable(QStringLiteral("RM"),
                          QStringLiteral("rm.exe -f"));
    if (project.type == kDevCppDynamicLibrary) {
        // A DLL also produces its import library and .def next to the output.
        const QString binDir = Paths::directoryForFile(project.outputPath());
        const QString libStem = Paths::normalizeSeparators(pathRelativeTo(
            project.projectDir,
            QDir(binDir).filePath(QStringLiteral("lib")
                                  + QFileInfo(project.outputPath()).fileName())));
        defPath = withExtension(libStem, QStringLiteral(".def"));
        staticPath = withExtension(libStem, QStringLiteral(".a"));
        lines << makeVariable(QStringLiteral("DEF"), makeQuotedPath(defPath));
        lines << makeVariable(QStringLiteral("STATIC"),
                              makeQuotedPath(staticPath));
    }
    lines << QString();

    // --- targets ---------------------------------------------------------
    lines << QStringLiteral(".PHONY: all all-before all-after clean "
                            "clean-custom");
    lines << QString();
    lines << QStringLiteral("all: all-before $(BIN_MK) all-after");
    lines << QString();
    for (const QString& include : project.makeIncludes) {
        // An `include` line is make's own syntax, not a recipe: a quoted name
        // is a file name with quote characters in it, and make would look for
        // `"my` and `file.mk"` (same reason as $(BIN_MK)).
        lines << QStringLiteral("include ")
              + makeEscapedPath(Paths::normalizeSeparators(include));
    }
    if (!project.makeIncludes.isEmpty()) {
        lines << QString();
    }
    lines << QStringLiteral("clean: clean-custom");
    lines << QChar(u'\t') + QStringLiteral("${RM} $(OBJ) $(BIN)");
    lines << QString();
    lines << QStringLiteral("$(BIN_MK): $(OBJ_MK)");
    const QString linker = project.isCpp ? QStringLiteral("$(CPP)")
                                         : QStringLiteral("$(CC)");
    switch (project.type) {
    case kDevCppStaticLibrary:
        if (routedTarget) {
            lines << QChar(u'\t')
                  + QStringLiteral("ar r ")
                  + intermediateTarget(QStringLiteral("a"))
                  + QStringLiteral(" $(LINKOBJ)");
            lines << QChar(u'\t')
                  + QStringLiteral("ranlib ")
                  + intermediateTarget(QStringLiteral("a"));
            lines << QChar(u'\t') + QStringLiteral("cmd /c move /y ")
                  + intermediateTarget(QStringLiteral("a"))
                  + QChar(u' ') + cmdQuotedPath(binPath);
        } else {
            lines << QChar(u'\t') + QStringLiteral("ar r $(BIN) $(LINKOBJ)");
            lines << QChar(u'\t') + QStringLiteral("ranlib $(BIN)");
        }
        break;
    case kDevCppDynamicLibrary:
        if (routedTarget) {
            lines << QChar(u'\t') + linker
                  + QStringLiteral(" -shared $(LINKOBJ) -o ")
                  + intermediateTarget(QStringLiteral("exe"))
                  + QStringLiteral(" $(LIBS) -Wl,--output-def,")
                  + intermediateTarget(QStringLiteral("def"))
                  + QStringLiteral(",--out-implib,")
                  + intermediateTarget(QStringLiteral("a"))
                  + QStringLiteral(",--add-stdcall-alias");
            lines << QChar(u'\t') + QStringLiteral("cmd /c move /y ")
                  + intermediateTarget(QStringLiteral("exe"))
                  + QChar(u' ') + cmdQuotedPath(binPath);
            lines << QChar(u'\t') + QStringLiteral("cmd /c move /y ")
                  + intermediateTarget(QStringLiteral("def"))
                  + QChar(u' ') + cmdQuotedPath(defPath);
            lines << QChar(u'\t') + QStringLiteral("cmd /c move /y ")
                  + intermediateTarget(QStringLiteral("a"))
                  + QChar(u' ') + cmdQuotedPath(staticPath);
        } else {
            lines << QChar(u'\t') + linker
                  + QStringLiteral(" -shared $(LINKOBJ) -o $(BIN) $(LIBS) "
                                   "-Wl,--output-def,$(DEF),--out-implib,$(STATIC)"
                                   ",--add-stdcall-alias");
        }
        break;
    default:
        if (routedTarget) {
            lines << QChar(u'\t') + linker
                  + QStringLiteral(" $(LINKOBJ) -o ")
                  + intermediateTarget(QStringLiteral("exe"))
                  + QStringLiteral(" $(LIBS)");
            lines << QChar(u'\t') + QStringLiteral("cmd /c move /y ")
                  + intermediateTarget(QStringLiteral("exe"))
                  + QChar(u' ') + cmdQuotedPath(binPath);
        } else {
            lines << QChar(u'\t') + linker
                  + QStringLiteral(" $(LINKOBJ) -o $(BIN) $(LIBS)");
        }
        break;
    }

    // --- per-unit rules --------------------------------------------------
    for (const DevCppUnit& unit : project.units) {
        if (!unit.compile) {
            continue;
        }
        const QString suffix = QFileInfo(unit.fileName).suffix().toLower();
        if (suffix == QLatin1String("rc") || isHeaderSuffix(suffix)
            || !isCompilableSource(unit.fileName)) {
            continue;
        }
        const QString obj = makeEscapedPath(objectFileFor(project, unit));
        const QString source = makeEscapedPath(pathRelativeTo(
            project.projectDir,
            absoluteFrom(project.projectDir, unit.fileName)));
        lines << QString();
        lines << obj + QStringLiteral(": ") + source;
        if (unit.overrideBuildCmd && !unit.buildCmd.trimmed().isEmpty()) {
            // <CRTAB> is how Dev-C++ spells a continued command line.
            lines << QChar(u'\t')
                  + QString(unit.buildCmd).replace(
                        QStringLiteral("<CRTAB>"),
                        QStringLiteral("\n\t"));
            continue;
        }
        lines << QChar(u'\t')
              + (unit.compileCpp ? QStringLiteral("$(CPP)")
                                 : QStringLiteral("$(CC)"))
              + QStringLiteral(" -c ") + makeQuotedPath(source)
              + QStringLiteral(" -o ") + makeQuotedPath(objectFileFor(project, unit))
              + (unit.compileCpp ? QStringLiteral(" $(CXXFLAGS)")
                                 : QStringLiteral(" $(CFLAGS)"));
    }

    // --- resource rule ---------------------------------------------------
    if (!resObject.isEmpty()) {
        lines << QString();
        lines << makeEscapedPath(resObject) + QStringLiteral(": ")
              + makeEscapedPath(resSource);
        lines << QChar(u'\t') + QStringLiteral("$(WINDRES) -i ")
              + makeQuotedPath(resSource)
              + QStringLiteral(" --input-format=rc -o ")
              + makeQuotedPath(resObject) + QStringLiteral(" -O coff");
    }

    const QString text = lines.join(QChar(u'\n')) + QChar(u'\n');

    // mingw32-make reads the file byte-for-byte and hands paths to the
    // compiler untouched, so the encoding is the machine's ANSI code page —
    // exactly what Dev-C++ writes. UTF-8 is the fallback when a character
    // cannot be represented (a non-Windows host, say).
    const auto encoded = LightCpp::Text::encodeFromQString(
        text, LightCpp::Text::localAnsiEncoding());
    plan.content = encoded.hasValue() ? encoded.value() : text.toUtf8();
    if (generatedResource) {
        // Same deal for the generated resource script: windres reads raw
        // bytes, and the icon path inside may contain non-ASCII.
        const auto script = LightCpp::Text::encodeFromQString(
            project.iconResourceScript(),
            LightCpp::Text::localAnsiEncoding());
        plan.resourceScript = script.hasValue() ? script.value()
                                                : project.iconResourceScript();
    }
    return plan;
}

bool writeDevCppMakefile(const DevCppMakefilePlan& plan,
                         const DevCppProject& project, QString* error)
{
    const auto fail = [error](const QString& reason) {
        if (error != nullptr) {
            *error = reason;
        }
        return false;
    };

    // The ObjectOutput= directory has to exist before make runs — Dev-C++
    // creates it while writing the makefile, not during the build. Same for
    // the output directory the link rule moves the target into.
    if (!project.objectOutput.trimmed().isEmpty()) {
        QDir(absoluteFrom(project.projectDir, project.objectOutput))
            .mkpath(QStringLiteral("."));
    }
    QDir(project.outputDirectory()).mkpath(QStringLiteral("."));
    if (!plan.resourceScript.isEmpty()) {
        QFile script(project.iconResourcePath());
        QDir().mkpath(Paths::directoryForFile(project.iconResourcePath()));
        if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || script.write(plan.resourceScript) != plan.resourceScript.size()) {
            return fail(QStringLiteral("cannot write the resource script %1")
                            .arg(project.iconResourcePath()));
        }
    }
    QDir().mkpath(Paths::directoryForFile(plan.path));
    QFile file(plan.path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(plan.content) != plan.content.size()) {
        return fail(QStringLiteral("cannot write %1").arg(plan.path));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------
std::optional<DevCppProject> parseDevProject(const QString& filePath,
                                             const QByteArray& content,
                                             QString* error,
                                             QStringList* warnings)
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

    // Dev-C++ is a pre-Unicode Windows program: it writes .dev files in the
    // machine's ANSI code page (GBK on zh-CN), so a project called 项目1 has
    // GBK bytes in Name=/OverrideOutputName=. Decoding those as UTF-8 would
    // silently replace them with U+FFFD and produce output paths no compiler
    // can open. Detection (BOM → UTF-8 validation → local ANSI) is exactly
    // what the editor uses for source files.
    const LightCpp::Text::EncodingDetection detected =
        LightCpp::Text::detectEncoding(content);
    QString text;
    if (detected.encoding == LightCpp::Text::TextEncoding::Utf8) {
        text = QString::fromUtf8(content);
    } else if (detected.encoding == LightCpp::Text::TextEncoding::Utf16Le
               || detected.encoding == LightCpp::Text::TextEncoding::Utf16Be
               || detected.encoding == LightCpp::Text::TextEncoding::Utf32Le
               || detected.encoding == LightCpp::Text::TextEncoding::Utf32Be
               || detected.encoding == LightCpp::Text::TextEncoding::Gb18030
               || detected.encoding == LightCpp::Text::TextEncoding::Gbk
               || detected.encoding == LightCpp::Text::TextEncoding::Big5
               || detected.encoding == LightCpp::Text::TextEncoding::ShiftJis
               || detected.encoding == LightCpp::Text::TextEncoding::EucKr
               || detected.encoding == LightCpp::Text::TextEncoding::Windows1252
               || detected.encoding == LightCpp::Text::TextEncoding::Latin1) {
        if (auto decoded = LightCpp::Text::decodeToQString(
                content, detected.encoding, /*stripBom=*/true)) {
            text = std::move(*decoded);
        } else {
            return fail(QStringLiteral("the project file is not valid in the "
                                       "detected encoding"));
        }
    } else {
        return fail(QStringLiteral("the project file is not text"));
    }

    // A UTF-8 BOM would otherwise glue itself onto the first key (the UTF-8
    // path above keeps it — decodeToQString only strips for the other paths).
    if (text.startsWith(QChar(0xFEFF))) {
        text.remove(0, 1);
    }

    const QList<IniEntry> entries = parseIni(text);

    DevCppProject project;
    project.filePath = Paths::absoluteFilePath(filePath);
    project.projectDir = Paths::directoryForFile(project.filePath);
    project.name = lookup(entries, QStringLiteral("Project"), "Name");
    if (project.name.isEmpty()) {
        project.name = QFileInfo(project.filePath).completeBaseName();
    }
    project.isCpp = lookup(entries, QStringLiteral("Project"), "IsCpp") != QStringLiteral("0");
    const QString type = lookup(entries, QStringLiteral("Project"), "Type");
    project.type = type.isEmpty() ? 0 : type.toInt();
    project.unitCount = lookup(entries, QStringLiteral("Project"), "UnitCount")
                            .toInt();

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
    project.privateResource =
        lookup(entries, QStringLiteral("Project"), "PrivateResource");
    project.makeIncludes = splitDevList(
        lookup(entries, QStringLiteral("Project"), "MakeIncludes"));
    project.runArguments = splitFlagString(
        lookup(entries, QStringLiteral("Project"), "CommandLine"));

    const QString icon = lookup(entries, QStringLiteral("Project"), "Icon");
    if (!icon.isEmpty()) {
        project.iconPath = absoluteFrom(project.projectDir, icon);
    }

    const auto warn = [warnings](const QString& message) {
        if (warnings != nullptr) {
            warnings->append(message);
        }
    };

    // Units are numbered. They are listed either as [UnitN] sections (what a
    // real project file has) or as UnitN= keys inside [Project] (what a
    // template has), and the two spellings can appear together — so both go
    // into one table keyed by N and come out in unit order, whichever form
    // each entry arrived in.
    struct RawUnit {
        QString fileName;
        bool compile = true;
        bool link = true;
        bool compileCpp = true;  // real default filled in by consider()
        bool overrideBuildCmd = false;
        QString buildCmd;
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
        if (unitsByIndex.contains(index)) {
            // The other spelling already claimed this index. Same file is
            // the benign case (both spellings of one unit); a DIFFERENT
            // file means one of the two silently vanished from the tree.
            const QString kept = unitsByIndex.value(index).fileName;
            if (absoluteFrom(project.projectDir, kept).toLower() != key) {
                warn(QStringLiteral(
                         "unit %1 is listed twice with different files "
                         "('%2' wins, '%3' is dropped)")
                         .arg(index).arg(kept, fileName));
            }
            return;
        }
        if (seenPaths.contains(key)) {
            warn(QStringLiteral("'%1' is listed more than once in the "
                                "project; only one entry is kept")
                     .arg(fileName));
            return;
        }
        seenPaths.insert(key);
        // No per-unit CompileCpp= yet → the project's flavour decides.
        unitsByIndex.insert(index, RawUnit{fileName, compile, link,
                                           project.isCpp, false, {}});
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
        const QString compileCpp = lookup(entries, section, "CompileCpp");
        const QString overrideBuildCmd =
            lookup(entries, section, "OverrideBuildCmd");
        const QString buildCmd = lookup(entries, section, "BuildCmd");
        const int index = section.mid(4).toInt();
        consider(index, fileName,
                 compile.isEmpty() || compile != QStringLiteral("0"),
                 link.isEmpty() || link != QStringLiteral("0"));
        // consider() skips duplicates/empties; the per-unit extras only
        // matter when the unit itself was taken.
        const auto it = unitsByIndex.find(index);
        if (it != unitsByIndex.end()) {
            if (!compileCpp.isEmpty()) {
                it->compileCpp = compileCpp != QStringLiteral("0");
            }
            it->overrideBuildCmd = overrideBuildCmd == QStringLiteral("1");
            it->buildCmd = buildCmd;
        }
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
                              it.value().link, it.value().compileCpp,
                              it.value().overrideBuildCmd, it.value().buildCmd});
    }

    if (project.units.isEmpty()) {
        return fail(QStringLiteral("the project lists no source files"));
    }
    if (!project.isValid()) {
        return fail(QStringLiteral("the project lists no compilable source files"));
    }
    // UnitCount= is what the IDE sized its unit table to; a mismatch means
    // the file was edited by hand (or by another tool) and some units went
    // missing along the way. Not fatal — the file still opens — but it must
    // not look like a clean load.
    if (project.unitCount > 0
        && project.unitCount != project.units.size()) {
        warn(QStringLiteral("UnitCount=%1 but %2 unit(s) were found")
                 .arg(project.unitCount).arg(project.units.size()));
    }
    return project;
}

std::optional<DevCppProject> loadDevProject(const QString& filePath,
                                            QString* error,
                                            QStringList* warnings)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot read %1").arg(filePath);
        }
        return std::nullopt;
    }
    const QByteArray content = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("error reading %1: %2")
                         .arg(filePath, file.errorString());
        }
        return std::nullopt;
    }
    file.close();
    return parseDevProject(filePath, content, error, warnings);
}

}  // namespace LightCpp::Project
