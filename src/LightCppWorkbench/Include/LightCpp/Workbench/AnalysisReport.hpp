// AnalysisReport — static analysis output from six tools, in one shape.
//
// lightcpp.1.md §七 lists thirteen features that are really four questions:
//
//   * which tool found something          (AnalysisTool)
//   * where and how bad                   (Finding)
//   * which *kind* of problem it is       (Finding::category)
//   * how complex is the code             (FunctionComplexity)
//
// The category is what makes "unused variables", "null pointer", "bounds",
// "leak", "style", "security" and "coding standard" usable: the tools report
// hundreds of rule ids (`cppcheck` "unreadVariable", clang-tidy
// "bugprone-use-after-move", PVS "V522", Coverity "FORWARD_NULL") and no two
// of them agree on a taxonomy. `categoryOf()` maps each tool's own ids onto
// one enum, so the panel can filter by the thing the plan asked for.
//
// Every parser takes the tool's *own* output format, because that is what the
// IDE receives:
//
//   cppcheck   --xml --xml-version=2          (XML, one <error> per finding)
//              --template='{file}:{line}:{column}: {severity}: {id}: {message}'
//   clang-tidy `file:line:col: warning: msg [rule]`
//   scan-build `file:line:col: warning: msg` (same as clang, plus a summary)
//   PVS        `file(12): warning V522: msg`
//   Coverity   CSV `"file","line","category","severity","message"`
//   compiler   GCC/Clang/MSVC — the same text `CompilerDiagnostics` reads,
//              kept here as well so one list can hold every source.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench {

enum class AnalysisTool {
    Unknown,
    Cppcheck,
    ClangTidy,
    ClangStaticAnalyzer,  ///< scan-build
    PvsStudio,
    Coverity,
    Compiler,             ///< gcc / clang / cl.exe warnings
};

[[nodiscard]] QString toolName(AnalysisTool tool);
[[nodiscard]] QString toolId(AnalysisTool tool);

enum class Severity { Note, Warning, Error, Style, Performance, Portability };

[[nodiscard]] QString severityName(Severity severity);

/// The plan's own taxonomy (§七).
enum class FindingCategory {
    Other,
    Style,
    Unused,
    NullPointer,
    Bounds,
    Leak,
    Complexity,
    CodingStandard,
    Security,
    CompilerWarning,
    Performance,
};

[[nodiscard]] QString categoryName(FindingCategory category);
/// The category a tool's own rule id belongs to, or Other.
[[nodiscard]] FindingCategory categoryOf(AnalysisTool tool, const QString& ruleId);

struct Finding {
    AnalysisTool tool = AnalysisTool::Unknown;
    Severity severity = Severity::Warning;
    FindingCategory category = FindingCategory::Other;
    QString ruleId;     ///< "unreadVariable" / "bugprone-..." / "V522"
    QString message;
    QString file;
    int line = 0;
    int column = 0;

    /// "unused variable: x [cppcheck/unreadVariable]"
    [[nodiscard]] QString label() const;
    /// True when the two findings are the same report (for de-duplication
    /// when two tools flag one line).
    [[nodiscard]] bool samePlace(const Finding& other) const;
};

/// An analysis run: the findings plus what produced them.
struct AnalysisReport {
    QList<Finding> findings;
    QStringList toolsRun;
    int filesAnalysed = 0;
    bool truncated = false;   ///< the tool hit its output limit

    [[nodiscard]] int count(FindingCategory category) const;
    [[nodiscard]] int count(Severity severity) const;
    /// Findings of `category`, worst severity first then by file and line.
    [[nodiscard]] QList<Finding> of(FindingCategory category) const;
    /// "12 warnings, 3 errors in 48 files (cppcheck, clang-tidy)"
    [[nodiscard]] QString summary() const;
    void sort();
};

/// Parse `cppcheck --xml --xml-version=2` output.
[[nodiscard]] QList<Finding> parseCppcheckXml(const QString& xml);
/// Parse cppcheck's `--template` line format.
[[nodiscard]] QList<Finding> parseCppcheckText(const QString& text);
/// Parse clang-tidy's diagnostics (`[rule]` suffix is the id).
[[nodiscard]] QList<Finding> parseClangTidyText(const QString& text);
/// Parse scan-build's console report.
[[nodiscard]] QList<Finding> parseScanBuildText(const QString& text);
/// Parse PVS-Studio's `file(line): level Vnnn: message`.
[[nodiscard]] QList<Finding> parsePvsText(const QString& text);
/// Parse Coverity's CSV export.
[[nodiscard]] QList<Finding> parseCoverityCsv(const QString& text);
/// Parse gcc/clang/msvc warnings into the same list.
[[nodiscard]] QList<Finding> parseCompilerWarnings(const QString& text);

/// The arguments each tool needs to produce parsable output.
[[nodiscard]] QStringList cppcheckArguments(const QStringList& paths,
                                            const QString& compileCommandsDir);
[[nodiscard]] QStringList clangTidyArguments(const QString& file,
                                             const QString& buildDirectory);
[[nodiscard]] QStringList scanBuildArguments(const QString& buildCommand);

// ---------------------------------------------------------------------------
// Complexity
// ---------------------------------------------------------------------------

/// Cyclomatic complexity of one function: 1 + decision points.
struct FunctionComplexity {
    QString name;
    int line = 0;
    int complexity = 1;
    int lines = 0;

    /// The usual reading of the number: 1-10 simple, 11-20 moderate,
    /// above 20 worth splitting. The wording is deliberate — a threshold is
    /// a prompt to look, not a verdict.
    [[nodiscard]] QString verdict() const;
};

/// Per-function complexity of one C/C++ source. Heuristic but honest: it
/// counts `if`/`for`/`while`/`case`/`&&`/`||`/`?:` inside each function body,
/// which is what -cyclomatic-complexity in most tools counts, and it says so
/// instead of pretending to be a compiler.
[[nodiscard]] QList<FunctionComplexity> computeComplexity(const QString& source);

/// Findings for every function above `threshold` (category Complexity).
[[nodiscard]] QList<Finding> complexityFindings(const QString& file,
                                                const QList<FunctionComplexity>& functions,
                                                int threshold = 15);

}  // namespace LightCpp::Workbench
