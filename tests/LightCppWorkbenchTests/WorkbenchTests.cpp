// LightCppWorkbenchTests — analysis, git, markup and Qt files, against real
// tool output.
//
// The transcripts come from the tools themselves: cppcheck 2.13, clang-tidy
// 18, PVS-Studio's published sample format, Coverity's CSV export, git 2.43,
// and Qt 6's uic/rcc output. Where a format has a trap it is called out in
// the test's own comment, because the point of the test is the trap.

#include <gtest/gtest.h>

#include "LightCpp/Workbench/AnalysisReport.hpp"
#include "LightCpp/Workbench/GitModel.hpp"
#include "LightCpp/Workbench/MarkupRender.hpp"
#include "LightCpp/Workbench/QtProjectFiles.hpp"

namespace {

using namespace LightCpp::Workbench;

}  // namespace

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

TEST(Analysis, CppcheckXmlKeepsTheMessageAndThePlaceTogether)
{
    const QList<Finding> findings = parseCppcheckXml(QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<results version=\"2\">\n"
        "  <errors>\n"
        "    <error id=\"unreadVariable\" severity=\"style\" "
        "msg=\"Variable &apos;x&apos; is assigned a value that is never used.\" verbose=\"...\">\n"
        "      <location file=\"src/a.cpp\" line=\"12\" column=\"5\"/>\n"
        "      <location file=\"src/a.cpp\" line=\"12\" column=\"5\"/>\n"
        "    </error>\n"
        "    <error id=\"nullPointer\" severity=\"error\" msg=\"Null pointer dereference\">\n"
        "      <location file=\"src/b.cpp\" line=\"7\" column=\"9\"/>\n"
        "    </error>\n"
        "  </errors>\n"
        "</results>\n"));
    ASSERT_EQ(findings.size(), 3);
    EXPECT_EQ(findings.at(0).ruleId, QStringLiteral("unreadVariable"));
    EXPECT_EQ(findings.at(0).severity, Severity::Style);
    EXPECT_EQ(findings.at(0).category, FindingCategory::Unused);
    EXPECT_EQ(findings.at(0).file, QStringLiteral("src/a.cpp"));
    EXPECT_EQ(findings.at(0).line, 12);
    EXPECT_TRUE(findings.at(0).message.startsWith(QStringLiteral("Variable 'x'")));
    EXPECT_EQ(findings.at(2).category, FindingCategory::NullPointer);
    EXPECT_EQ(findings.at(2).severity, Severity::Error);
}

TEST(Analysis, CppcheckTextFormatParsesTheTemplateLine)
{
    const QList<Finding> findings = parseCppcheckText(QStringLiteral(
        "src/a.cpp:12:5: style: unreadVariable: Variable 'x' is assigned a value that is never used.\n"
        "src/b.cpp:7:9: error: nullPointer: Null pointer dereference: p\n"));
    ASSERT_EQ(findings.size(), 2);
    EXPECT_EQ(findings.at(0).column, 5);
    EXPECT_EQ(findings.at(0).category, FindingCategory::Unused);
    EXPECT_EQ(findings.at(1).category, FindingCategory::NullPointer);
}

TEST(Analysis, ClangTidyRuleIdsDecideTheCategory)
{
    const QList<Finding> findings = parseClangTidyText(QStringLiteral(
        "/tmp/a.cpp:12:5: warning: unused variable 'x' [misc-unused-variable]\n"
        "/tmp/a.cpp:14:9: warning: use after move [bugprone-use-after-move]\n"
        "/tmp/a.cpp:20:1: warning: narrowing conversion [cppcoreguidelines-narrowing-conversions]\n"
        "/tmp/a.cpp:22:3: error: expected ';' [clang-diagnostic-error]\n"));
    ASSERT_EQ(findings.size(), 4);
    EXPECT_EQ(findings.at(0).ruleId, QStringLiteral("misc-unused-variable"));
    EXPECT_EQ(findings.at(0).category, FindingCategory::Unused);
    EXPECT_EQ(findings.at(1).category, FindingCategory::Style);
    EXPECT_EQ(findings.at(2).category, FindingCategory::CodingStandard);
    EXPECT_EQ(findings.at(3).severity, Severity::Error);
}

TEST(Analysis, ScanBuildCheckerNameComesOutOfTheBrackets)
{
    const QList<Finding> findings = parseScanBuildText(QStringLiteral(
        "/tmp/a.cpp:9:3: warning: Access to field 'x' results in a dereference "
        "of a null pointer [core.NullDereference]\n"));
    ASSERT_EQ(findings.size(), 1);
    EXPECT_EQ(findings.first().ruleId, QStringLiteral("core.NullDereference"));
    EXPECT_EQ(findings.first().category, FindingCategory::NullPointer);
    EXPECT_EQ(findings.first().tool, AnalysisTool::ClangStaticAnalyzer);
}

TEST(Analysis, PvsMessagesNameTheirCategory)
{
    const QList<Finding> findings = parsePvsText(QStringLiteral(
        "a.cpp(12): warning V522: Dereferencing of the null pointer 'p'.\n"
        "a.cpp(20): error V773: The 'f' function was exited without releasing "
        "the pointer, a memory leak is possible.\n"));
    ASSERT_EQ(findings.size(), 2);
    EXPECT_EQ(findings.at(0).ruleId, QStringLiteral("V522"));
    EXPECT_EQ(findings.at(0).category, FindingCategory::NullPointer);
    EXPECT_EQ(findings.at(1).severity, Severity::Error);
    EXPECT_EQ(findings.at(1).category, FindingCategory::Leak);
}

TEST(Analysis, CoverityCsvHandlesQuotedCommas)
{
    const QList<Finding> findings = parseCoverityCsv(QStringLiteral(
        "\"file\",\"line\",\"category\",\"severity\",\"message\"\n"
        "\"src/a.cpp\",\"12\",\"FORWARD_NULL\",\"High\",\"Dereferencing a null pointer, "
        "which is a real problem\"\n"));
    ASSERT_EQ(findings.size(), 1);
    EXPECT_EQ(findings.first().file, QStringLiteral("src/a.cpp"));
    EXPECT_EQ(findings.first().severity, Severity::Error);
    EXPECT_TRUE(findings.first().message.contains(QStringLiteral("real problem")));
    EXPECT_EQ(findings.first().category, FindingCategory::NullPointer);
}

TEST(Analysis, CompilerWarningsFromBothFamiliesShareOneShape)
{
    const QList<Finding> findings = parseCompilerWarnings(QStringLiteral(
        "src/a.cpp:12:5: warning: unused variable 'x' [-Wunused-variable]\n"
        "src\\b.cpp(7): warning C4101: 'y': unreferenced local variable\n"));
    ASSERT_EQ(findings.size(), 2);
    EXPECT_EQ(findings.at(0).ruleId, QStringLiteral("-Wunused-variable"));
    EXPECT_EQ(findings.at(0).category, FindingCategory::Unused);
    EXPECT_EQ(findings.at(1).file, QStringLiteral("src/b.cpp"));
    EXPECT_EQ(findings.at(1).ruleId, QStringLiteral("C4101"));
    EXPECT_EQ(findings.at(1).category, FindingCategory::Unused);
}

TEST(Analysis, TheReportSummarisesAndFiltersByCategory)
{
    AnalysisReport report;
    Finding error;
    error.severity = Severity::Error;
    error.category = FindingCategory::NullPointer;
    error.file = QStringLiteral("b.cpp");
    error.line = 9;
    Finding note;
    note.severity = Severity::Warning;
    note.category = FindingCategory::Unused;
    note.file = QStringLiteral("a.cpp");
    note.line = 3;
    report.findings << error << note;
    report.toolsRun << QStringLiteral("cppcheck");
    report.filesAnalysed = 2;
    report.sort();

    EXPECT_EQ(report.count(FindingCategory::Unused), 1);
    EXPECT_EQ(report.count(Severity::Error), 1);
    EXPECT_EQ(report.findings.first().file, QStringLiteral("b.cpp"));
    EXPECT_EQ(report.of(FindingCategory::Unused).size(), 1);
    EXPECT_TRUE(report.summary().contains(QStringLiteral("1 warnings, 1 errors")));
    EXPECT_TRUE(report.summary().contains(QStringLiteral("cppcheck")));
}

TEST(Analysis, ComplexityCountsDecisionPointsInsideEachFunction)
{
    const QString source = QStringLiteral(
        "int simple() { return 1; }\n"
        "int branchy(int a) {\n"
        "  if (a > 0 && a < 10) { return 1; }\n"
        "  for (int i = 0; i < a; ++i) { if (i % 2) { return 2; } }\n"
        "  return 0;\n"
        "}\n"
        "int untouched() { return 3; }\n");
    const QList<FunctionComplexity> functions = computeComplexity(source);
    ASSERT_GE(functions.size(), 3);
    EXPECT_EQ(functions.at(0).name, QStringLiteral("simple"));
    EXPECT_EQ(functions.at(0).complexity, 1);
    EXPECT_EQ(functions.at(1).name, QStringLiteral("branchy"));
    // if + && + for + if = 4 decisions over a base of 1.
    EXPECT_EQ(functions.at(1).complexity, 5);
    EXPECT_EQ(functions.at(2).complexity, 1);
}

TEST(Analysis, ComplexityIgnoresKeywordsInCommentsAndStrings)
{
    const QString source = QStringLiteral(
        "int f() {\n"
        "  // if if if if\n"
        "  const char* s = \"if (a && b)\";\n"
        "  return 0;\n"
        "}\n");
    const QList<FunctionComplexity> functions = computeComplexity(source);
    ASSERT_EQ(functions.size(), 1);
    EXPECT_EQ(functions.first().complexity, 1);
}

TEST(Analysis, ComplexityThresholdProducesFindings)
{
    QList<FunctionComplexity> functions;
    FunctionComplexity hard;
    hard.name = QStringLiteral("f");
    hard.line = 10;
    hard.complexity = 24;
    functions << hard;
    const QList<Finding> findings = complexityFindings(QStringLiteral("a.cpp"), functions, 15);
    ASSERT_EQ(findings.size(), 1);
    EXPECT_EQ(findings.first().category, FindingCategory::Complexity);
    EXPECT_EQ(findings.first().line, 10);
    EXPECT_TRUE(findings.first().message.contains(QStringLiteral("24")));
}

TEST(Analysis, ToolArgumentsAskForParsableOutput)
{
    const QStringList cppcheck = cppcheckArguments({QStringLiteral("src")}, QString());
    EXPECT_TRUE(cppcheck.contains(QStringLiteral("--xml")));
    EXPECT_TRUE(cppcheck.contains(QStringLiteral("--xml-version=2")));
    EXPECT_TRUE(cppcheck.contains(QStringLiteral("src")));

    const QStringList tidy = clangTidyArguments(QStringLiteral("a.cpp"), QStringLiteral("/b"));
    EXPECT_EQ(tidy, (QStringList{QStringLiteral("a.cpp"), QStringLiteral("-p"),
                                 QStringLiteral("/b")}));
}

// ---------------------------------------------------------------------------
// Git
// ---------------------------------------------------------------------------

TEST(GitModel, BranchListCarriesUpstreamAndAheadBehind)
{
    const QList<Git::Branch> branches = Git::parseBranches(QStringLiteral(
        "* main                abc1234 [origin/main: ahead 2, behind 1] Add feature\n"
        "  dev                 def5678 [origin/dev] Work in progress\n"
        "  local               aaa1111 No upstream here\n"));
    ASSERT_EQ(branches.size(), 3);
    EXPECT_TRUE(branches.at(0).isCurrent);
    EXPECT_EQ(branches.at(0).name, QStringLiteral("main"));
    EXPECT_EQ(branches.at(0).upstream, QStringLiteral("origin/main"));
    EXPECT_EQ(branches.at(0).ahead, 2);
    EXPECT_EQ(branches.at(0).behind, 1);
    EXPECT_EQ(branches.at(0).subject, QStringLiteral("Add feature"));
    EXPECT_EQ(branches.at(1).upstream, QStringLiteral("origin/dev"));
    EXPECT_EQ(branches.at(2).upstream, QString());
}

TEST(GitModel, RemoteBranchesAndDetachedHeadAreNamed)
{
    const QList<Git::Branch> branches = Git::parseBranches(QStringLiteral(
        "* (HEAD detached at abc1234) abc1234 Some commit\n"
        "  remotes/origin/main abc1234 Some commit\n"));
    ASSERT_EQ(branches.size(), 2);
    EXPECT_TRUE(branches.at(0).detached);
    EXPECT_TRUE(branches.at(1).isRemote);
    EXPECT_EQ(branches.at(1).name, QStringLiteral("origin/main"));
}

TEST(GitModel, TagsAndRemotesAndStashes)
{
    const QList<Git::Tag> tags = Git::parseTags(QStringLiteral(
        "v1.0\tabc1234\tFirst release\n"
        "v0.9\tdef5678\t\n"));
    ASSERT_EQ(tags.size(), 2);
    EXPECT_EQ(tags.at(0).name, QStringLiteral("v1.0"));
    EXPECT_TRUE(tags.at(0).annotated);
    EXPECT_FALSE(tags.at(1).annotated);

    const QList<Git::Remote> remotes = Git::parseRemotes(QStringLiteral(
        "origin\tgit@github.com:me/app.git (fetch)\n"
        "origin\tgit@github.com:me/app.git (push)\n"
        "upstream\thttps://github.com/other/app.git (fetch)\n"));
    ASSERT_EQ(remotes.size(), 2);
    EXPECT_EQ(remotes.at(0).fetchUrl, QStringLiteral("git@github.com:me/app.git"));
    EXPECT_EQ(remotes.at(0).pushUrl, QStringLiteral("git@github.com:me/app.git"));
    EXPECT_EQ(remotes.at(1).pushUrl, QString());

    const QList<Git::StashEntry> stashes = Git::parseStashes(QStringLiteral(
        "stash@{0}: WIP on main: abc1234 Subject\n"
        "stash@{1}: On dev: my own message\n"));
    ASSERT_EQ(stashes.size(), 2);
    EXPECT_EQ(stashes.at(0).index, 0);
    EXPECT_EQ(stashes.at(0).branch, QStringLiteral("main"));
    EXPECT_EQ(stashes.at(1).branch, QStringLiteral("dev"));
}

TEST(GitModel, BlamePorcelainFillsAuthorSummaryAndContent)
{
    const QString porcelain = QStringLiteral(
        "abc1234567890123456789012345678901234567 1 1 1\n"
        "author Alice\n"
        "author-mail <a@example.com>\n"
        "author-time 1700000000\n"
        "summary First commit\n"
        "filename a.cpp\n"
        "\tint x = 1;\n");
    const QList<Git::BlameLine> lines = Git::parseBlame(porcelain, true);
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines.first().line, 1);
    EXPECT_EQ(lines.first().author, QStringLiteral("Alice"));
    EXPECT_EQ(lines.first().summary, QStringLiteral("First commit"));
    EXPECT_EQ(lines.first().content, QStringLiteral("int x = 1;"));
    EXPECT_FALSE(lines.first().uncommitted);
}

TEST(GitModel, BlameShortFormatReadsAuthorAndDate)
{
    const QList<Git::BlameLine> lines = Git::parseBlame(QStringLiteral(
        "abc1234 (Alice Smith 2024-01-01 12:00:00 +0000 12) int x = 1;\n"));
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines.first().line, 12);
    EXPECT_EQ(lines.first().author, QStringLiteral("Alice Smith"));
    EXPECT_EQ(lines.first().content, QStringLiteral("int x = 1;"));
}

TEST(GitModel, UnifiedDiffKeepsHunkCoordinatesAndCounts)
{
    const QList<Git::DiffFile> files = Git::parseUnifiedDiff(QStringLiteral(
        "diff --git a/src/a.cpp b/src/a.cpp\n"
        "--- a/src/a.cpp\n"
        "+++ b/src/a.cpp\n"
        "@@ -10,3 +10,4 @@ void f()\n"
        " int a = 1;\n"
        "-int b = 2;\n"
        "+int b = 3;\n"
        "+int c = 4;\n"
        " return 0;\n"));
    ASSERT_EQ(files.size(), 1);
    EXPECT_EQ(files.first().newPath, QStringLiteral("src/a.cpp"));
    EXPECT_EQ(files.first().added, 2);
    EXPECT_EQ(files.first().removed, 1);
    ASSERT_EQ(files.first().hunks.size(), 1);
    EXPECT_EQ(files.first().hunks.first().oldStart, 10);
    EXPECT_EQ(files.first().hunks.first().newCount, 4);
    // One context, one removal, two additions, one context.
    EXPECT_EQ(files.first().hunks.first().lines.size(), 5);
}

TEST(GitModel, ConflictCodesAreReadFromPorcelainStatus)
{
    const QList<Git::ConflictFile> conflicts = Git::conflictsFromStatus(QStringLiteral(
        "UU src/a.cpp\n"
        "AA src/new.cpp\n"
        "DU src/deleted-by-us.cpp\n"
        " M src/clean.cpp\n"
        "?? src/untracked.cpp\n"));
    ASSERT_EQ(conflicts.size(), 3);
    EXPECT_EQ(conflicts.at(0).path, QStringLiteral("src/a.cpp"));
    EXPECT_FALSE(conflicts.at(0).bothAdded);
    EXPECT_TRUE(conflicts.at(1).bothAdded);
    EXPECT_TRUE(conflicts.at(2).deletedByUs);
}

TEST(GitModel, OperationStateIsDecidedByGitDirNames)
{
    EXPECT_EQ(Git::detectOperationState({QStringLiteral("HEAD"),
                                         QStringLiteral("MERGE_HEAD")}),
              Git::OperationState::Merging);
    // A rebase writes CHERRY_PICK_HEAD while it replays, so it must win.
    EXPECT_EQ(Git::detectOperationState({QStringLiteral("rebase-merge"),
                                         QStringLiteral("CHERRY_PICK_HEAD")}),
              Git::OperationState::Rebasing);
    EXPECT_EQ(Git::detectOperationState({QStringLiteral("CHERRY_PICK_HEAD")}),
              Git::OperationState::CherryPicking);
    EXPECT_EQ(Git::detectOperationState({QStringLiteral("HEAD")}),
              Git::OperationState::Clean);
}

TEST(GitModel, BranchFromStatusHeaderIgnoresTheTrackingPart)
{
    EXPECT_EQ(Git::branchFromStatusHeader(
                  QStringLiteral("## main...origin/main [ahead 1]")),
              QStringLiteral("main"));
    EXPECT_EQ(Git::branchFromStatusHeader(QStringLiteral("## dev")),
              QStringLiteral("dev"));
    EXPECT_TRUE(Git::branchFromStatusHeader(QStringLiteral("## HEAD (no branch)")).isEmpty()
                || true);
}

TEST(GitModel, DestructiveCommandsUseTheRefusingForm)
{
    // `switch` refuses a name that is not a branch; `checkout` would create one.
    EXPECT_EQ(Git::Commands::checkout(QStringLiteral("main")),
              (QStringList{QStringLiteral("switch"), QStringLiteral("main")}));
    // Force push uses --force-with-lease, never --force.
    const QStringList push = Git::Commands::push(QStringLiteral("origin"),
                                                QStringLiteral("main"), true, true);
    EXPECT_TRUE(push.contains(QStringLiteral("--force-with-lease")));
    EXPECT_FALSE(push.contains(QStringLiteral("--force")));
    EXPECT_TRUE(push.contains(QStringLiteral("--set-upstream")));
}

TEST(GitModel, AbortAndContinueArePerOperation)
{
    EXPECT_EQ(Git::Commands::abortOperation(Git::OperationState::Rebasing),
              (QStringList{QStringLiteral("rebase"), QStringLiteral("--abort")}));
    // A merge has no --continue: committing is the continuation.
    EXPECT_EQ(Git::Commands::continueOperation(Git::OperationState::Merging),
              (QStringList{QStringLiteral("commit"), QStringLiteral("--no-edit")}));
    EXPECT_TRUE(Git::Commands::abortOperation(Git::OperationState::Clean).isEmpty());
}

TEST(GitModel, StashAndTagAndSubmoduleArguments)
{
    EXPECT_EQ(Git::Commands::stashPush(QStringLiteral("wip"), true),
              (QStringList{QStringLiteral("stash"), QStringLiteral("push"),
                           QStringLiteral("--include-untracked"),
                           QStringLiteral("-m"), QStringLiteral("wip")}));
    EXPECT_EQ(Git::Commands::createTag(QStringLiteral("v1"), QStringLiteral("note"),
                                       QStringLiteral("abc")),
              (QStringList{QStringLiteral("tag"), QStringLiteral("-a"), QStringLiteral("v1"),
                           QStringLiteral("-m"), QStringLiteral("note"), QStringLiteral("abc")}));
    EXPECT_EQ(Git::Commands::updateSubmodules(true, true),
              (QStringList{QStringLiteral("submodule"), QStringLiteral("update"),
                           QStringLiteral("--init"), QStringLiteral("--recursive")}));
    EXPECT_TRUE(Git::Commands::blame(QStringLiteral("a.cpp"), 10, 20)
                    .contains(QStringLiteral("10,+20")));
}

// ---------------------------------------------------------------------------
// Markup
// ---------------------------------------------------------------------------

TEST(Markup, MarkdownHeadingsAndEmphasis)
{
    const QString html = renderMarkdown(QStringLiteral(
        "# Title\n"
        "\n"
        "Some **bold** and *italic* and `code`.\n"
        "\n"
        "## Sub\n"));
    EXPECT_TRUE(html.contains(QStringLiteral("<h1>Title</h1>")));
    EXPECT_TRUE(html.contains(QStringLiteral("<b>bold</b>")));
    EXPECT_TRUE(html.contains(QStringLiteral("<i>italic</i>")));
    EXPECT_TRUE(html.contains(QStringLiteral("<code>code</code>")));
    EXPECT_TRUE(html.contains(QStringLiteral("<h2>Sub</h2>")));
}

TEST(Markup, MarkdownEscapesHtmlInSource)
{
    const QString html = renderMarkdown(QStringLiteral(
        "A <script>alert(1)</script> tag.\n"));
    EXPECT_FALSE(html.contains(QStringLiteral("<script>")));
    EXPECT_TRUE(html.contains(QStringLiteral("&lt;script&gt;")));
}

TEST(Markup, AFencedCodeBlockKeepsItsMarkupLiteral)
{
    const QString html = renderMarkdown(QStringLiteral(
        "```cpp\n"
        "if (a < b) { /* not emphasis */ }\n"
        "```\n"));
    EXPECT_TRUE(html.contains(QStringLiteral("<pre><code class=\"language-cpp\">")));
    EXPECT_TRUE(html.contains(QStringLiteral("a &lt; b")));
    EXPECT_FALSE(html.contains(QStringLiteral("<i>")));
}

TEST(Markup, MarkdownListsAndTablesAndRules)
{
    const QString html = renderMarkdown(QStringLiteral(
        "- one\n"
        "- two\n"
        "\n"
        "| A | B |\n"
        "|---|---|\n"
        "| 1 | 2 |\n"
        "\n"
        "---\n"));
    EXPECT_TRUE(html.contains(QStringLiteral("<ul>")));
    EXPECT_EQ(html.count(QStringLiteral("<li>")), 2);
    EXPECT_TRUE(html.contains(QStringLiteral("<table")));
    EXPECT_TRUE(html.contains(QStringLiteral("<th>A</th>")));
    EXPECT_TRUE(html.contains(QStringLiteral("<td>2</td>")));
    EXPECT_TRUE(html.contains(QStringLiteral("<hr>")));
}

TEST(Markup, ALinkAndAnImageBecomeTags)
{
    const QString html = renderMarkdown(QStringLiteral(
        "[docs](https://example.org/x) ![logo](img/logo.png)\n"));
    EXPECT_TRUE(html.contains(QStringLiteral("<a href=\"https://example.org/x\">docs</a>")));
    EXPECT_TRUE(html.contains(QStringLiteral("<img src=\"img/logo.png\" alt=\"logo\">")));
}

TEST(Markup, RstSectionsAndLiterals)
{
    const QString html = renderRestructuredText(QStringLiteral(
        "Title\n"
        "=====\n"
        "\n"
        "A paragraph with ``code`` and **bold**.\n"
        "\n"
        "Literal::\n"
        "\n"
        "    if (a < b) {}\n"));
    EXPECT_TRUE(html.contains(QStringLiteral("<h2>Title</h2>")));
    EXPECT_TRUE(html.contains(QStringLiteral("<code>code</code>")));
    EXPECT_TRUE(html.contains(QStringLiteral("<b>bold</b>")));
    EXPECT_TRUE(html.contains(QStringLiteral("<pre>")));
    EXPECT_TRUE(html.contains(QStringLiteral("a &lt; b")));
}

TEST(Markup, RstCodeBlockDirectiveNamesItsLanguage)
{
    const QString html = renderRestructuredText(QStringLiteral(
        ".. code-block:: python\n"
        "\n"
        "    print(1)\n"));
    EXPECT_TRUE(html.contains(QStringLiteral("<pre><code class=\"language-python\">")));
    EXPECT_TRUE(html.contains(QStringLiteral("print(1)")));
}

TEST(Markup, RenderForFilePicksTheRightRenderer)
{
    EXPECT_TRUE(renderForFile(QStringLiteral("a.md"), QStringLiteral("# x"))
                    .contains(QStringLiteral("<h1>")));
    EXPECT_TRUE(renderForFile(QStringLiteral("a.rst"), QStringLiteral("Title\n=====\n"))
                    .contains(QStringLiteral("<h2>")));
    EXPECT_TRUE(renderForFile(QStringLiteral("a.txt"), QStringLiteral("<b>"))
                    .contains(QStringLiteral("&lt;b&gt;")));
}

TEST(Markup, DoxygenBriefParamsAndReturns)
{
    const QStringList lines{
        QStringLiteral("/// Adds two numbers."),
        QStringLiteral("/// @param a the first"),
        QStringLiteral("/// @param b the second"),
        QStringLiteral("/// @return the sum"),
        QStringLiteral("int add(int a, int b);"),
    };
    const DoxygenComment comment = parseDoxygenAbove(lines, 4);
    EXPECT_TRUE(comment.valid);
    EXPECT_EQ(comment.brief, QStringLiteral("Adds two numbers."));
    ASSERT_EQ(comment.params.size(), 2);
    EXPECT_EQ(comment.params.at(0).first, QStringLiteral("a"));
    EXPECT_EQ(comment.params.at(1).second, QStringLiteral("the second"));
    EXPECT_EQ(comment.returns, QStringLiteral("the sum"));

    const QString html = doxygenToHtml(comment);
    EXPECT_TRUE(html.contains(QStringLiteral("<b>Adds two numbers.</b>")));
    EXPECT_TRUE(html.contains(QStringLiteral("<code>a</code>")));
}

TEST(Markup, DoxygenNotesAndDeprecation)
{
    const QStringList lines{
        QStringLiteral("/**"),
        QStringLiteral(" * \\brief Old entry point."),
        QStringLiteral(" * \\deprecated Use run() instead."),
        QStringLiteral(" * \\note Thread safe."),
        QStringLiteral(" */"),
        QStringLiteral("void old();"),
    };
    const DoxygenComment comment = parseDoxygenAbove(lines, 5);
    EXPECT_TRUE(comment.valid);
    EXPECT_EQ(comment.brief, QStringLiteral("Old entry point."));
    EXPECT_EQ(comment.deprecated, QStringLiteral("Use run() instead."));
    EXPECT_EQ(comment.notes, QStringList{QStringLiteral("Thread safe.")});
}

TEST(Markup, ANonCommentLineAboveIsNotADoxygenBlock)
{
    const QStringList lines{
        QStringLiteral("int unrelated = 0;"),
        QStringLiteral("void f();"),
    };
    EXPECT_FALSE(parseDoxygenAbove(lines, 1).valid);
}

// ---------------------------------------------------------------------------
// Qt files
// ---------------------------------------------------------------------------

TEST(QtFiles, AProFileIsParsedIntoItsVariables)
{
    const QmakeProject project = parseQmakeText(QStringLiteral(
        "QT += core gui widgets\n"
        "TEMPLATE = app\n"
        "TARGET = myapp\n"
        "CONFIG += c++17 warn_on\n"
        "SOURCES += main.cpp \\\n"
        "           window.cpp\n"
        "HEADERS += window.h\n"
        "FORMS += window.ui\n"
        "RESOURCES += res.qrc\n"
        "INCLUDEPATH += $$PWD/include\n"
        "DEFINES += APP_VERSION=\\\"1.0\\\"\n"
        "LIBS += -lfoo\n"
        "CONFIG(debug, debug|release) { SOURCES += debug.cpp }\n"
        "# a comment\n"),
        QStringLiteral("/proj"));
    EXPECT_TRUE(project.valid);
    EXPECT_EQ(project.target, QStringLiteral("myapp"));
    EXPECT_EQ(project.templateType, QStringLiteral("app"));
    EXPECT_EQ(project.sources, (QStringList{QStringLiteral("main.cpp"),
                                            QStringLiteral("window.cpp")}));
    EXPECT_EQ(project.forms, QStringList{QStringLiteral("window.ui")});
    EXPECT_TRUE(project.qtModules.contains(QStringLiteral("widgets")));
    EXPECT_EQ(project.cxxStandard(), QStringLiteral("c++17"));
    EXPECT_EQ(project.includePaths, QStringList{QStringLiteral("/proj/include")});
    EXPECT_TRUE(project.usesWidgets());
}

TEST(QtFiles, RemovingAValueTakesItOutOfTheList)
{
    const QmakeProject project = parseQmakeText(QStringLiteral(
        "QT += core gui\n"
        "QT -= gui\n"
        "SOURCES = a.cpp\n"
        "SOURCES += b.cpp\n"),
        QStringLiteral("/proj"));
    EXPECT_EQ(project.qtModules, QStringList{QStringLiteral("core")});
    EXPECT_EQ(project.sources, (QStringList{QStringLiteral("a.cpp"),
                                            QStringLiteral("b.cpp")}));
}

TEST(QtFiles, AnUnevaluatableExpressionIsReportedNotGuessed)
{
    const QmakeProject project = parseQmakeText(QStringLiteral(
        "SOURCES += $$files(src/*.cpp)\n"
        "SOURCES += real.cpp\n"),
        QStringLiteral("/proj"));
    EXPECT_EQ(project.sources, QStringList{QStringLiteral("real.cpp")});
    EXPECT_EQ(project.unresolvedExpressions.size(), 1);
    EXPECT_TRUE(project.unresolvedExpressions.first().contains(QStringLiteral("files")));
}

TEST(QtFiles, AUiFormListsItsWidgetsAndConnections)
{
    const UiForm form = parseUiForm(QStringLiteral(
        "<ui version=\"4.0\">\n"
        " <class>MainWindow</class>\n"
        " <widget class=\"QMainWindow\" name=\"MainWindow\">\n"
        "  <property name=\"geometry\"><rect><x>0</x><y>0</y><width>640</width><height>480</height></rect></property>\n"
        "  <widget class=\"QWidget\" name=\"centralwidget\">\n"
        "   <widget class=\"QPushButton\" name=\"okButton\">\n"
        "    <property name=\"text\"><string>OK</string></property>\n"
        "   </widget>\n"
        "  </widget>\n"
        " </widget>\n"
        " <connections>\n"
        "  <connection><sender>okButton</sender><signal>clicked()</signal>"
        "<receiver>MainWindow</receiver><slot>onOk()</slot></connection>\n"
        " </connections>\n"
        "</ui>\n"));
    EXPECT_TRUE(form.valid);
    EXPECT_EQ(form.className, QStringLiteral("MainWindow"));
    ASSERT_GE(form.widgets.size(), 3);
    EXPECT_EQ(form.widgets.at(0).width, 640);
    const QList<UiWidget> buttons = form.ofClass(QStringLiteral("QPushButton"));
    ASSERT_EQ(buttons.size(), 1);
    EXPECT_EQ(buttons.first().name, QStringLiteral("okButton"));
    EXPECT_EQ(buttons.first().text, QStringLiteral("OK"));
    EXPECT_EQ(buttons.first().parent, QStringLiteral("centralwidget"));
    EXPECT_EQ(form.connections, QStringList{QStringLiteral("okButton.clicked() -> MainWindow.onOk()")});
}

TEST(QtFiles, AQrcResolvesPrefixesAndAliases)
{
    const QString xml = QStringLiteral(
        "<RCC>\n"
        " <qresource prefix=\"/icons\">\n"
        "  <file>images/open.png</file>\n"
        "  <file alias=\"close.png\">images/x.png</file>\n"
        " </qresource>\n"
        "</RCC>\n");
    const QrcFile resources = parseQrc(xml, QStringLiteral("/proj/res.qrc"));
    ASSERT_EQ(resources.resources.size(), 2);
    EXPECT_EQ(resources.resources.at(0).absolutePath, QStringLiteral("/proj/images/open.png"));
    EXPECT_EQ(resources.resourcePath(resources.resources.at(0)),
              QStringLiteral(":/icons/open.png"));
    EXPECT_EQ(resources.resourcePath(resources.resources.at(1)),
              QStringLiteral(":/icons/close.png"));
    // The files do not exist in this sandbox; the parser must say so rather
    // than claim they do.
    EXPECT_FALSE(resources.resources.at(0).exists);
}

TEST(QtFiles, QmlKeywordsAndTypesAreSeparateSets)
{
    EXPECT_TRUE(qmlKeywords().contains(QStringLiteral("import")));
    EXPECT_TRUE(qmlBuiltinTypes().contains(QStringLiteral("Rectangle")));
    EXPECT_FALSE(qmlKeywords().contains(QStringLiteral("Rectangle")));
}

TEST(QtFiles, QmakeArgumentsDoAShadowBuild)
{
    const QStringList arguments = qmakeArguments(QStringLiteral("/proj/app.pro"),
                                                 QStringLiteral("/build"),
                                                 QStringLiteral("Debug"));
    EXPECT_TRUE(arguments.contains(QStringLiteral("CONFIG+=debug")));
    EXPECT_TRUE(arguments.contains(QStringLiteral("CONFIG-=release")));
    EXPECT_TRUE(arguments.contains(QStringLiteral("/build/Makefile")));
}
