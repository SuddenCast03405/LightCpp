// AnalysisReport — implementation. See the header for the formats.

#include "LightCpp/Workbench/AnalysisReport.hpp"

#include <QDir>
#include <QDomDocument>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <algorithm>

namespace LightCpp::Workbench {
namespace {

QString normalize(QString path)
{
    path = path.trimmed();
    // A Windows path can reach this code on any host (MSVC output read on a
    // build server, a transcript in a test), so a backslash forms the
    // separator regardless of what QDir considers native.
    if (!path.startsWith(QLatin1Char('/')) && path.contains(QLatin1Char('\\'))) {
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        return path;
    }
    return QDir::fromNativeSeparators(path);
}

Severity severityFromCppcheck(const QString& name)
{
    if (name == QLatin1String("error"))   return Severity::Error;
    if (name == QLatin1String("style"))   return Severity::Style;
    if (name == QLatin1String("performance")) return Severity::Performance;
    if (name == QLatin1String("portability")) return Severity::Portability;
    return Severity::Warning;
}

/// A stable order for the list: errors first, then warnings, then the rest;
/// inside a severity, by file and line.
int severityRank(Severity severity)
{
    switch (severity) {
    case Severity::Error:         return 0;
    case Severity::Warning:       return 1;
    case Severity::Performance:   return 2;
    case Severity::Portability:   return 3;
    case Severity::Style:         return 4;
    case Severity::Note:          break;
    }
    return 5;
}

}  // namespace

QString toolName(AnalysisTool tool)
{
    switch (tool) {
    case AnalysisTool::Cppcheck:            return QStringLiteral("Cppcheck");
    case AnalysisTool::ClangTidy:           return QStringLiteral("clang-tidy");
    case AnalysisTool::ClangStaticAnalyzer: return QStringLiteral("Clang Static Analyzer");
    case AnalysisTool::PvsStudio:           return QStringLiteral("PVS-Studio");
    case AnalysisTool::Coverity:            return QStringLiteral("Coverity");
    case AnalysisTool::Compiler:            return QStringLiteral("Compiler");
    case AnalysisTool::Unknown:             break;
    }
    return QStringLiteral("Unknown");
}

QString toolId(AnalysisTool tool)
{
    switch (tool) {
    case AnalysisTool::Cppcheck:            return QStringLiteral("cppcheck");
    case AnalysisTool::ClangTidy:           return QStringLiteral("clang-tidy");
    case AnalysisTool::ClangStaticAnalyzer: return QStringLiteral("scan-build");
    case AnalysisTool::PvsStudio:           return QStringLiteral("pvs");
    case AnalysisTool::Coverity:            return QStringLiteral("coverity");
    case AnalysisTool::Compiler:            return QStringLiteral("compiler");
    case AnalysisTool::Unknown:             break;
    }
    return QStringLiteral("unknown");
}

QString severityName(Severity severity)
{
    switch (severity) {
    case Severity::Note:        return QStringLiteral("note");
    case Severity::Warning:     return QStringLiteral("warning");
    case Severity::Error:       return QStringLiteral("error");
    case Severity::Style:       return QStringLiteral("style");
    case Severity::Performance: return QStringLiteral("performance");
    case Severity::Portability: return QStringLiteral("portability");
    }
    return QStringLiteral("warning");
}

QString categoryName(FindingCategory category)
{
    switch (category) {
    case FindingCategory::Style:           return QStringLiteral("Style");
    case FindingCategory::Unused:          return QStringLiteral("Unused");
    case FindingCategory::NullPointer:     return QStringLiteral("Null pointer");
    case FindingCategory::Bounds:          return QStringLiteral("Bounds");
    case FindingCategory::Leak:            return QStringLiteral("Leak");
    case FindingCategory::Complexity:      return QStringLiteral("Complexity");
    case FindingCategory::CodingStandard:  return QStringLiteral("Coding standard");
    case FindingCategory::Security:        return QStringLiteral("Security");
    case FindingCategory::CompilerWarning: return QStringLiteral("Compiler warning");
    case FindingCategory::Performance:     return QStringLiteral("Performance");
    case FindingCategory::Other:           break;
    }
    return QStringLiteral("Other");
}

FindingCategory categoryOf(AnalysisTool tool, const QString& ruleId)
{
    const QString id = ruleId.toLower();
    if (id.isEmpty()) {
        return FindingCategory::Other;
    }

    // --- cppcheck ids -----------------------------------------------------
    if (tool == AnalysisTool::Cppcheck) {
        static const QStringList unused{
            QStringLiteral("unreadvariable"), QStringLiteral("unusedvariable"),
            QStringLiteral("unusedfunction"), QStringLiteral("unusedstructmember"),
            QStringLiteral("unusedlabel"), QStringLiteral("unusedprivatefunction"),
            QStringLiteral("unusedscopedobject")};
        static const QStringList nullPointer{
            QStringLiteral("nullpointer"), QStringLiteral("nullpointerarithmetic"),
            QStringLiteral("nullpointerdefaultarg"), QStringLiteral("nullpointerredundantcode"),
            QStringLiteral("zerodivision")};
        static const QStringList bounds{
            QStringLiteral("arrayindexoutofbounds"), QStringLiteral("bufferaccessoutofbounds"),
            QStringLiteral("arrayindexthencheck"), QStringLiteral("outofbounds")};
        static const QStringList leak{
            QStringLiteral("memleak"), QStringLiteral("resourceleak"),
            QStringLiteral("memleakondestructor"), QStringLiteral("leaknoerror"),
            QStringLiteral("leakuninit")};
        static const QStringList security{
            QStringLiteral("bufferaccessoutofbounds"),
            QStringLiteral("formatstring"), QStringLiteral("sprintfoverlappingdata"),
            QStringLiteral("unsigneddivision"), QStringLiteral("signconversion"),
            QStringLiteral("invalidscanf"), QStringLiteral("conststatement"),
            QStringLiteral("usessprintf")};
        static const QStringList style{
            QStringLiteral("unusedfunction"), QStringLiteral("variablehidesfunction"),
            QStringLiteral("redundantassignment"), QStringLiteral("duplicateexpression"),
            QStringLiteral("variablehidesvariablescope"),
            QStringLiteral("constparameter"), QStringLiteral("constvariable"),
            QStringLiteral("usestlalgorithm"), QStringLiteral("reademptycontainer"),
            QStringLiteral("knownconditiontruefalse"), QStringLiteral("clarifycondition"),
            QStringLiteral("unusedstructmember")};
        static const QStringList performance{
            QStringLiteral("postfixoperator"), QStringLiteral("passedbyvalue"),
            QStringLiteral("usestlalgorithm"), QStringLiteral("stlifindstr")};
        if (unused.contains(id))        return FindingCategory::Unused;
        if (nullPointer.contains(id))   return FindingCategory::NullPointer;
        if (bounds.contains(id))        return FindingCategory::Bounds;
        if (leak.contains(id))          return FindingCategory::Leak;
        if (performance.contains(id))   return FindingCategory::Performance;
        if (style.contains(id))         return FindingCategory::Style;
        // security after bounds/leak so the more specific bucket wins.
        if (security.contains(id))      return FindingCategory::Security;
        return FindingCategory::Other;
    }

    // --- clang-tidy rule prefixes ----------------------------------------
    if (tool == AnalysisTool::ClangTidy) {
        static const QStringList unused{
            QStringLiteral("misc-unused"), QStringLiteral("clang-diagnostic-unused"),
            QStringLiteral("bugprone-unused")};
        static const QStringList nullPointer{
            QStringLiteral("bugprone-null"), QStringLiteral("bugprone-dereference"),
            QStringLiteral("clang-analyzer-core.null")};
        static const QStringList bounds{
            QStringLiteral("bugprone-out-of-bounds"), QStringLiteral("clang-analyzer-security.array"),
            QStringLiteral("cppcoreguidelines-pro-bounds")};
        static const QStringList leak{
            QStringLiteral("clang-analyzer-cplusplus.newdelete"),
            QStringLiteral("clang-analyzer-unix.malloc"),
            QStringLiteral("bugprone-unhandled-self-assignment")};
        static const QStringList security{
            QStringLiteral("cert-"), QStringLiteral("clang-analyzer-security."),
            QStringLiteral("bugprone-easily-swappable"), QStringLiteral("bugprone-signal-handler"),
            QStringLiteral("bugprone-signed-char-misuse"), QStringLiteral("bugprone-string-constructor")};
        static const QStringList performance{
            QStringLiteral("performance-")};
        static const QStringList style{
            QStringLiteral("readability-"), QStringLiteral("modernize-"),
            QStringLiteral("google-"), QStringLiteral("llvm-"), QStringLiteral("misc-"),
            // "bugprone-" is advice about a construct that works but is easy
            // to get wrong; the more specific buckets above win first.
            QStringLiteral("bugprone-")};
        static const QStringList standard{
            QStringLiteral("cppcoreguidelines-"), QStringLiteral("hicpp-"),
            QStringLiteral("cert-")};
        for (const QString& prefix : unused) {
            if (id.startsWith(prefix)) return FindingCategory::Unused;
        }
        for (const QString& prefix : nullPointer) {
            if (id.startsWith(prefix)) return FindingCategory::NullPointer;
        }
        for (const QString& prefix : bounds) {
            if (id.startsWith(prefix)) return FindingCategory::Bounds;
        }
        for (const QString& prefix : leak) {
            if (id.startsWith(prefix)) return FindingCategory::Leak;
        }
        for (const QString& prefix : performance) {
            if (id.startsWith(prefix)) return FindingCategory::Performance;
        }
        for (const QString& prefix : security) {
            if (id.startsWith(prefix)) return FindingCategory::Security;
        }
        for (const QString& prefix : standard) {
            if (id.startsWith(prefix)) return FindingCategory::CodingStandard;
        }
        for (const QString& prefix : style) {
            if (id.startsWith(prefix)) return FindingCategory::Style;
        }
        return FindingCategory::Other;
    }

    if (tool == AnalysisTool::ClangStaticAnalyzer) {
        if (id.contains(QLatin1String("null")))    return FindingCategory::NullPointer;
        if (id.contains(QLatin1String("leak")))    return FindingCategory::Leak;
        if (id.contains(QLatin1String("array")))   return FindingCategory::Bounds;
        if (id.contains(QLatin1String("unused")))  return FindingCategory::Unused;
        return FindingCategory::Other;
    }

    if (tool == AnalysisTool::PvsStudio) {
        // PVS's Vnnn ids group by chapter: 5xx general, 6xx 64-bit,
        // 7xx memory, 8xx exceptions, 9xx, 10xx templates, 2xxx diagnostics.
        // The message is more reliable than the number for the specific
        // categories the plan names, so match on it too.
        if (id.contains(QLatin1String("v5")) || id.contains(QLatin1String("v6"))) {
            return FindingCategory::Other;
        }
        return FindingCategory::Other;
    }

    if (tool == AnalysisTool::Coverity) {
        if (id.contains(QLatin1String("null")))    return FindingCategory::NullPointer;
        if (id.contains(QLatin1String("overflow"))) return FindingCategory::Bounds;
        if (id.contains(QLatin1String("leak")) || id.contains(QLatin1String("resource"))) {
            return FindingCategory::Leak;
        }
        if (id.contains(QLatin1String("unused")))  return FindingCategory::Unused;
        if (id.contains(QLatin1String("security")) || id.contains(QLatin1String("injection"))) {
            return FindingCategory::Security;
        }
        return FindingCategory::Other;
    }

    if (tool == AnalysisTool::Compiler) {
        // MSVC's codes are numbers; the two that mean "unused" are worth
        // mapping because they are the ones the plan's §七 names.
        if (id == QLatin1String("c4101") || id == QLatin1String("c4189")) {
            return FindingCategory::Unused;
        }
        if (id.contains(QLatin1String("unused")))  return FindingCategory::Unused;
        if (id.contains(QLatin1String("null")))    return FindingCategory::NullPointer;
        return FindingCategory::CompilerWarning;
    }
    return FindingCategory::Other;
}

QString Finding::label() const
{
    QString text = message;
    if (!ruleId.isEmpty()) {
        text += QStringLiteral(" [%1/%2]").arg(toolId(tool), ruleId);
    }
    return text;
}

bool Finding::samePlace(const Finding& other) const
{
    return file == other.file && line == other.line && column == other.column;
}

int AnalysisReport::count(FindingCategory category) const
{
    int total = 0;
    for (const Finding& finding : findings) {
        if (finding.category == category) {
            ++total;
        }
    }
    return total;
}

int AnalysisReport::count(Severity severity) const
{
    int total = 0;
    for (const Finding& finding : findings) {
        if (finding.severity == severity) {
            ++total;
        }
    }
    return total;
}

QList<Finding> AnalysisReport::of(FindingCategory category) const
{
    QList<Finding> out;
    for (const Finding& finding : findings) {
        if (finding.category == category) {
            out.append(finding);
        }
    }
    return out;
}

QString AnalysisReport::summary() const
{
    if (findings.isEmpty()) {
        return QStringLiteral("no findings");
    }
    const int errors = count(Severity::Error);
    const int warnings = count(Severity::Warning);
    QString text = QStringLiteral("%1 warnings, %2 errors").arg(warnings).arg(errors);
    if (filesAnalysed > 0) {
        text += QStringLiteral(" in %1 files").arg(filesAnalysed);
    }
    if (!toolsRun.isEmpty()) {
        text += QStringLiteral(" (%1)").arg(toolsRun.join(QStringLiteral(", ")));
    }
    if (truncated) {
        text += QStringLiteral(" [truncated]");
    }
    return text;
}

void AnalysisReport::sort()
{
    std::stable_sort(findings.begin(), findings.end(),
                     [](const Finding& a, const Finding& b) {
                         const int ra = severityRank(a.severity);
                         const int rb = severityRank(b.severity);
                         if (ra != rb) {
                             return ra < rb;
                         }
                         if (a.file != b.file) {
                             return a.file < b.file;
                         }
                         return a.line < b.line;
                     });
}

// ---------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------

QList<Finding> parseCppcheckXml(const QString& xml)
{
    // The <error> element carries the message and severity; each of its
    // <location> children is one occurrence. Reading them in that order means
    // the message is in scope when a location arrives — the earlier version
    // looked at "the last finding with an empty message", which attached a
    // second location to the *previous* error's duplicate.
    QList<Finding> findings;
    QXmlStreamReader reader(xml);
    QString currentFile;
    bool inError = false;
    Severity severity = Severity::Warning;
    QString ruleId;
    QString message;

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QStringView name = reader.name();
            if (name == QLatin1String("error")) {
                const QXmlStreamAttributes attributes = reader.attributes();
                inError = true;
                severity = severityFromCppcheck(attributes.value(QLatin1String("severity")).toString());
                ruleId = attributes.value(QLatin1String("id")).toString();
                message = attributes.value(QLatin1String("msg")).toString();
                continue;
            }
            if (name == QLatin1String("location")) {
                const QXmlStreamAttributes attributes = reader.attributes();
                Finding finding;
                finding.tool = AnalysisTool::Cppcheck;
                finding.severity = severity;
                finding.ruleId = ruleId;
                finding.category = categoryOf(AnalysisTool::Cppcheck, ruleId);
                finding.message = message;
                finding.file = normalize(attributes.value(QLatin1String("file")).toString());
                if (finding.file.isEmpty()) {
                    finding.file = currentFile;
                }
                finding.line = attributes.value(QLatin1String("line")).toInt();
                finding.column = attributes.value(QLatin1String("column")).toInt();
                if (!finding.file.isEmpty()) {
                    findings.append(finding);
                }
                continue;
            }
            if (name == QLatin1String("file")) {
                currentFile = normalize(reader.attributes().value(QLatin1String("name")).toString());
                continue;
            }
            continue;
        }
        if (reader.isEndElement()) {
            const QStringView name = reader.name();
            if (name == QLatin1String("error")) {
                inError = false;
                // An error with no <location> at all still deserves a row:
                // cppcheck emits those for "too many #ifdef configurations"
                // and similar global complaints.
                bool placed = false;
                for (qsizetype i = findings.size() - 1; i >= 0; --i) {
                    if (findings.at(i).ruleId == ruleId && findings.at(i).message == message) {
                        placed = true;
                        break;
                    }
                    if (findings.size() - i > 64) {
                        break;
                    }
                }
                if (!placed && !message.isEmpty()) {
                    Finding finding;
                    finding.tool = AnalysisTool::Cppcheck;
                    finding.severity = severity;
                    finding.ruleId = ruleId;
                    finding.category = categoryOf(AnalysisTool::Cppcheck, ruleId);
                    finding.message = message;
                    finding.file = currentFile;
                    findings.append(finding);
                }
            }
            continue;
        }
    }
    Q_UNUSED(inError);
    return findings;
}

QList<Finding> parseCppcheckText(const QString& text)
{
    // "{file}:{line}:{column}: {severity}: {id}: {message}"
    static const QRegularExpression pattern(
        QStringLiteral("^(.+?):(\\d+):(\\d+):\\s*(error|warning|style|performance|portability|information):\\s*([^:]+):\\s*(.+)$"));
    QList<Finding> findings;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const auto match = pattern.match(raw.trimmed());
        if (!match.hasMatch()) {
            continue;
        }
        Finding finding;
        finding.tool = AnalysisTool::Cppcheck;
        finding.file = normalize(match.captured(1));
        finding.line = match.captured(2).toInt();
        finding.column = match.captured(3).toInt();
        finding.severity = severityFromCppcheck(match.captured(4));
        finding.ruleId = match.captured(5).trimmed();
        finding.category = categoryOf(AnalysisTool::Cppcheck, finding.ruleId);
        finding.message = match.captured(6).trimmed();
        findings.append(finding);
    }
    return findings;
}

QList<Finding> parseClangTidyText(const QString& text)
{
    // "/tmp/a.cpp:12:5: warning: unused variable 'x' [misc-unused-variable]"
    static const QRegularExpression pattern(
        QStringLiteral("^(.+?):(\\d+):(\\d+):\\s*(warning|error|note):\\s*(.+?)(?:\\s+\\[([^\\]]+)\\])?$"));
    QList<Finding> findings;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const auto match = pattern.match(raw.trimmed());
        if (!match.hasMatch()) {
            continue;
        }
        Finding finding;
        finding.tool = AnalysisTool::ClangTidy;
        finding.file = normalize(match.captured(1));
        finding.line = match.captured(2).toInt();
        finding.column = match.captured(3).toInt();
        const QString level = match.captured(4);
        finding.severity = level == QLatin1String("error") ? Severity::Error
                           : level == QLatin1String("note")  ? Severity::Note
                                                            : Severity::Warning;
        finding.message = match.captured(5).trimmed();
        finding.ruleId = match.captured(6);
        finding.category = categoryOf(AnalysisTool::ClangTidy, finding.ruleId);
        findings.append(finding);
    }
    return findings;
}

QList<Finding> parseScanBuildText(const QString& text)
{
    static const QRegularExpression pattern(
        QStringLiteral("^(.+?):(\\d+):(\\d+):\\s*(warning|error):\\s*(.+)$"));
    QList<Finding> findings;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const auto match = pattern.match(raw.trimmed());
        if (!match.hasMatch()) {
            continue;
        }
        Finding finding;
        finding.tool = AnalysisTool::ClangStaticAnalyzer;
        finding.file = normalize(match.captured(1));
        finding.line = match.captured(2).toInt();
        finding.column = match.captured(3).toInt();
        finding.severity = match.captured(4) == QLatin1String("error")
                               ? Severity::Error : Severity::Warning;
        finding.message = match.captured(5).trimmed();
        // scan-build's checker name is in the message: "… [core.NullDereference]"
        static const QRegularExpression checker(
            QStringLiteral("\\[([A-Za-z0-9_.]+)\\]\\s*$"));
        if (const auto named = checker.match(finding.message); named.hasMatch()) {
            finding.ruleId = named.captured(1);
            finding.message = finding.message.left(named.capturedStart()).trimmed();
        }
        finding.category = categoryOf(AnalysisTool::ClangStaticAnalyzer, finding.ruleId);
        findings.append(finding);
    }
    return findings;
}

QList<Finding> parsePvsText(const QString& text)
{
    // "a.cpp(12): warning V522: Dereferencing of the null pointer"
    static const QRegularExpression pattern(
        QStringLiteral("^(.+?)\\((\\d+)\\):\\s*(error|warning|note)\\s+(V\\d+):\\s*(.+)$"),
        QRegularExpression::CaseInsensitiveOption);
    QList<Finding> findings;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const auto match = pattern.match(raw.trimmed());
        if (!match.hasMatch()) {
            continue;
        }
        Finding finding;
        finding.tool = AnalysisTool::PvsStudio;
        finding.file = normalize(match.captured(1));
        finding.line = match.captured(2).toInt();
        const QString level = match.captured(3).toLower();
        finding.severity = level == QLatin1String("error") ? Severity::Error
                           : level == QLatin1String("note")  ? Severity::Note
                                                            : Severity::Warning;
        finding.ruleId = match.captured(4).toUpper();
        finding.category = categoryOf(AnalysisTool::PvsStudio, finding.ruleId);
        finding.message = match.captured(5).trimmed();
        // PVS names the category in the message for the ones the plan lists.
        if (finding.message.contains(QLatin1String("null pointer"), Qt::CaseInsensitive)) {
            finding.category = FindingCategory::NullPointer;
        } else if (finding.message.contains(QLatin1String("memory leak"), Qt::CaseInsensitive)) {
            finding.category = FindingCategory::Leak;
        } else if (finding.message.contains(QLatin1String("out of bounds"), Qt::CaseInsensitive)
                   || finding.message.contains(QLatin1String("overflow"), Qt::CaseInsensitive)) {
            finding.category = FindingCategory::Bounds;
        }
        findings.append(finding);
    }
    return findings;
}

QList<Finding> parseCoverityCsv(const QString& text)
{
    // "file","line","category","severity","message"
    QList<Finding> findings;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1String("\"file\""))) {
            continue;
        }
        QStringList fields;
        QString current;
        bool inQuotes = false;
        for (int i = 0; i < line.size(); ++i) {
            const QChar ch = line.at(i);
            if (ch == QLatin1Char('"')) {
                if (inQuotes && i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) {
                    current.append(QLatin1Char('"'));
                    ++i;
                } else {
                    inQuotes = !inQuotes;
                }
                continue;
            }
            if (ch == QLatin1Char(',') && !inQuotes) {
                fields << current;
                current.clear();
                continue;
            }
            current.append(ch);
        }
        fields << current;
        if (fields.size() < 5) {
            continue;
        }
        Finding finding;
        finding.tool = AnalysisTool::Coverity;
        finding.file = normalize(fields.at(0));
        finding.line = fields.at(1).toInt();
        finding.ruleId = fields.at(2);
        const QString severity = fields.at(3).toLower();
        finding.severity = severity.contains(QLatin1String("high"))
                               || severity.contains(QLatin1String("error"))
                               ? Severity::Error : Severity::Warning;
        finding.message = fields.at(4);
        finding.category = categoryOf(AnalysisTool::Coverity, finding.ruleId);
        findings.append(finding);
    }
    return findings;
}

QList<Finding> parseCompilerWarnings(const QString& text)
{
    // gcc/clang: "file:12:5: warning: msg [-Wunused-variable]"
    // msvc:      "file(12): warning C4101: msg"
    static const QRegularExpression gcc(
        QStringLiteral("^(.+?):(\\d+):(\\d+):\\s*(warning|error|note):\\s*(.+)$"));
    static const QRegularExpression msvc(
        QStringLiteral("^(.+?)\\((\\d+)\\):\\s*(warning|error)\\s+(C\\d+):\\s*(.+)$"));
    QList<Finding> findings;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            continue;
        }
        Finding finding;
        finding.tool = AnalysisTool::Compiler;
        QString level;
        if (const auto match = gcc.match(line); match.hasMatch()) {
            finding.file = normalize(match.captured(1));
            finding.line = match.captured(2).toInt();
            finding.column = match.captured(3).toInt();
            level = match.captured(4);
            finding.message = match.captured(5).trimmed();
            static const QRegularExpression flag(
                QStringLiteral("\\[(-W[A-Za-z0-9=+_-]+)\\]\\s*$"));
            if (const auto named = flag.match(finding.message); named.hasMatch()) {
                finding.ruleId = named.captured(1);
                finding.message = finding.message.left(named.capturedStart()).trimmed();
            }
        } else if (const auto match = msvc.match(line); match.hasMatch()) {
            finding.file = normalize(match.captured(1));
            finding.line = match.captured(2).toInt();
            level = match.captured(3);
            finding.ruleId = match.captured(4);
            finding.message = match.captured(5).trimmed();
        } else {
            continue;
        }
        finding.severity = level == QLatin1String("error") ? Severity::Error
                           : level == QLatin1String("note")  ? Severity::Note
                                                            : Severity::Warning;
        finding.category = categoryOf(AnalysisTool::Compiler, finding.ruleId);
        findings.append(finding);
    }
    return findings;
}

// ---------------------------------------------------------------------------
// Tool invocations
// ---------------------------------------------------------------------------

QStringList cppcheckArguments(const QStringList& paths,
                              const QString& compileCommandsDir)
{
    QStringList arguments;
    arguments << QStringLiteral("--enable=warning,style,performance,portability")
              << QStringLiteral("--inconclusive")
              << QStringLiteral("--inline-suppr")
              << QStringLiteral("--xml")
              << QStringLiteral("--xml-version=2")
              << QStringLiteral("--output-file=-")
              << QStringLiteral("--suppress=missingIncludeSystem")
              << QStringLiteral("--quiet");
    if (!compileCommandsDir.isEmpty()) {
        // The project's own build flags; without them cppcheck guesses and
        // reports headers it could not resolve as findings.
        arguments << QStringLiteral("--project=%1/compile_commands.json")
                         .arg(compileCommandsDir);
    } else {
        for (const QString& path : paths) {
            arguments << path;
        }
    }
    return arguments;
}

QStringList clangTidyArguments(const QString& file, const QString& buildDirectory)
{
    QStringList arguments;
    arguments << file;
    if (!buildDirectory.isEmpty()) {
        arguments << QStringLiteral("-p") << buildDirectory;
    }
    return arguments;
}

QStringList scanBuildArguments(const QString& buildCommand)
{
    // `scan-build --status-bugs <build command>` — the exit code becomes
    // non-zero when a bug was found, which is what CI needs.
    return {QStringLiteral("--status-bugs"), QStringLiteral("--"),
            buildCommand};
}

// ---------------------------------------------------------------------------
// Complexity
// ---------------------------------------------------------------------------

QString FunctionComplexity::verdict() const
{
    if (complexity <= 10) {
        return QStringLiteral("simple");
    }
    if (complexity <= 20) {
        return QStringLiteral("moderate");
    }
    return QStringLiteral("hard to test");
}

namespace {

/// Remove comments and string/char literals so their contents cannot be
/// mistaken for keywords. Returns the cleaned text with a parallel map from
/// cleaned offsets to original line numbers.
struct CleanedSource {
    QString text;
    QList<int> lineOfOffset;
};

CleanedSource cleanSource(const QString& source)
{
    CleanedSource cleaned;
    cleaned.text.reserve(source.size());
    int line = 1;
    bool inLineComment = false;
    bool inBlockComment = false;
    bool inString = false;
    bool inChar = false;
    for (int i = 0; i < source.size(); ++i) {
        const QChar ch = source.at(i);
        const QChar next = (i + 1 < source.size()) ? source.at(i + 1) : QChar();
        if (ch == QLatin1Char('\n')) {
            ++line;
            inLineComment = false;
        }
        if (inLineComment) {
            continue;
        }
        if (inBlockComment) {
            if (ch == QLatin1Char('*') && next == QLatin1Char('/')) {
                inBlockComment = false;
                ++i;
            }
            continue;
        }
        if (inString || inChar) {
            if (ch == QLatin1Char('\\') && i + 1 < source.size()) {
                if (source.at(i + 1) == QLatin1Char('\n')) {
                    ++line;
                }
                ++i;
                continue;
            }
            if ((inString && ch == QLatin1Char('"'))
                || (inChar && ch == QLatin1Char('\''))) {
                inString = false;
                inChar = false;
            }
            continue;
        }
        if (ch == QLatin1Char('/') && next == QLatin1Char('/')) {
            inLineComment = true;
            ++i;
            continue;
        }
        if (ch == QLatin1Char('/') && next == QLatin1Char('*')) {
            inBlockComment = true;
            ++i;
            continue;
        }
        if (ch == QLatin1Char('"')) {
            inString = true;
            continue;
        }
        if (ch == QLatin1Char('\'')) {
            inChar = true;
            continue;
        }
        cleaned.text.append(ch);
        cleaned.lineOfOffset.append(line);
    }
    return cleaned;
}

bool isIdentifierChar(QChar ch)
{
    return ch.isLetterOrNumber() || ch == QLatin1Char('_');
}

/// The function name that precedes the opening paren at `parenIndex`, or
/// empty when the paren belongs to a call/control statement.
QString functionNameBefore(const QString& text, int parenIndex)
{
    int end = parenIndex;
    while (end > 0 && text.at(end - 1).isSpace()) {
        --end;
    }
    int start = end;
    while (start > 0 && (isIdentifierChar(text.at(start - 1))
                         || text.at(start - 1) == QLatin1Char('~'))) {
        --start;
    }
    if (start == end) {
        return {};
    }
    const QString name = text.mid(start, end - start);
    static const QStringList keywords{
        QStringLiteral("if"), QStringLiteral("for"), QStringLiteral("while"),
        QStringLiteral("switch"), QStringLiteral("catch"), QStringLiteral("return"),
        QStringLiteral("sizeof"), QStringLiteral("decltype"), QStringLiteral("alignof"),
        QStringLiteral("noexcept"), QStringLiteral("static_assert")};
    if (keywords.contains(name)) {
        return {};
    }
    return name;
}

}  // namespace

QList<FunctionComplexity> computeComplexity(const QString& source)
{
    const CleanedSource cleaned = cleanSource(source);
    const QString& text = cleaned.text;

    QList<FunctionComplexity> functions;
    // A function body is a `{` that follows a `)` whose paren had a name
    // before it, at brace depth 0 (or inside a class, which is depth 1).
    int depth = 0;
    int candidateParen = -1;
    QString candidateName;

    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('(')) {
            const QString name = functionNameBefore(text, i);
            if (!name.isEmpty()) {
                candidateName = name;
                candidateParen = i;
            }
            continue;
        }
        if (ch == QLatin1Char('{')) {
            ++depth;
            if (candidateParen >= 0 && depth <= 2 && !candidateName.isEmpty()) {
                // Find the matching close brace and measure the body.
                int inner = depth;
                int end = i + 1;
                int decisions = 0;
                bool inWord = false;
                QString word;
                int nestedFunctionDepth = -1;
                for (; end < text.size(); ++end) {
                    const QChar c = text.at(end);
                    if (c == QLatin1Char('{')) {
                        ++inner;
                        continue;
                    }
                    if (c == QLatin1Char('}')) {
                        --inner;
                        if (inner < depth) {
                            break;
                        }
                        continue;
                    }
                    if (isIdentifierChar(c)) {
                        if (!inWord) {
                            inWord = true;
                            word.clear();
                        }
                        word.append(c);
                        continue;
                    }
                    if (inWord) {
                        inWord = false;
                        if (word == QLatin1String("if") || word == QLatin1String("for")
                            || word == QLatin1String("while") || word == QLatin1String("case")
                            || word == QLatin1String("catch")) {
                            ++decisions;
                        }
                        word.clear();
                    }
                    if (c == QLatin1Char('&') || c == QLatin1Char('|')) {
                        if (end + 1 < text.size() && text.at(end + 1) == c) {
                            ++decisions;  // && or ||
                            ++end;
                            continue;
                        }
                    }
                    if (c == QLatin1Char('?')) {
                        ++decisions;
                    }
                }
                Q_UNUSED(nestedFunctionDepth);
                FunctionComplexity info;
                info.name = candidateName;
                info.line = cleaned.lineOfOffset.value(i, 1);
                info.complexity = 1 + decisions;
                info.lines = cleaned.lineOfOffset.value(end, info.line) - info.line + 1;
                functions.append(info);
                i = end;      // continue after the body
                depth = inner; // == depth - 1 after the loop decrement
                candidateParen = -1;
                candidateName.clear();
                continue;
            }
            continue;
        }
        if (ch == QLatin1Char('}')) {
            if (depth > 0) {
                --depth;
            }
            continue;
        }
        if (ch == QLatin1Char(';') || ch == QLatin1Char('=')) {
            candidateParen = -1;
        }
    }
    return functions;
}

QList<Finding> complexityFindings(const QString& file,
                                  const QList<FunctionComplexity>& functions,
                                  int threshold)
{
    QList<Finding> findings;
    for (const FunctionComplexity& function : functions) {
        if (function.complexity < threshold) {
            continue;
        }
        Finding finding;
        finding.tool = AnalysisTool::Unknown;
        finding.severity = function.complexity >= threshold * 2 ? Severity::Warning
                                                                : Severity::Note;
        finding.category = FindingCategory::Complexity;
        finding.ruleId = QStringLiteral("cyclomatic");
        finding.file = file;
        finding.line = function.line;
        finding.message = QStringLiteral("%1 has cyclomatic complexity %2 (%3)")
                              .arg(function.name)
                              .arg(function.complexity)
                              .arg(function.verdict());
        findings.append(finding);
    }
    return findings;
}

}  // namespace LightCpp::Workbench
