// WorkbenchExtraTests — the modules added for the remaining §三 / §五 / §六 /
// §八 / §九 / §十一 / §十二 items.
//
// Each suite is written against a real transcript of the tool it parses (a
// clangd answer, a git conflict file, a vcpkg list). The assertions name the
// failure mode the code was written to avoid, because that is the part a
// future change is most likely to break silently.

#include <gtest/gtest.h>

#include "LightCpp/Workbench/BuildOptions.hpp"
#include "LightCpp/Workbench/DebugSymbols.hpp"
#include "LightCpp/Workbench/DocsCatalog.hpp"
#include "LightCpp/Workbench/EditorTools.hpp"
#include "LightCpp/Workbench/ExternalTools.hpp"
#include "LightCpp/Workbench/GitExtra.hpp"
#include "LightCpp/Workbench/GitModel.hpp"
#include "LightCpp/Workbench/LspModel.hpp"
#include "LightCpp/Workbench/PackageManager.hpp"
#include "LightCpp/Workbench/QtPreview.hpp"
#include "LightCpp/Workbench/QtProjectFiles.hpp"
#include "LightCpp/Workbench/ShellIntegration.hpp"
#include "LightCpp/Workbench/TerminalEmulator.hpp"
#include "LightCpp/Workbench/Viewers.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

namespace {

using namespace LightCpp::Workbench;

// `Qt` is both the global namespace and ours, so the preview tests spell it
// out; this alias keeps the rest of those tests readable.
namespace QtNs = LightCpp::Workbench::Qt;

QJsonArray jsonArrayOf(const char* json)
{
    return QJsonDocument::fromJson(QByteArray(json)).array();
}

}  // namespace

// ---------------------------------------------------------------------------
// LSP: semantic tokens
// ---------------------------------------------------------------------------

TEST(SemanticTokens, DeltasAreAbsolutePerLineAndRelativeWithinALine)
{
    // Token 0: line 0, char 0, length 3, type 0 ("namespace")
    // Token 1: +1 line, char 9 (absolute on a new line), length 4, type 1
    // Token 2: same line, +5 chars (relative), length 2, type 2
    const QJsonArray data{
        0, 0, 3, 0, 0,
        1, 9, 4, 1, 0,
        0, 5, 2, 2, 0,
    };
    const auto tokens = Lsp::decodeSemanticTokens(data);
    ASSERT_EQ(tokens.size(), 3);
    EXPECT_EQ(tokens.at(0).line, 0);
    EXPECT_EQ(tokens.at(0).column, 0);
    EXPECT_EQ(tokens.at(1).line, 1);
    EXPECT_EQ(tokens.at(1).column, 9);
    EXPECT_EQ(tokens.at(2).line, 1);
    // The third token starts 5 characters after the *second* token's start,
    // not after its end: 9 + 5.
    EXPECT_EQ(tokens.at(2).column, 14);
}

TEST(SemanticTokens, AStreamThatIsNotFiveTuplesIsRejected)
{
    QString error;
    const auto tokens = Lsp::decodeSemanticTokens(QJsonArray{0, 0, 3, 0}, &error);
    EXPECT_TRUE(tokens.isEmpty());
    EXPECT_TRUE(error.contains(QLatin1String("multiple of 5")));
}

TEST(SemanticTokens, ANegativeDeltaDropsTheWholeAnswer)
{
    QString error;
    const auto tokens = Lsp::decodeSemanticTokens(
        QJsonArray{0, 0, 3, 0, 0, -1, 4, 2, 1, 0}, &error);
    EXPECT_TRUE(tokens.isEmpty());
    EXPECT_FALSE(error.isEmpty());
}

TEST(SemanticTokens, TheLegendMapsIndexesToNamesAndModifierBits)
{
    const QJsonObject initialize = QJsonDocument::fromJson(R"({
        "capabilities": {
            "semanticTokensProvider": {
                "legend": {
                    "tokenTypes": ["namespace", "class", "function", "macro"],
                    "tokenModifiers": ["declaration", "definition", "deprecated"]
                },
                "full": true
            }
        }
    })").object();
    const Lsp::SemanticLegend legend = Lsp::parseLegend(initialize);
    ASSERT_EQ(legend.tokenTypes.size(), 4);
    EXPECT_EQ(legend.typeName(1), QStringLiteral("class"));
    EXPECT_EQ(legend.typeName(99), QString());
    EXPECT_EQ(legend.modifierBit(QStringLiteral("deprecated")), 2);
    EXPECT_TRUE(legend.hasModifier(1 << 2, QStringLiteral("deprecated")));
    EXPECT_FALSE(legend.hasModifier(1 << 0, QStringLiteral("deprecated")));
    EXPECT_EQ(legend.modifierNames(1 | 4).size(), 2);
}

TEST(SemanticTokens, RolesCoverClangdsTypesAndDefaultToUnknown)
{
    EXPECT_EQ(Lsp::roleFor(QStringLiteral("class")), Lsp::TokenRole::Type);
    EXPECT_EQ(Lsp::roleFor(QStringLiteral("method")), Lsp::TokenRole::Function);
    EXPECT_EQ(Lsp::roleFor(QStringLiteral("macro")), Lsp::TokenRole::Macro);
    EXPECT_EQ(Lsp::roleFor(QStringLiteral("not-a-type")), Lsp::TokenRole::Unknown);
    EXPECT_EQ(Lsp::roleName(Lsp::TokenRole::EnumMember), QStringLiteral("enum member"));
}

TEST(SemanticTokens, AnEmptyDocumentIsNotAnError)
{
    QString error = QStringLiteral("stale");
    const auto tokens = Lsp::decodeSemanticTokens(QJsonArray{}, &error);
    EXPECT_TRUE(tokens.isEmpty());
    EXPECT_TRUE(error.isEmpty());
}

// ---------------------------------------------------------------------------
// LSP: hierarchy
// ---------------------------------------------------------------------------

TEST(Hierarchy, ItemsKeepTheOpaqueDataTheNextRequestMustEcho)
{
    const QJsonArray items = jsonArrayOf(R"([{
        "name": "compute",
        "kind": 12,
        "uri": "file:///tmp/a.cpp",
        "range": {"start": {"line": 3, "character": 0},
                  "end": {"line": 9, "character": 1}},
        "selectionRange": {"start": {"line": 3, "character": 5},
                           "end": {"line": 3, "character": 12}},
        "data": {"file": "/tmp/a.cpp", "offset": 42}
    }])");
    const auto parsed = Lsp::parseHierarchyItems(items);
    ASSERT_EQ(parsed.size(), 1);
    EXPECT_EQ(parsed.first().name, QStringLiteral("compute"));
    EXPECT_EQ(parsed.first().selectionRange.startLine, 3);
    EXPECT_EQ(Lsp::pathFromUri(parsed.first().uri), QStringLiteral("/tmp/a.cpp"));
    EXPECT_EQ(parsed.first().data.value(QStringLiteral("offset")).toInt(), 42);
}

TEST(Hierarchy, PathFromUriDecodesPercentEscapes)
{
    EXPECT_EQ(Lsp::pathFromUri(QStringLiteral("file:///tmp/a%20b.cpp")),
              QStringLiteral("/tmp/a b.cpp"));
    EXPECT_EQ(Lsp::pathFromUri(QStringLiteral("/tmp/already-a-path")),
              QStringLiteral("/tmp/already-a-path"));
}

TEST(Hierarchy, IncomingCallsReadFromAndOutgoingReadTo)
{
    const QJsonArray incoming = jsonArrayOf(R"([{
        "from": {"name": "main", "kind": 12, "uri": "file:///tmp/a.cpp",
                 "range": {"start": {"line": 20, "character": 0},
                           "end": {"line": 24, "character": 1}},
                 "selectionRange": {"start": {"line": 20, "character": 5},
                                    "end": {"line": 20, "character": 9}},
                 "data": {"k": 1}},
        "fromRanges": [{"start": {"line": 22, "character": 4},
                        "end": {"line": 22, "character": 11}}]
    }])");
    const auto calls = Lsp::parseCallEdges(incoming);
    ASSERT_EQ(calls.size(), 1);
    EXPECT_EQ(calls.first().item.name, QStringLiteral("main"));
    ASSERT_EQ(calls.first().fromRanges.size(), 1);
    EXPECT_EQ(calls.first().fromRanges.first().startLine, 22);

    const QJsonArray outgoing = jsonArrayOf(R"([{
        "to": {"name": "helper", "kind": 12, "uri": "file:///tmp/b.cpp",
               "range": {"start": {"line": 1, "character": 0},
                         "end": {"line": 3, "character": 1}},
               "selectionRange": {"start": {"line": 1, "character": 5},
                                  "end": {"line": 1, "character": 11}}},
        "fromRanges": []
    }])");
    const auto callees = Lsp::parseCallEdges(outgoing);
    ASSERT_EQ(callees.size(), 1);
    EXPECT_EQ(callees.first().item.name, QStringLiteral("helper"));
}

TEST(CallGraph, TwoOverloadsAreTwoNodesAndTheTreeCutsCycles)
{
    Lsp::HierarchyItem root;
    root.name = QStringLiteral("a");
    root.uri = QStringLiteral("file:///tmp/x.cpp");
    root.selectionRange.startLine = 1;
    root.selectionRange.startColumn = 0;

    Lsp::HierarchyItem b = root;
    b.name = QStringLiteral("b");
    b.selectionRange.startLine = 10;
    b.selectionRange.startColumn = 0;

    Lsp::HierarchyItem a2 = root;
    a2.selectionRange.startLine = 30;  // a second overload of a()

    Lsp::CallGraph graph;
    graph.addCall(root, b);
    graph.addCall(b, root);   // b calls a — a cycle
    graph.addCall(root, a2);

    EXPECT_EQ(graph.nodes.size(), 3);
    EXPECT_TRUE(graph.hasCycleFrom(Lsp::nodeKey(root)));
    const QString tree = graph.toTreeText(Lsp::nodeKey(root));
    EXPECT_TRUE(tree.contains(QLatin1String("[recursive]")));
    const QString dot = graph.toDot(QStringLiteral("calls"));
    EXPECT_TRUE(dot.startsWith(QLatin1String("digraph")));
    EXPECT_EQ(dot.count(QLatin1String("->")), 3);
}

TEST(ClassView, NestedClassesAreFlattenedIntoTheirParentsMembers)
{
    const QJsonArray symbols = jsonArrayOf(R"([{
        "name": "Widget",
        "kind": 23,
        "detail": "struct Widget : public Base",
        "range": {"start": {"line": 0, "character": 0}, "end": {"line": 30, "character": 1}},
        "selectionRange": {"start": {"line": 0, "character": 7}, "end": {"line": 0, "character": 13}},
        "children": [
            {"name": "paint", "kind": 6,
             "selectionRange": {"start": {"line": 4, "character": 9}, "end": {"line": 4, "character": 14}}},
            {"name": "Impl", "kind": 23, "detail": "struct Widget::Impl",
             "selectionRange": {"start": {"line": 12, "character": 11}, "end": {"line": 12, "character": 15}},
             "children": [
                 {"name": "value", "kind": 8,
                  "selectionRange": {"start": {"line": 13, "character": 8}, "end": {"line": 13, "character": 13}}}
             ]}
        ]
    }])");
    const auto classes = Lsp::buildClassView(symbols);
    ASSERT_EQ(classes.size(), 1);
    EXPECT_EQ(classes.first().name, QStringLiteral("Widget"));
    EXPECT_EQ(classes.first().baseClasses, QStringLiteral("public Base"));
    ASSERT_EQ(classes.first().members.size(), 3);
    EXPECT_EQ(classes.first().members.at(0).name, QStringLiteral("paint"));
    EXPECT_EQ(classes.first().members.at(1).name, QStringLiteral("Impl"));
    EXPECT_EQ(classes.first().members.at(2).name, QStringLiteral("value"));
}

TEST(ClassView, AQualifiedNameIsNotMistakenForABaseList)
{
    const QJsonArray symbols = jsonArrayOf(R"([{
        "name": "Thing", "kind": 5, "detail": "class ns::Thing",
        "selectionRange": {"start": {"line": 0, "character": 6}, "end": {"line": 0, "character": 11}}
    }])");
    const auto classes = Lsp::buildClassView(symbols);
    ASSERT_EQ(classes.size(), 1);
    EXPECT_TRUE(classes.first().baseClasses.isEmpty());
}

// ---------------------------------------------------------------------------
// Workspace edits
// ---------------------------------------------------------------------------

TEST(WorkspaceEdits, EditsApplyBottomUpAndKeepEarlierLinesIntact)
{
    const QString text = QStringLiteral("int foo = 1;\nint bar = foo;\n");
    QList<Lsp::TextEdit> edits;
    edits.append({QString(), 0, 4, 0, 7, QStringLiteral("baz")});   // foo -> baz
    edits.append({QString(), 1, 10, 1, 13, QStringLiteral("baz")}); // foo -> baz
    int applied = 0;
    int dropped = 0;
    const QString result = Lsp::applyTextEdits(text, edits, &applied, &dropped);
    EXPECT_EQ(result, QStringLiteral("int baz = 1;\nint bar = baz;\n"));
    EXPECT_EQ(applied, 2);
    EXPECT_EQ(dropped, 0);
}

TEST(WorkspaceEdits, AnOutOfRangeEditIsDroppedNotAppliedAtTheEnd)
{
    const QString text = QStringLiteral("one\ntwo\n");
    QList<Lsp::TextEdit> edits;
    edits.append({QString(), 9, 0, 9, 2, QStringLiteral("X")});
    int applied = 0;
    int dropped = 0;
    const QString result = Lsp::applyTextEdits(text, edits, &applied, &dropped);
    EXPECT_EQ(result, text);
    EXPECT_EQ(applied, 0);
    EXPECT_EQ(dropped, 1);
}

TEST(WorkspaceEdits, GroupByFilePreservesFirstSeenOrder)
{
    QList<Lsp::TextEdit> edits;
    edits.append({QStringLiteral("/a.cpp"), 0, 0, 0, 1, QStringLiteral("x")});
    edits.append({QStringLiteral("/b.cpp"), 0, 0, 0, 1, QStringLiteral("y")});
    edits.append({QStringLiteral("/a.cpp"), 1, 0, 1, 1, QStringLiteral("z")});
    const auto grouped = Lsp::groupByFile(edits);
    ASSERT_EQ(grouped.size(), 2);
    EXPECT_EQ(grouped.at(0).first, QStringLiteral("/a.cpp"));
    EXPECT_EQ(grouped.at(0).second.size(), 2);
    EXPECT_EQ(grouped.at(1).second.size(), 1);
}

// ---------------------------------------------------------------------------
// Include sorting
// ---------------------------------------------------------------------------

TEST(IncludeSort, IncludeRunsAreSortedWithinTheirGroupAndKeptApart)
{
    const QString text = QStringLiteral(
        "#include \"zebra.h\"\n"
        "#include \"alpha.h\"\n"
        "\n"
        "#include <vector>\n"
        "#include <array>\n"
        "\n"
        "int main() { return 0; }\n");
    const auto result = Editor::sortIncludes(text);
    EXPECT_TRUE(result.changed);
    EXPECT_EQ(result.sortedIncludes, 4);
    EXPECT_EQ(result.text,
              QStringLiteral("#include \"alpha.h\"\n"
                             "#include \"zebra.h\"\n"
                             "\n"
                             "#include <array>\n"
                             "#include <vector>\n"
                             "\n"
                             "int main() { return 0; }\n"));
}

TEST(IncludeSort, AnIncludeInsideAConditionalIsNeverMovedAcrossIt)
{
    const QString text = QStringLiteral(
        "#ifdef _WIN32\n"
        "#include <windows.h>\n"
        "#include <tchar.h>\n"
        "#endif\n"
        "#include <cstdio>\n"
        "#include <cstdlib>\n");
    const auto result = Editor::sortIncludes(text);
    EXPECT_TRUE(result.changed);
    // The Windows block sorts internally (tchar before windows) and the two
    // libc headers sort after `#endif`, but nothing crosses the directive.
    EXPECT_EQ(result.text,
              QStringLiteral("#ifdef _WIN32\n"
                             "#include <tchar.h>\n"
                             "#include <windows.h>\n"
                             "#endif\n"
                             "#include <cstdio>\n"
                             "#include <cstdlib>\n"));
}

TEST(IncludeSort, ATrailingCommentTravelsWithItsInclude)
{
    const QString text = QStringLiteral(
        "#include \"b.h\" // the B interface\n"
        "#include \"a.h\" // the A interface\n");
    const auto result = Editor::sortIncludes(text);
    EXPECT_EQ(result.text,
              QStringLiteral("#include \"a.h\" // the A interface\n"
                             "#include \"b.h\" // the B interface\n"));
}

TEST(IncludeSort, SystemLastIsTheDefaultOrder)
{
    const QString text = QStringLiteral(
        "#include <vector>\n"
        "#include \"local.h\"\n");
    Editor::IncludeSortOptions options;
    options.separateGroups = false;
    const auto result = Editor::sortIncludes(text, options);
    EXPECT_EQ(result.text,
              QStringLiteral("#include \"local.h\"\n"
                             "#include <vector>\n"));
}

TEST(IncludeSort, AnAlreadySortedFileIsNotRewritten)
{
    const QString text = QStringLiteral("#include <a.h>\n#include <b.h>\n");
    const auto result = Editor::sortIncludes(text);
    EXPECT_FALSE(result.changed);
    EXPECT_EQ(result.text, text);
}

TEST(IncludeSort, IncludeLinesAreRecognisedWithAnySpacing)
{
    EXPECT_TRUE(Editor::isIncludeLine(QStringLiteral("#include <a>")));
    EXPECT_TRUE(Editor::isIncludeLine(QStringLiteral("  #  include \"a\"")));
    EXPECT_FALSE(Editor::isIncludeLine(QStringLiteral("// #include <a>")));
    EXPECT_TRUE(Editor::isConditionalDirective(QStringLiteral("  #endif")));
    EXPECT_FALSE(Editor::isConditionalDirective(QStringLiteral("#pragma once")));
}

// ---------------------------------------------------------------------------
// Line diff (gutter marks)
// ---------------------------------------------------------------------------

TEST(LineDiff, AOneLineEditIsModifiedNotDeletedAndAdded)
{
    const auto marks = Editor::diffLines(
        {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")},
        {QStringLiteral("a"), QStringLiteral("B"), QStringLiteral("c")});
    ASSERT_EQ(marks.size(), 3);
    EXPECT_EQ(marks.at(0).change, Editor::LineChange::Unchanged);
    EXPECT_EQ(marks.at(1).change, Editor::LineChange::Modified);
    EXPECT_EQ(marks.at(2).change, Editor::LineChange::Unchanged);
    EXPECT_EQ(Editor::changedLineCount(marks), 1);
}

TEST(LineDiff, InsertedLinesAreAddedAndDeletedLinesDoNotShiftTheRest)
{
    const auto marks = Editor::diffLines(
        {QStringLiteral("a"), QStringLiteral("c")},
        {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
    ASSERT_EQ(marks.size(), 3);
    EXPECT_EQ(marks.at(1).change, Editor::LineChange::Added);
    EXPECT_EQ(marks.at(2).change, Editor::LineChange::Unchanged);
}

TEST(LineDiff, IdenticalTextHasNoMarks)
{
    const QString text = QStringLiteral("x\ny\n");
    const auto marks = Editor::diffText(text, text);
    EXPECT_EQ(Editor::changedLineCount(marks), 0);
}

TEST(LineDiff, ATrailingNewlineIsNotAnExtraLine)
{
    const auto marks = Editor::diffText(QStringLiteral("a\n"), QStringLiteral("a"));
    ASSERT_EQ(marks.size(), 1);
    EXPECT_EQ(marks.at(0).change, Editor::LineChange::Unchanged);
}

// ---------------------------------------------------------------------------
// Debug symbols
// ---------------------------------------------------------------------------

TEST(DebugSymbols, GdbPrettyPrintingUsesTheMiCommand)
{
    const auto commands = Debug::prettyPrintCommands(Debug::Backend::Gdb, true);
    ASSERT_EQ(commands.size(), 1);
    // The console form is silently rejected by the MI interpreter; this is
    // the assertion that stops that regression.
    EXPECT_EQ(commands.first(), QStringLiteral("-enable-pretty-printing"));
}

TEST(DebugSymbols, CdbHasNoPrettyPrinterAndNothingIsSentInstead)
{
    EXPECT_TRUE(Debug::prettyPrintCommands(Debug::Backend::Cdb, true).isEmpty());
    EXPECT_TRUE(Debug::enumDisplayCommands(Debug::Backend::Cdb, true).isEmpty());
}

TEST(DebugSymbols, SubstitutePathQuotesBothHalves)
{
    Debug::PathMapping mapping;
    mapping.from = QStringLiteral("/build/src");
    mapping.to = QStringLiteral("C:/work/my project/src");
    const auto commands = Debug::sourceMappingCommands(Debug::Backend::Gdb, {mapping});
    ASSERT_EQ(commands.size(), 1);
    EXPECT_EQ(commands.first(),
              QStringLiteral("set substitute-path /build/src \"C:/work/my project/src\""));
}

TEST(DebugSymbols, LldbUsesSourceMapAndCdbUsesSrcpath)
{
    Debug::PathMapping mapping{QStringLiteral("/build"), QStringLiteral("/home/a")};
    const auto lldb = Debug::sourceMappingCommands(Debug::Backend::Lldb, {mapping});
    ASSERT_EQ(lldb.size(), 1);
    EXPECT_TRUE(lldb.first().startsWith(QLatin1String("settings set target.source-map")));
    const auto cdb = Debug::sourceMappingCommands(Debug::Backend::Cdb, {mapping});
    ASSERT_EQ(cdb.size(), 1);
    EXPECT_EQ(cdb.first(), QStringLiteral(".srcpath+ /home/a"));
}

TEST(DebugSymbols, StartupCommandsPutPathRewritingBeforeTheDisplaySwitches)
{
    Debug::SymbolSettings settings;
    settings.sourceMappings.append({QStringLiteral("/build"), QStringLiteral("/src")});
    settings.symbolSearchPaths.append(QStringLiteral("/usr/lib/debug"));
    const auto commands = Debug::startupCommands(Debug::Backend::Gdb, settings);
    ASSERT_GE(commands.size(), 4);
    EXPECT_TRUE(commands.first().startsWith(QLatin1String("set substitute-path")));
    EXPECT_TRUE(commands.contains(QStringLiteral("-enable-pretty-printing")));
}

TEST(DebugSymbols, AnEmptyMappingIsSkipped)
{
    Debug::PathMapping empty;
    EXPECT_TRUE(Debug::sourceMappingCommands(Debug::Backend::Gdb, {empty}).isEmpty());
}

TEST(DebugSymbols, NatvisIsOnlySentToCdb)
{
    EXPECT_TRUE(Debug::natvisCommands(Debug::Backend::Gdb,
                                      {QStringLiteral("x.natvis")}).isEmpty());
    const auto cdb = Debug::natvisCommands(Debug::Backend::Cdb,
                                           {QStringLiteral("x.natvis")});
    ASSERT_EQ(cdb.size(), 1);
    EXPECT_EQ(cdb.first(), QStringLiteral(".nvload x.natvis"));
}

TEST(DebugSymbols, BackendNamesRoundTrip)
{
    EXPECT_EQ(Debug::backendFromName(QStringLiteral("lldb")), Debug::Backend::Lldb);
    EXPECT_EQ(Debug::backendFromName(QStringLiteral("cdb")), Debug::Backend::Cdb);
    EXPECT_EQ(Debug::backendFromName(QStringLiteral("nope")), Debug::Backend::Unknown);
    EXPECT_EQ(Debug::backendName(Debug::Backend::Gdb), QStringLiteral("gdb"));
}

// ---------------------------------------------------------------------------
// Git: blame blocks
// ---------------------------------------------------------------------------

TEST(GitBlame, ConsecutiveLinesOfOneCommitAreOneBlock)
{
    QList<Git::BlameLine> lines;
    for (int i = 1; i <= 3; ++i) {
        Git::BlameLine line;
        line.line = i;
        line.sha = QStringLiteral("abcdef1234567890");
        line.author = QStringLiteral("Ada");
        line.content = QStringLiteral("line %1").arg(i);
        lines.append(line);
    }
    Git::BlameLine other;
    other.line = 4;
    other.sha = QStringLiteral("1234567890abcdef");
    other.author = QStringLiteral("Bob");
    other.content = QStringLiteral("line 4");
    lines.append(other);

    const auto blocks = Git::groupBlame(lines);
    ASSERT_EQ(blocks.size(), 2);
    EXPECT_EQ(blocks.at(0).firstLine, 1);
    EXPECT_EQ(blocks.at(0).lineCount, 3);
    EXPECT_EQ(blocks.at(0).content.size(), 3);
    EXPECT_EQ(blocks.at(1).shortSha, QStringLiteral("12345678"));
}

TEST(GitBlame, TheSameCommitAppearingTwiceIsTwoBlocks)
{
    QList<Git::BlameLine> lines;
    for (int i = 1; i <= 2; ++i) {
        Git::BlameLine line;
        line.line = i * 2;  // 2 and 4 — not adjacent
        line.sha = QStringLiteral("aaaaaaaa");
        lines.append(line);
    }
    EXPECT_EQ(Git::groupBlame(lines).size(), 2);
}

TEST(GitBlame, TheColourIndexIsStableForTheSameSha)
{
    const int first = Git::blameColourIndex(QStringLiteral("deadbeef"));
    const int second = Git::blameColourIndex(QStringLiteral("deadbeef"));
    EXPECT_EQ(first, second);
    EXPECT_GE(first, 0);
    EXPECT_LT(first, 8);
}

// ---------------------------------------------------------------------------
// Git: three-way merge
// ---------------------------------------------------------------------------

TEST(MergeFile, BothSidesAndTheBaseAreParsed)
{
    const QString text =
        "line before\n"
        "<<<<<<< HEAD\n"
        "ours()\n"
        "||||||| merged common ancestors\n"
        "base()\n"
        "=======\n"
        "theirs()\n"
        ">>>>>>> feature\n"
        "line after\n";
    const auto file = Git::parseConflictFile(text);
    ASSERT_TRUE(file.hasConflicts);
    ASSERT_EQ(file.hunks.size(), 1);
    EXPECT_EQ(file.hunks.first().oursLabel, QStringLiteral("HEAD"));
    EXPECT_EQ(file.hunks.first().theirsLabel, QStringLiteral("feature"));
    EXPECT_EQ(file.hunks.first().ourLines, QStringList{QStringLiteral("ours()")});
    EXPECT_EQ(file.hunks.first().baseLines, QStringList{QStringLiteral("base()")});
    EXPECT_EQ(file.hunks.first().theirLines, QStringList{QStringLiteral("theirs()")});
    EXPECT_EQ(file.prefix, QStringList{QStringLiteral("line before")});
    EXPECT_EQ(file.suffix, QStringList{QStringLiteral("line after")});
}

TEST(MergeFile, ABaseSectionIsOptional)
{
    const QString text =
        "<<<<<<< HEAD\n"
        "a\n"
        "=======\n"
        "b\n"
        ">>>>>>> other\n";
    const auto file = Git::parseConflictFile(text);
    ASSERT_EQ(file.hunks.size(), 1);
    EXPECT_FALSE(file.hunks.first().hasBase);
    EXPECT_TRUE(file.hunks.first().baseLines.isEmpty());
}

TEST(MergeFile, TwoHunksKeepTheTextBetweenThem)
{
    const QString text =
        "head\n"
        "<<<<<<< HEAD\n"
        "a1\n"
        "=======\n"
        "b1\n"
        ">>>>>>> x\n"
        "middle\n"
        "<<<<<<< HEAD\n"
        "a2\n"
        "=======\n"
        "b2\n"
        ">>>>>>> x\n"
        "tail\n";
    const auto file = Git::parseConflictFile(text);
    ASSERT_EQ(file.hunks.size(), 2);
    ASSERT_EQ(file.betweenTexts.size(), 1);
    // `betweenTexts[i]` is the text between hunk i and hunk i+1, so the whole
    // of it is the single "middle" line — not a list of lists.
    EXPECT_EQ(file.betweenTexts.at(0), QStringLiteral("middle"));
    const QString resolved = Git::resolveAll(file, Git::MergeChoice::Theirs);
    EXPECT_EQ(resolved,
              QStringLiteral("head\nb1\nmiddle\nb2\ntail"));
}

TEST(MergeFile, AFileWithNoMarkersIsReportedAsAlreadyResolved)
{
    const auto file = Git::parseConflictFile(QStringLiteral("int main() {}\n"));
    EXPECT_FALSE(file.hasConflicts);
    EXPECT_TRUE(file.hunks.isEmpty());
    EXPECT_EQ(Git::conflictCount(file), 0);
}

TEST(MergeFile, BothKeepsOursThenTheirsAndReversedSwaps)
{
    Git::MergeHunk hunk;
    hunk.ourLines = {QStringLiteral("a")};
    hunk.theirLines = {QStringLiteral("b")};
    EXPECT_EQ(Git::resolveHunk(hunk, Git::MergeChoice::Both),
              (QStringList{QStringLiteral("a"), QStringLiteral("b")}));
    EXPECT_EQ(Git::resolveHunk(hunk, Git::MergeChoice::BothReversed),
              (QStringList{QStringLiteral("b"), QStringLiteral("a")}));
}

TEST(MergeFile, CrlfIsPreservedOnResolve)
{
    const QString text =
        "a\r\n"
        "<<<<<<< HEAD\r\n"
        "ours\r\n"
        "=======\r\n"
        "theirs\r\n"
        ">>>>>>> x\r\n";
    const auto file = Git::parseConflictFile(text);
    ASSERT_EQ(file.hunks.size(), 1);
    EXPECT_EQ(file.newline, QStringLiteral("\r\n"));
    const QString resolved = Git::resolveAll(file, Git::MergeChoice::Ours);
    EXPECT_TRUE(resolved.contains(QLatin1String("\r\n")));
    EXPECT_FALSE(resolved.contains(QLatin1String("<<<<<<<")));
}

// ---------------------------------------------------------------------------
// Git: commit message, signing, submodules, worktrees
// ---------------------------------------------------------------------------

TEST(GitCommit, ATemplateSubstitutesTheBranchAndFiles)
{
    const QString templated = Git::applyCommitTemplate(
        QStringLiteral("feat: {branch}\n\nFiles: {files}\n"), QStringLiteral("main"),
        {QStringLiteral("a.cpp"), QStringLiteral("b.cpp")});
    EXPECT_TRUE(templated.contains(QLatin1String("feat: main")));
    EXPECT_TRUE(templated.contains(QLatin1String("a.cpp, b.cpp")));
}

TEST(GitCommit, SigningIsOnlyAddedWhenAskedAndTheKeyOnlyWhenSet)
{
    const auto plain = Git::commitArguments(QStringLiteral("msg"), false, false, false, QString());
    EXPECT_FALSE(plain.contains(QStringLiteral("--gpg-sign")));
    EXPECT_TRUE(plain.contains(QStringLiteral("--cleanup=strip")));

    const auto signedWithKey = Git::commitArguments(QStringLiteral("msg"), true, true, true,
                                                    QStringLiteral("ABCD1234"));
    EXPECT_TRUE(signedWithKey.contains(QStringLiteral("--gpg-sign=ABCD1234")));
    EXPECT_TRUE(signedWithKey.contains(QStringLiteral("--amend")));
    EXPECT_TRUE(signedWithKey.contains(QStringLiteral("--signoff")));
}

TEST(GitCommit, TheSigningConfigReadsOnlyTheKeysThatDecideSigning)
{
    const QString config =
        "commit.gpgsign true\n"
        "user.signingkey ABCD1234\n"
        "gpg.format ssh\n"
        "user.name Ada\n";
    const auto parsed = Git::parseSigningConfig(config);
    EXPECT_TRUE(parsed.enabled);
    EXPECT_EQ(parsed.keyId, QStringLiteral("ABCD1234"));
    EXPECT_EQ(parsed.format, QStringLiteral("ssh"));
}

TEST(GitSubmodule, StatusCharactersAndDescribeAreParsed)
{
    const QString text =
        " 1234567890abcdef path/one (v1.2.3)\n"
        "-abcdef1234567890 path/two\n"
        "+1111111111111111 path/three (heads/main)\n";
    const auto entries = Git::parseSubmoduleStatus(text);
    ASSERT_EQ(entries.size(), 3);
    EXPECT_EQ(entries.at(0).status, QLatin1Char(' '));
    EXPECT_EQ(entries.at(0).path, QStringLiteral("path/one"));
    EXPECT_EQ(entries.at(0).describe, QStringLiteral("v1.2.3"));
    EXPECT_EQ(entries.at(1).status, QLatin1Char('-'));
    EXPECT_TRUE(entries.at(1).describe.isEmpty());
    EXPECT_EQ(entries.at(2).status, QLatin1Char('+'));
}

TEST(GitWorktree, PorcelainBlocksBecomeEntries)
{
    const QString text =
        "worktree /repo\n"
        "HEAD 1111111111111111111111111111111111111111\n"
        "branch refs/heads/main\n"
        "\n"
        "worktree /repo-feature\n"
        "HEAD 2222222222222222222222222222222222222222\n"
        "detached\n"
        "\n";
    const auto entries = Git::parseWorktreeList(text);
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries.at(0).path, QStringLiteral("/repo"));
    EXPECT_EQ(entries.at(0).branch, QStringLiteral("main"));
    // The main worktree is the *first* block, whatever branch it has checked
    // out. Deciding by "no branch" would mislabel every detached HEAD.
    EXPECT_TRUE(entries.at(0).isMain(0));
    EXPECT_FALSE(entries.at(1).isMain(1));
    EXPECT_TRUE(entries.at(1).detached);
}

TEST(GitRemote, SetUrlAndRenameBuildTheExactArguments)
{
    EXPECT_EQ(Git::setRemoteUrl(QStringLiteral("origin"), QStringLiteral("git@x:y.git"), true),
              (QStringList{QStringLiteral("remote"), QStringLiteral("set-url"),
                           QStringLiteral("--push"), QStringLiteral("origin"),
                           QStringLiteral("git@x:y.git")}));
    EXPECT_EQ(Git::renameRemote(QStringLiteral("a"), QStringLiteral("b")),
              (QStringList{QStringLiteral("remote"), QStringLiteral("rename"),
                           QStringLiteral("a"), QStringLiteral("b")}));
}

// ---------------------------------------------------------------------------
// Package managers
// ---------------------------------------------------------------------------

TEST(PackageManager, VcpkgInstallPutsTheTripletInThePackageSpec)
{
    const auto arguments = Pkg::installArguments(Pkg::Manager::Vcpkg, QStringLiteral("fmt"),
                                                 QString(), QStringLiteral("x64-linux"));
    ASSERT_EQ(arguments.size(), 2);
    EXPECT_EQ(arguments.at(0), QStringLiteral("install"));
    EXPECT_EQ(arguments.at(1), QStringLiteral("fmt:x64-linux"));
}

TEST(PackageManager, ConanUsesTheV2RequiresForm)
{
    const auto arguments = Pkg::installArguments(Pkg::Manager::Conan, QStringLiteral("fmt"),
                                                 QStringLiteral("10.1.1"), QString());
    ASSERT_GE(arguments.size(), 2);
    EXPECT_EQ(arguments.at(1), QStringLiteral("--requires=fmt/10.1.1"));
    EXPECT_TRUE(arguments.contains(QStringLiteral("--build=missing")));
}

TEST(PackageManager, VcpkgListParsesNameVersionAndDescription)
{
    const auto packages = Pkg::parseList(
        Pkg::Manager::Vcpkg,
        QStringLiteral("fmt:x64-windows      10.1.1       A modern formatting library\n"
                       "zlib:x64-windows     1.2.13       Compression library\n"));
    ASSERT_EQ(packages.size(), 2);
    EXPECT_EQ(packages.at(0).name, QStringLiteral("fmt"));
    EXPECT_EQ(packages.at(0).triplet, QStringLiteral("x64-windows"));
    EXPECT_EQ(packages.at(0).version, QStringLiteral("10.1.1"));
    EXPECT_TRUE(packages.at(0).installed);
    EXPECT_TRUE(packages.at(0).description.contains(QLatin1String("formatting")));
}

TEST(PackageManager, BrewListAndSearchAreParsed)
{
    const auto installed = Pkg::parseList(Pkg::Manager::Brew,
                                          QStringLiteral("fmt 10.1.1\nqt 6.8.3\n"));
    ASSERT_EQ(installed.size(), 2);
    EXPECT_EQ(installed.at(1).name, QStringLiteral("qt"));

    const auto found = Pkg::parseSearch(
        Pkg::Manager::Brew,
        QStringLiteral("==> Formulae\nfmt (10.1.1)\nboost (1.84.0)\n==> Casks\n"));
    ASSERT_EQ(found.size(), 2);
    EXPECT_EQ(found.at(1).version, QStringLiteral("1.84.0"));
}

TEST(PackageManager, AptInstalledListIsParsed)
{
    const auto packages = Pkg::parseList(
        Pkg::Manager::Apt,
        QStringLiteral("libfmt-dev/now 9.1.0+ds1-2 amd64 [installed]\n"));
    ASSERT_EQ(packages.size(), 1);
    EXPECT_EQ(packages.at(0).name, QStringLiteral("libfmt-dev"));
    EXPECT_EQ(packages.at(0).version, QStringLiteral("9.1.0+ds1-2"));
}

TEST(PackageManager, ConanListIgnoresHeadingsAndKeepsNameVersion)
{
    const auto packages = Pkg::parseList(
        Pkg::Manager::Conan,
        QStringLiteral("Local Cache\n"
                       "  zlib/1.2.13#7f3a1b\n"
                       "  fmt/10.1.1#aabbcc\n"));
    ASSERT_EQ(packages.size(), 2);
    EXPECT_EQ(packages.at(0).name, QStringLiteral("zlib"));
    EXPECT_EQ(packages.at(0).version, QStringLiteral("1.2.13"));
}

TEST(PackageManager, CMakeIntegrationNamesTheToolchainFileAndTriplet)
{
    const auto arguments = Pkg::cmakeIntegrationArguments(
        Pkg::Manager::Vcpkg, QStringLiteral("/opt/vcpkg"), QStringLiteral("arm64-osx"));
    ASSERT_EQ(arguments.size(), 2);
    EXPECT_TRUE(arguments.at(0).contains(QLatin1String("scripts/buildsystems/vcpkg.cmake")));
    EXPECT_EQ(arguments.at(1), QStringLiteral("-DVCPKG_TARGET_TRIPLET=arm64-osx"));
}

TEST(PackageManager, HunterHasNoExternalCommand)
{
    EXPECT_TRUE(Pkg::installArguments(Pkg::Manager::Hunter, QStringLiteral("fmt"),
                                      QString(), QString()).isEmpty());
    EXPECT_TRUE(Pkg::versionArguments(Pkg::Manager::Hunter).isEmpty());
}

TEST(PackageManager, ManifestsNameTheManagersAProjectUses)
{
    const auto info = Pkg::detectManifests(
        {QStringLiteral("vcpkg.json"), QStringLiteral("conanfile.txt"),
         QStringLiteral("CMakeLists.txt")});
    ASSERT_EQ(info.managers.size(), 2);
    EXPECT_EQ(info.managers.at(0), Pkg::Manager::Vcpkg);
    EXPECT_EQ(info.managers.at(1), Pkg::Manager::Conan);
}

TEST(PackageManager, VersionParsingDoesNotInventAVersion)
{
    EXPECT_EQ(Pkg::parseVersion(Pkg::Manager::Conan, QStringLiteral("Conan version 2.0.17")),
              QStringLiteral("2.0.17"));
    EXPECT_EQ(Pkg::parseVersion(Pkg::Manager::Vcpkg, QStringLiteral("boom")), QString());
}

// ---------------------------------------------------------------------------
// Hex viewer
// ---------------------------------------------------------------------------

TEST(HexViewer, OffsetsStayAbsoluteAcrossWindows)
{
    const QByteArray data("0123456789abcdefXYZ", 19);
    const auto rows = Viewer::hexRows(data, 0x1000, 16);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows.at(0).offset, 0x1000u);
    EXPECT_EQ(rows.at(1).offset, 0x1010u);
    EXPECT_EQ(rows.at(1).bytes.size(), 3);
    EXPECT_EQ(rows.at(1).ascii, QStringLiteral("XYZ"));
    EXPECT_EQ(rows.at(0).ascii, QStringLiteral("0123456789abcdef"));
}

TEST(HexViewer, TheRowOfASearchHitIsMarked)
{
    const QByteArray data(64, '\0');
    const auto rows = Viewer::hexRows(data, 0, 16, 20, 4);
    ASSERT_EQ(rows.size(), 4);
    EXPECT_FALSE(rows.at(0).highlighted);
    EXPECT_TRUE(rows.at(1).highlighted);  // byte 20 is in row 1
}

TEST(HexViewer, QueriesAcceptSpacedHexCescapesAndText)
{
    EXPECT_EQ(Viewer::parseHexQuery(QStringLiteral("DE AD BE EF")),
              QByteArray::fromHex("DEADBEEF"));
    EXPECT_EQ(Viewer::parseHexQuery(QStringLiteral("\\xDE\\xAD")),
              QByteArray::fromHex("DEAD"));
    bool isText = false;
    EXPECT_EQ(Viewer::parseHexQuery(QStringLiteral("\"hello\""), &isText),
              QByteArray("hello"));
    EXPECT_TRUE(isText);
    EXPECT_EQ(Viewer::parseHexQuery(QStringLiteral("hello world"), &isText),
              QByteArray("hello world"));
    EXPECT_TRUE(isText);
}

TEST(HexViewer, FindReturnsTheOffsetOrMinusOne)
{
    const QByteArray data("abcdefabcdef");
    EXPECT_EQ(Viewer::findBytes(data, QByteArray("cd")), 2);
    EXPECT_EQ(Viewer::findBytes(data, QByteArray("cd"), 3), 8);
    EXPECT_EQ(Viewer::findBytes(data, QByteArray("zz")), -1);
}

TEST(HexViewer, TheDumpTextKeepsTheByteColumnsAligned)
{
    const QByteArray data("AB");
    const QString dump = Viewer::hexDumpText(data, 0, 16);
    EXPECT_TRUE(dump.startsWith(QLatin1String("0000")));
    EXPECT_TRUE(dump.contains(QLatin1String("41 42")));
}

// ---------------------------------------------------------------------------
// Log viewer
// ---------------------------------------------------------------------------

TEST(LogViewer, LevelsAreFoundAfterATimestamp)
{
    QString timestamp;
    EXPECT_EQ(Viewer::classifyLine(QStringLiteral("2024-01-01T12:00:00Z ERROR boom"), &timestamp),
              Viewer::Level::Error);
    EXPECT_EQ(timestamp, QStringLiteral("2024-01-01T12:00:00Z"));
    EXPECT_EQ(Viewer::classifyLine(QStringLiteral("[warn] disk almost full")),
              Viewer::Level::Warning);
    EXPECT_EQ(Viewer::classifyLine(QStringLiteral("Jan  1 12:00:00 host app[1]: starting")),
              Viewer::Level::Unknown);
    EXPECT_EQ(Viewer::classifyLine(
                  QStringLiteral("2024-01-01 12:00:00,123 - app - INFO - ready")),
              Viewer::Level::Info);
    EXPECT_EQ(Viewer::classifyLine(QStringLiteral("FATAL: cannot continue")),
              Viewer::Level::Critical);
}

TEST(LogViewer, AStackTraceInheritsTheLevelOfTheErrorAboveIt)
{
    const QString text =
        "2024-01-01T00:00:00Z ERROR failed\n"
        "  at frame one\n"
        "  at frame two\n"
        "2024-01-01T00:00:01Z INFO recovered\n";
    const auto lines = Viewer::parseLog(text);
    ASSERT_EQ(lines.size(), 4);
    EXPECT_EQ(lines.at(1).level, Viewer::Level::Error);
    EXPECT_EQ(lines.at(2).level, Viewer::Level::Error);
    EXPECT_EQ(lines.at(3).level, Viewer::Level::Info);
}

TEST(LogViewer, TheFilterCombinesLevelAndSubstring)
{
    const auto lines = Viewer::parseLog(
        QStringLiteral("[ERROR] disk\n[INFO] disk ok\n[ERROR] network\n"));
    Viewer::LogFilter filter;
    filter.minimumLevel = Viewer::Level::Warning;
    filter.substring = QStringLiteral("disk");
    const auto kept = Viewer::filterLog(lines, filter);
    ASSERT_EQ(kept.size(), 1);
    EXPECT_TRUE(kept.first().text.contains(QLatin1String("disk")));
}

TEST(LogViewer, AnInvalidRegexFallsBackToSubstringInsteadOfEmptyingTheView)
{
    const auto lines = Viewer::parseLog(QStringLiteral("alpha\nbeta [unclosed\n"));
    Viewer::LogFilter filter;
    filter.useRegex = true;
    filter.substring = QStringLiteral("[unclosed");
    // A half-typed pattern is not valid regex; the half-typed text is still
    // what the user is looking for, so the filter degrades to a substring
    // search rather than matching nothing.
    const auto kept = Viewer::filterLog(lines, filter);
    ASSERT_EQ(kept.size(), 1);
    EXPECT_TRUE(kept.first().text.contains(QLatin1String("[unclosed")));
}

TEST(LogViewer, CountsAndSummaryNameTheProblems)
{
    const auto lines = Viewer::parseLog(
        QStringLiteral("[ERROR] a\n[warn] b\n[warn] c\ninfo line\n"));
    const auto counts = Viewer::countLevels(lines);
    EXPECT_EQ(counts.error, 1);
    EXPECT_EQ(counts.warning, 2);
    EXPECT_EQ(counts.problems(), 3);
    const QString summary = Viewer::describeCounts(counts);
    EXPECT_TRUE(summary.contains(QLatin1String("1 error")));
    EXPECT_TRUE(summary.contains(QLatin1String("2 warning")));
}

// ---------------------------------------------------------------------------
// Binary sniffing
// ---------------------------------------------------------------------------

TEST(Sniffing, TheCommonFormatsAreIdentified)
{
    EXPECT_EQ(Viewer::sniffBytes(QByteArray("\x89PNG\r\n\x1a\n", 8)), Viewer::BinaryKind::Png);
    EXPECT_EQ(Viewer::sniffBytes(QByteArray("\xFF\xD8\xFF", 3)), Viewer::BinaryKind::Jpeg);
    EXPECT_EQ(Viewer::sniffBytes(QByteArray("%PDF-1.7", 8)), Viewer::BinaryKind::Pdf);
    EXPECT_EQ(Viewer::sniffBytes(QByteArray("\x7f" "ELF", 4)), Viewer::BinaryKind::Elf);
    EXPECT_EQ(Viewer::sniffBytes(QByteArray("MZ\x90\x00", 4)), Viewer::BinaryKind::Pe);
    EXPECT_EQ(Viewer::sniffBytes(QByteArray("hello world\n", 12)), Viewer::BinaryKind::Text);
    EXPECT_EQ(Viewer::sniffBytes(QByteArray("\x00\x01\x02", 3)), Viewer::BinaryKind::Unknown);
    EXPECT_TRUE(Viewer::isImageKind(Viewer::BinaryKind::Svg));
    EXPECT_FALSE(Viewer::isImageKind(Viewer::BinaryKind::Pdf));
}

// ---------------------------------------------------------------------------
// Build options
// ---------------------------------------------------------------------------

TEST(CompileOptions, DefinesIncludesLibrariesAndStandardsAreSplitOut)
{
    const auto options = Build::parseCompileCommand(
        {QStringLiteral("-std=c++20"),
         QStringLiteral("-DFOO=1"), QStringLiteral("-DBAR"),
         QStringLiteral("-I/usr/include"), QStringLiteral("-isystem/opt/qt"),
         QStringLiteral("-L/opt/lib"), QStringLiteral("-lfmt"),
         QStringLiteral("-Wall"), QStringLiteral("main.cpp")});
    EXPECT_EQ(options.cppStandard, QStringLiteral("c++20"));
    EXPECT_EQ(options.defines, (QStringList{QStringLiteral("FOO=1"), QStringLiteral("BAR")}));
    EXPECT_EQ(options.includePaths,
              (QStringList{QStringLiteral("/usr/include"), QStringLiteral("/opt/qt")}));
    EXPECT_EQ(options.libraryPaths, QStringList{QStringLiteral("/opt/lib")});
    EXPECT_EQ(options.libraries, QStringList{QStringLiteral("fmt")});
    // The caller passes the arguments *after* the program (that is how a
    // compile_commands.json "arguments" entry is read once the compiler is
    // stripped), so the flag list must hold "-Wall" and nothing else.
    EXPECT_EQ(options.flags, QStringList{QStringLiteral("-Wall")});
    EXPECT_TRUE(options.toArguments().contains(QStringLiteral("-DFOO=1")));
}

TEST(CompileOptions, TheMsvcFormUsesSlashesAndLibSuffix)
{
    Build::CompileOptions options;
    options.defines = {QStringLiteral("WIN32")};
    options.includePaths = {QStringLiteral("C:/qt/include")};
    options.libraries = {QStringLiteral("fmt")};
    const auto arguments = options.toMsvcArguments();
    EXPECT_TRUE(arguments.contains(QStringLiteral("/DWIN32")));
    EXPECT_TRUE(arguments.contains(QStringLiteral("/IC:/qt/include")));
    EXPECT_TRUE(arguments.contains(QStringLiteral("fmt.lib")));
}

TEST(CompileOptions, ACommandStringIsSplitHonouringQuotes)
{
    const auto arguments = Build::splitCommandLine(
        QStringLiteral("clang++ -I\"C:/Program Files/Qt\" -DA=\"x y\" main.cpp"));
    ASSERT_EQ(arguments.size(), 4);
    EXPECT_EQ(arguments.at(1), QStringLiteral("-IC:/Program Files/Qt"));
    EXPECT_EQ(arguments.at(2), QStringLiteral("-DA=x y"));
}

TEST(CrossCompile, TheToolchainFileNamesTheSystemAndBothCompilers)
{
    Build::CrossToolchain toolchain;
    toolchain.name = QStringLiteral("aarch64-linux-gnu");
    toolchain.systemName = QStringLiteral("Linux");
    toolchain.processor = QStringLiteral("aarch64");
    toolchain.cCompiler = QStringLiteral("/usr/bin/aarch64-linux-gnu-gcc");
    toolchain.cxxCompiler = QStringLiteral("/usr/bin/aarch64-linux-gnu-g++");
    toolchain.sysroot = QStringLiteral("/usr/aarch64-linux-gnu");
    ASSERT_TRUE(toolchain.isValid());
    const QString file = toolchain.toCMakeToolchainFile();
    EXPECT_TRUE(file.contains(QLatin1String("CMAKE_SYSTEM_NAME \"Linux\"")));
    EXPECT_TRUE(file.contains(QLatin1String("CMAKE_CXX_COMPILER")));
    EXPECT_TRUE(file.contains(QLatin1String("CMAKE_SYSROOT")));
    // A host library must never be found for a target build.
    EXPECT_TRUE(file.contains(QLatin1String("MODE_LIBRARY ONLY")));
}

TEST(CrossCompile, AMissingSystemNameIsReportedNotSilentlyIgnored)
{
    Build::CrossToolchain toolchain;
    toolchain.cCompiler = QStringLiteral("/bin/gcc");
    toolchain.cxxCompiler = QStringLiteral("/bin/g++");
    EXPECT_FALSE(toolchain.isValid());
    const auto problems = toolchain.problems();
    ASSERT_FALSE(problems.isEmpty());
    EXPECT_TRUE(problems.first().contains(QLatin1String("CMAKE_SYSTEM_NAME")));
}

TEST(CrossCompile, ATripletPrefixIsInferredFromTheCompilerName)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString bin = QDir(directory.path()).filePath(QStringLiteral("bin"));
    ASSERT_TRUE(QDir().mkpath(bin));
    for (const QString& name : {QStringLiteral("aarch64-linux-gnu-gcc"),
                                QStringLiteral("aarch64-linux-gnu-g++"),
                                QStringLiteral("aarch64-linux-gnu-ar")}) {
        QFile file(QDir(bin).filePath(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\n");
    }
    const auto toolchains =
        Build::inferToolchains(QDir(bin).filePath(QStringLiteral("aarch64-linux-gnu-g++")));
    ASSERT_EQ(toolchains.size(), 1);
    EXPECT_EQ(toolchains.first().systemName, QStringLiteral("Linux"));
    EXPECT_EQ(toolchains.first().processor, QStringLiteral("aarch64"));
    EXPECT_TRUE(toolchains.first().ar.endsWith(QLatin1String("aarch64-linux-gnu-ar")));
}

TEST(CrossCompile, TheToolchainFileIsWrittenAndRefusesAnInvalidSetup)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString path = QDir(directory.path()).filePath(QStringLiteral("tc/cmake.cmake"));
    Build::CrossToolchain good;
    good.systemName = QStringLiteral("Linux");
    good.cCompiler = QStringLiteral("/bin/gcc");
    good.cxxCompiler = QStringLiteral("/bin/g++");
    QString error;
    EXPECT_TRUE(Build::writeToolchainFile(path, good, &error)) << error.toStdString();
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_TRUE(file.readAll().contains("CMAKE_SYSTEM_NAME"));

    Build::CrossToolchain bad;
    QString badError;
    EXPECT_FALSE(Build::writeToolchainFile(path, bad, &badError));
    EXPECT_FALSE(badError.isEmpty());
}

TEST(BuildTargets, HelpOutputIsParsedWithoutTheGeneratorsOwnTargets)
{
    const QString help =
        "The following are some of the valid targets for this Makefile:\n"
        "... all (the default if no target is provided)\n"
        "... clean\n"
        "... mylib\n"
        "... app\n";
    EXPECT_EQ(Build::parseTargetList(help),
              (QStringList{QStringLiteral("mylib"), QStringLiteral("app")}));
}

TEST(BuildTargets, TheParallelAndConfigFlagsMatchTheGenerator)
{
    EXPECT_EQ(Build::parallelArguments(true, 8),
              (QStringList{QStringLiteral("--parallel"), QStringLiteral("8")}));
    EXPECT_TRUE(Build::configArguments(QStringLiteral("Debug"), false).isEmpty());
    EXPECT_EQ(Build::configArguments(QStringLiteral("Debug"), true).size(), 2);
    EXPECT_EQ(Build::buildTargetArguments(QStringLiteral("app"), true),
              QStringList{QStringLiteral("/t:app")});
}

TEST(BuildTargets, ALogFileNameIsSafeOnEveryPlatform)
{
    const QString name = Build::buildLogFileName(
        QStringLiteral("a/b:c"), QStringLiteral("2024-01-01 12:00:00"));
    EXPECT_FALSE(name.contains(QLatin1Char('/')));
    EXPECT_FALSE(name.contains(QLatin1Char(':')));
    EXPECT_TRUE(name.endsWith(QLatin1String(".log")));
}

TEST(CompileCommands, TheEntryForAFileIsFoundByAbsolutePathOrBasename)
{
    const QString json = QStringLiteral(R"([
      {"directory": "/tmp/build", "file": "/src/a.cpp",
       "command": "clang++ -std=c++20 -DFOO -c /src/a.cpp"}
    ])");
    const auto entry = Build::compileCommandFor(json, QStringLiteral("/src/a.cpp"));
    ASSERT_TRUE(entry.valid);
    EXPECT_TRUE(entry.command.contains(QLatin1String("-DFOO")));
    const auto byName = Build::compileCommandFor(json, QStringLiteral("/other/dir/a.cpp"));
    EXPECT_TRUE(byName.valid);
    const auto missing = Build::compileCommandFor(json, QStringLiteral("/src/b.cpp"));
    EXPECT_FALSE(missing.valid);
}

TEST(CompileCommands, AnArgumentsArrayIsTurnedBackIntoText)
{
    const QString json = QStringLiteral(R"([
      {"directory": "/tmp", "file": "a.cpp",
       "arguments": ["clang++", "-I/opt/x", "-c", "a.cpp"]}
    ])");
    const auto entry = Build::compileCommandFor(json, QStringLiteral("/tmp/a.cpp"));
    ASSERT_TRUE(entry.valid);
    EXPECT_EQ(Build::compileCommandText(entry),
              QStringLiteral("clang++ -I/opt/x -c a.cpp"));
}

TEST(BuildLauncher, CcacheArgumentsRouteBothLanguages)
{
    Build::CompilerLauncher launcher;
    launcher.program = QStringLiteral("ccache");
    launcher.path = QStringLiteral("/usr/bin/ccache");
    launcher.found = true;
    const auto arguments = Build::launcherArguments(launcher);
    ASSERT_EQ(arguments.size(), 2);
    EXPECT_TRUE(arguments.at(0).startsWith(QLatin1String("-DCMAKE_C_COMPILER_LAUNCHER=")));

    const auto environment = Build::launcherEnvironment(
        launcher, {QStringLiteral("/tmp/ccache"), QStringLiteral("5G")});
    ASSERT_EQ(environment.size(), 2);
    EXPECT_EQ(environment.at(0).first, QStringLiteral("CCACHE_DIR"));
}

TEST(BuildLauncher, TheHitRateIsReadFromEitherTool)
{
    EXPECT_EQ(Build::parseLauncherHitRate(
                  QStringLiteral("cache hit rate  87.50 %")),
              QStringLiteral("87.50%"));
    EXPECT_EQ(Build::parseLauncherHitRate(
                  QStringLiteral("Compile requests executed  10\nCache hits  9\n")),
              QStringLiteral("90.0%"));
    EXPECT_TRUE(Build::parseLauncherHitRate(QStringLiteral("nothing here")).isEmpty());
}

// ---------------------------------------------------------------------------
// Documentation catalogue
// ---------------------------------------------------------------------------

TEST(Docs, AKeywordGetsItsSummaryAndTheKeywordPage)
{
    const auto entry = Docs::quickHelp(QStringLiteral("constexpr"));
    EXPECT_TRUE(entry.summary.contains(QLatin1String("constant expression")));
    EXPECT_TRUE(entry.url.endsWith(QLatin1String("/keyword/constexpr")));
}

TEST(Docs, AnUnknownSymbolGetsASearchUrlAndNoInventedSummary)
{
    const auto entry = Docs::quickHelp(QStringLiteral("myProjectThing"));
    EXPECT_TRUE(entry.summary.isEmpty());
    EXPECT_TRUE(entry.url.contains(QLatin1String("search=")));
}

TEST(Docs, AStandardLibraryNameMapsToItsPage)
{
    EXPECT_EQ(Docs::cppreferenceUrl(QStringLiteral("std::vector<int>")),
              QStringLiteral("https://en.cppreference.com/w/cpp/container/vector"));
    const auto entry = Docs::quickHelp(QStringLiteral("std::unique_ptr"));
    EXPECT_EQ(entry.kind, QStringLiteral("library"));
}

TEST(Docs, SearchCoversKeywordsStandardsAndLibraries)
{
    // A standard-library name is not a keyword, so it must come back from the
    // catalogue rather than from the keyword table — the two used to be
    // conflated and searching for `vector` found nothing.
    const auto hits = Docs::search({}, QStringLiteral("vector"));
    ASSERT_FALSE(hits.isEmpty());
    bool sawLibrary = false;
    for (const auto& entry : hits) {
        if (entry.kind == QLatin1String("library") && entry.title == QStringLiteral("std::vector")) {
            sawLibrary = true;
        }
    }
    EXPECT_TRUE(sawLibrary) << "expected std::vector in the search index";
    EXPECT_TRUE(Docs::isKnownKeyword(QStringLiteral("noexcept")));
    EXPECT_FALSE(Docs::isKnownKeyword(QStringLiteral("vector")));
}

TEST(Docs, TheIndexIsNotEmptyAndNamesTheStandards)
{
    const auto index = Docs::search({}, QString());
    EXPECT_GE(index.size(), 8);
    bool sawStandard = false;
    for (const auto& entry : index) {
        if (entry.kind == QLatin1String("standard")) {
            sawStandard = true;
        }
    }
    EXPECT_TRUE(sawStandard);
}

// ---------------------------------------------------------------------------
// Qt previews
// ---------------------------------------------------------------------------

TEST(Qt, AUiFormRendersItsWidgetsAndConnections)
{
    const QString xml = QStringLiteral(R"(<?xml version="1.0"?>
<ui version="4.0">
 <class>MainWindow</class>
 <widget class="QMainWindow" name="MainWindow">
  <widget class="QWidget" name="centralwidget">
   <widget class="QPushButton" name="okButton">
    <property name="geometry"><rect><x>10</x><y>20</y><width>80</width><height>24</height></rect></property>
    <property name="text"><string>OK</string></property>
   </widget>
  </widget>
 </widget>
 <connections>
  <connection><sender>okButton</sender><signal>clicked()</signal><receiver>MainWindow</receiver><slot>onOk()</slot></connection>
 </connections>
</ui>)");
    const auto form = parseUiForm(xml);
    ASSERT_TRUE(form.valid);
    const QString html = QtNs::uiPreviewHtml(form, QStringLiteral("mainwindow.ui"));
    EXPECT_TRUE(html.contains(QLatin1String("MainWindow")));
    EXPECT_TRUE(html.contains(QLatin1String("OK")));
    EXPECT_TRUE(html.contains(QLatin1String("onOk()")));
    EXPECT_TRUE(html.contains(QLatin1String("left:10px")));
}

TEST(Qt, AResourceEntryThatIsMissingIsMarked)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString xml = QStringLiteral(R"(<RCC>
  <qresource prefix="/icons">
    <file>open.png</file>
    <file alias="close.png">close_icon.png</file>
  </qresource>
</RCC>)");
    const auto qrc = parseQrc(xml, QDir(directory.path()).filePath(QStringLiteral("r.qrc")));
    ASSERT_TRUE(qrc.valid);
    const QString html = QtNs::qrcPreviewHtml(qrc, QStringLiteral("r.qrc"));
    EXPECT_TRUE(html.contains(QLatin1String(":/icons/open.png")));
    EXPECT_TRUE(html.contains(QLatin1String("missing")));
}

TEST(Qt, TheQmlTreeShowsTypesAndIds)
{
    const QString qml = QStringLiteral(
        "import QtQuick 2.0\n"
        "Item {\n"
        "    id: root\n"
        "    width: 100\n"
        "    Rectangle {\n"
        "        id: box\n"
        "        color: \"red\"\n"
        "    }\n"
        "}\n");
    const QStringList tree = QtNs::qmlObjectTree(qml);
    ASSERT_EQ(tree.size(), 2) << tree.join(QLatin1Char('|')).toStdString();
    EXPECT_EQ(tree.at(0), QStringLiteral("Item  (id: root)"));
    // Nested objects are indented by two spaces per level, and the id is the
    // only property shown — "width: 100" is configuration, not structure.
    EXPECT_EQ(tree.at(1), QStringLiteral("  Rectangle  (id: box)"));
}

TEST(Qt, HasPreviewCoversTheFourExtensions)
{
    EXPECT_TRUE(QtNs::hasPreview(QStringLiteral("a.ui")));
    EXPECT_TRUE(QtNs::hasPreview(QStringLiteral("a.qrc")));
    EXPECT_TRUE(QtNs::hasPreview(QStringLiteral("a.qml")));
    EXPECT_FALSE(QtNs::hasPreview(QStringLiteral("a.cpp")));
}

// ---------------------------------------------------------------------------
// Class view — the panel-facing details
// ---------------------------------------------------------------------------

TEST(ClassView, ADocumentWithNoTypesIsAnEmptyViewAndNotAnError)
{
    // A header of free functions and constants is legitimate C++; the panel
    // must say "nothing here", not fail to open.
    const QJsonArray symbols = jsonArrayOf(R"([
        {"name": "run", "kind": 12,
         "selectionRange": {"start": {"line": 0, "character": 5}, "end": {"line": 0, "character": 8}}},
        {"name": "kMax", "kind": 14,
         "selectionRange": {"start": {"line": 1, "character": 6}, "end": {"line": 1, "character": 10}}}
    ])");
    EXPECT_TRUE(Lsp::buildClassView(symbols).isEmpty());
    // ... and neither is an empty answer or a null one.
    EXPECT_TRUE(Lsp::buildClassView(QJsonValue(QJsonArray())).isEmpty());
    EXPECT_TRUE(Lsp::buildClassView(QJsonValue()).isEmpty());
}

TEST(ClassView, EveryTypeKindIsAcceptedAndAnonymousTypesAreDropped)
{
    const QJsonArray symbols = jsonArrayOf(R"([
        {"name": "C", "kind": 5, "selectionRange": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": 1}}},
        {"name": "S", "kind": 23, "selectionRange": {"start": {"line": 2, "character": 0}, "end": {"line": 2, "character": 1}}},
        {"name": "I", "kind": 11, "selectionRange": {"start": {"line": 3, "character": 0}, "end": {"line": 3, "character": 1}}},
        {"name": "O", "kind": 19, "selectionRange": {"start": {"line": 4, "character": 0}, "end": {"line": 4, "character": 1}}},
        {"name": "M", "kind": 6, "selectionRange": {"start": {"line": 5, "character": 0}, "end": {"line": 5, "character": 1}}},
        {"name": "", "kind": 5, "selectionRange": {"start": {"line": 6, "character": 0}, "end": {"line": 6, "character": 1}}}
    ])");
    const auto classes = Lsp::buildClassView(symbols);
    // class, struct, interface, object — but not the method, and not the
    // nameless type (a row with no label cannot be picked from a list).
    ASSERT_EQ(classes.size(), 4);
    EXPECT_EQ(classes.at(0).name, QStringLiteral("C"));
    EXPECT_EQ(classes.at(1).name, QStringLiteral("S"));
    EXPECT_EQ(classes.at(2).name, QStringLiteral("I"));
    EXPECT_EQ(classes.at(3).name, QStringLiteral("O"));
}

TEST(ClassView, MemberPositionsSurviveSoThePanelCanFlashToThem)
{
    // The panel jumps to a member on double click, using exactly these
    // numbers. If they came from `range` instead of `selectionRange` the
    // caret would land on the first line of the body, not on the name.
    const QJsonArray symbols = jsonArrayOf(R"cpp([{
        "name": "Box", "kind": 23, "detail": "struct Box",
        "range": {"start": {"line": 0, "character": 0}, "end": {"line": 40, "character": 1}},
        "selectionRange": {"start": {"line": 0, "character": 7}, "end": {"line": 0, "character": 10}},
        "children": [
            {"name": "width", "kind": 8, "detail": "int",
             "range": {"start": {"line": 12, "character": 4}, "end": {"line": 25, "character": 5}},
             "selectionRange": {"start": {"line": 12, "character": 8}, "end": {"line": 12, "character": 13}}},
            {"name": "resize", "kind": 6, "detail": "void resize(int)",
             "selectionRange": {"start": {"line": 20, "character": 9}, "end": {"line": 20, "character": 15}}}
        ]
    }])cpp");
    const auto classes = Lsp::buildClassView(symbols);
    ASSERT_EQ(classes.size(), 1);
    const auto& box = classes.first();
    EXPECT_EQ(box.line, 0);
    EXPECT_EQ(box.column, 7);
    ASSERT_EQ(box.members.size(), 2);
    EXPECT_EQ(box.members.at(0).name, QStringLiteral("width"));
    EXPECT_EQ(box.members.at(0).detail, QStringLiteral("int"));
    EXPECT_EQ(box.members.at(0).line, 12);
    EXPECT_EQ(box.members.at(0).column, 8);
    EXPECT_EQ(box.members.at(1).name, QStringLiteral("resize"));
    EXPECT_EQ(box.members.at(1).kind, Lsp::SymbolKind::Method);
}

TEST(ClassView, EnumeratorsAreMembersOfAnEnumButAnEnumIsNotAClass)
{
    // clangd reports the enumerators as children of the enum, and the enum
    // itself as SymbolKind::Enum. The class browser shows classes, so the
    // enum is not a row — but an enum *inside* a class arrives as a member
    // and must keep its enumerators reachable through that member.
    const QJsonArray symbols = jsonArrayOf(R"([{
        "name": "Mode", "kind": 10,
        "selectionRange": {"start": {"line": 0, "character": 6}, "end": {"line": 0, "character": 10}},
        "children": [
            {"name": "Fast", "kind": 22,
             "selectionRange": {"start": {"line": 1, "character": 4}, "end": {"line": 1, "character": 8}}}
        ]
    }, {
        "name": "Runner", "kind": 5, "detail": "class Runner",
        "selectionRange": {"start": {"line": 5, "character": 6}, "end": {"line": 5, "character": 12}},
        "children": [
            {"name": "mode", "kind": 8, "detail": "Mode",
             "selectionRange": {"start": {"line": 6, "character": 9}, "end": {"line": 6, "character": 13}}}
        ]
    }])");
    const auto classes = Lsp::buildClassView(symbols);
    ASSERT_EQ(classes.size(), 1);
    EXPECT_EQ(classes.first().name, QStringLiteral("Runner"));
    ASSERT_EQ(classes.first().members.size(), 1);
    EXPECT_EQ(classes.first().members.first().name, QStringLiteral("mode"));
}

TEST(ClassView, MultipleClassesInOneFileKeepTheirOwnSourceOrder)
{
    // The list is read top to bottom, so it must be in source order — not
    // sorted by name, and not grouped by kind. This is the one thing a
    // reader can rely on without reading the labels.
    const QJsonArray symbols = jsonArrayOf(R"([
        {"name": "Zebra", "kind": 5, "selectionRange": {"start": {"line": 50, "character": 6}, "end": {"line": 50, "character": 11}}},
        {"name": "Apple", "kind": 23, "selectionRange": {"start": {"line": 3, "character": 7}, "end": {"line": 3, "character": 12}}},
        {"name": "Mango", "kind": 5, "selectionRange": {"start": {"line": 20, "character": 6}, "end": {"line": 20, "character": 11}}}
    ])");
    const auto classes = Lsp::buildClassView(symbols);
    ASSERT_EQ(classes.size(), 3);
    // The server already sends source order; the point of the test is that
    // buildClassView preserves it instead of reordering for display.
    EXPECT_EQ(classes.at(0).name, QStringLiteral("Zebra"));
    EXPECT_EQ(classes.at(1).name, QStringLiteral("Apple"));
    EXPECT_EQ(classes.at(2).name, QStringLiteral("Mango"));
}

// ---------------------------------------------------------------------------
// Call graph — the parts the graph window relies on
// ---------------------------------------------------------------------------

TEST(CallGraph, TheEmptyGraphIsStillValidDot)
{
    // The window offers "Copy DOT" on every finished walk, including trivial
    // ones, so the empty graph must still be a *valid* DOT document.
    Lsp::CallGraph empty;
    const QString dot = empty.toDot(QStringLiteral("nothing"));
    EXPECT_TRUE(dot.startsWith(QLatin1String("digraph \"nothing\" {")));
    EXPECT_TRUE(dot.trimmed().endsWith(QLatin1Char('}')));
    EXPECT_EQ(dot.count(QLatin1String("->")), 0);
}

TEST(CallGraph, DotLabelsEscapeQuotesAndBackslashes)
{
    // A C++ function name cannot contain a quote, but a detail string can
    // (e.g. operator""_km). Unescaped it would break the whole document,
    // and Graphviz would reject it with a parse error pointing at nothing
    // useful.
    Lsp::HierarchyItem root;
    root.name = QStringLiteral("operator\"\"_km");
    root.uri = QStringLiteral("file:///src/units.cpp");
    root.selectionRange.startLine = 4;

    Lsp::CallGraph graph;
    graph.addNode(root);
    const QString dot = graph.toDot();
    EXPECT_TRUE(dot.contains(QLatin1String("operator\\\"\\\"_km")));
    // Every label opened is closed: an odd number of unescaped quotes on a
    // line means the escaping failed somewhere.
    for (const QString& line : dot.split(QLatin1Char('\n'))) {
        if (!line.contains(QLatin1String("label="))) {
            continue;
        }
        int quotes = 0;
        for (int i = 0; i < line.size(); ++i) {
            if (line.at(i) == QLatin1Char('"')
                && (i == 0 || line.at(i - 1) != QLatin1Char('\\'))) {
                ++quotes;
            }
        }
        EXPECT_EQ(quotes % 2, 0) << line.toStdString();
    }
}

TEST(CallGraph, ADiamondIsNotACycle)
{
    // a calls b and c; both call d. The tree text repeats d under each, but
    // `hasCycleFrom` must stay false — a shared callee is not recursion, and
    // telling the user their code recurses when it does not is the worse of
    // the two mistakes.
    Lsp::HierarchyItem a;
    a.name = QStringLiteral("a");
    a.uri = QStringLiteral("file:///a.cpp");
    a.selectionRange.startLine = 0;
    Lsp::HierarchyItem b = a;
    b.name = QStringLiteral("b");
    b.selectionRange.startLine = 10;
    Lsp::HierarchyItem c = a;
    c.name = QStringLiteral("c");
    c.selectionRange.startLine = 20;
    Lsp::HierarchyItem d = a;
    d.name = QStringLiteral("d");
    d.selectionRange.startLine = 30;

    Lsp::CallGraph graph;
    graph.addCall(a, b);
    graph.addCall(a, c);
    graph.addCall(b, d);
    graph.addCall(c, d);

    EXPECT_FALSE(graph.hasCycleFrom(Lsp::nodeKey(a)));
    const QString tree = graph.toTreeText(Lsp::nodeKey(a));
    // d appears twice in the *text* (once per path) and once in the graph.
    EXPECT_EQ(tree.count(QLatin1String("d (")), 2);
    EXPECT_EQ(graph.nodes.size(), 4);
    EXPECT_FALSE(tree.contains(QLatin1String("[recursive]")));
}

TEST(CallGraph, ARepeatedCallIsOneEdgeAndTwoOverloadsAreTwoNodes)
{
    Lsp::HierarchyItem run;
    run.name = QStringLiteral("run");
    run.uri = QStringLiteral("file:///r.cpp");
    run.selectionRange.startLine = 0;
    Lsp::HierarchyItem otherSameLine = run;
    otherSameLine.selectionRange.startColumn = 12;  // overload on one line

    Lsp::CallGraph graph;
    graph.addCall(run, run);
    graph.addCall(run, run);  // the same edge twice
    EXPECT_EQ(graph.edges.size(), 1);

    Lsp::HierarchyItem overload;
    overload.name = QStringLiteral("run");
    overload.uri = QStringLiteral("file:///r.cpp");
    overload.selectionRange.startLine = 40;
    graph.addCall(run, overload);
    graph.addNode(otherSameLine);
    // Two functions called run() at different lines, plus the overloaded one
    // on the same line as the first: three nodes, not one.
    EXPECT_EQ(graph.nodes.size(), 3);
}

TEST(CallGraph, AMissingRootProducesNoTreeInsteadOfTheWholeGraph)
{
    // toTreeText is keyed by the root; an unknown root must not fall back to
    // "print whatever is there", which would show a graph with the wrong
    // function at the top.
    Lsp::HierarchyItem a;
    a.name = QStringLiteral("a");
    a.uri = QStringLiteral("file:///a.cpp");
    Lsp::CallGraph graph;
    graph.addNode(a);
    EXPECT_TRUE(graph.toTreeText(QStringLiteral("file:///nowhere.cpp#0:0")).isEmpty());
}

// ---------------------------------------------------------------------------
// Hover: three shapes in, one Markdown out, plus the type hint
// ---------------------------------------------------------------------------

TEST(Hover, APlainStringAnswerIsMarkdownWithNoCodeBlock)
{
    const Lsp::HoverInfo hover = Lsp::parseHover(QJsonObject{
        {"contents", QStringLiteral("no answer here")}});
    EXPECT_EQ(hover.markdown, QStringLiteral("no answer here"));
    EXPECT_TRUE(hover.codeBlocks.isEmpty());
    EXPECT_TRUE(hover.primaryCode().isEmpty());
    EXPECT_FALSE(hover.hasRange);
}

TEST(Hover, AMarkedStringObjectKeepsItsLanguageTag)
{
    // The protocol's second form: {language, value} rather than a bare
    // string. The language tag is what separates a declaration from prose.
    const Lsp::HoverInfo hover = Lsp::parseHover(QJsonObject{
        {"contents", QJsonObject{{"language", "cpp"},
                                 {"value", QStringLiteral("int x")}}}});
    ASSERT_EQ(hover.codeBlocks.size(), 1);
    EXPECT_EQ(hover.codeBlocks.first().first, QStringLiteral("cpp"));
    EXPECT_EQ(hover.primaryCode(), QStringLiteral("int x"));
}

TEST(Hover, AnArrayOfMixedShapesIsOneMarkdownWithBothParts)
{
    // This is exactly what clangd sends: a fenced declaration, then the
    // comment as a plain string. Reading only the first element loses the
    // documentation; reading only the string loses the signature.
    const Lsp::HoverInfo hover = Lsp::parseHover(QJsonObject{
        {"contents", QJsonArray{
             QJsonObject{{"language", "cpp"}, {"value", QStringLiteral("void run()")}},
             QStringLiteral("Runs the thing."),
         }}});
    ASSERT_EQ(hover.codeBlocks.size(), 1);
    EXPECT_TRUE(hover.markdown.contains(QLatin1String("void run()")));
    EXPECT_TRUE(hover.markdown.contains(QLatin1String("Runs the thing.")));
    EXPECT_TRUE(hover.hasDocumentation());
}

TEST(Hover, AFencedStringInPlainTextIsStillASignature)
{
    // Some servers (and clangd under `contentFormat: plaintext`) put the
    // fence inside the string instead of in a MarkedString. Dropping the
    // fence is what stops the tooltip from showing three backticks.
    const Lsp::HoverInfo hover = Lsp::parseHover(QJsonObject{
        {"contents", QStringLiteral("```cpp\nstd::vector<int> v\n```")}});
    ASSERT_EQ(hover.codeBlocks.size(), 1);
    EXPECT_EQ(hover.codeBlocks.first().first, QStringLiteral("cpp"));
    EXPECT_EQ(hover.primaryCode(), QStringLiteral("std::vector<int> v"));
    EXPECT_FALSE(hover.markdown.contains(QLatin1String("```")));
}

TEST(Hover, TheRangeIsReadWhenTheServerNarrowsIt)
{
    const Lsp::HoverInfo hover = Lsp::parseHover(QJsonObject{
        {"contents", QStringLiteral("int x")},
        {"range", QJsonObject{
             {"start", QJsonObject{{"line", 4}, {"character", 8}}},
             {"end", QJsonObject{{"line", 4}, {"character", 9}}}}}});
    EXPECT_TRUE(hover.hasRange);
    EXPECT_EQ(hover.range.startLine, 4);
    EXPECT_EQ(hover.range.startColumn, 8);

    // A null result is the normal "nothing here", not a malformed answer.
    const Lsp::HoverInfo empty = Lsp::parseHover(QJsonValue());
    EXPECT_TRUE(empty.isEmpty());
    EXPECT_FALSE(empty.hasRange);
}

TEST(TypeHint, AVariableDeclarationYieldsItsType)
{
    Lsp::HoverInfo hover;
    hover.codeBlocks.append({"cpp", QStringLiteral("Widget w")});
    const Lsp::TypeHint hint = Lsp::typeHintFromHover(hover, QStringLiteral("w"));
    ASSERT_TRUE(hint.isValid());
    EXPECT_EQ(hint.type, QStringLiteral("Widget"));
    EXPECT_EQ(hint.variable, QStringLiteral("w"));
}

TEST(TypeHint, PointerAndReferenceDeclaratorsKeepTheStar)
{
    // `int *p` → `int`, because the star belongs to the declarator in C++
    // and a hint that says `int *` for an `int` pointer is still readable —
    // but `int * *` is not, so the stars are stripped with the name.
    Lsp::HoverInfo pointer;
    pointer.codeBlocks.append({"cpp", QStringLiteral("const char *name")});
    EXPECT_EQ(Lsp::typeHintFromHover(pointer, QStringLiteral("name")).type,
              QStringLiteral("const char"));

    Lsp::HoverInfo reference;
    reference.codeBlocks.append({"cpp", QStringLiteral("Widget &ref")});
    EXPECT_EQ(Lsp::typeHintFromHover(reference, QStringLiteral("ref")).type,
              QStringLiteral("Widget"));
}

TEST(TypeHint, AFunctionOrTypeDeclarationYieldsNoHint)
{
    // A hint that repeats the signature is noise. `void run()` has nothing
    // to strip, and neither has a bare type spelling.
    Lsp::HoverInfo function;
    function.codeBlocks.append({"cpp", QStringLiteral("void run()")});
    EXPECT_FALSE(
        Lsp::typeHintFromHover(function, QStringLiteral("run")).isValid());

    Lsp::HoverInfo type;
    type.codeBlocks.append({"cpp", QStringLiteral("class Widget")});
    EXPECT_FALSE(
        Lsp::typeHintFromHover(type, QStringLiteral("Widget")).isValid());
}

TEST(TypeHint, AnInitialiserOrMultiLineDeclarationYieldsNoHint)
{
    // `auto x = make();` hovers as the deduced declaration in clangd, but a
    // server that answers with the initialiser expression must not produce
    // "auto x = make(" as a type.
    Lsp::HoverInfo withCall;
    withCall.codeBlocks.append(
        {"cpp", QStringLiteral("Widget w = make_widget()")});
    EXPECT_FALSE(
        Lsp::typeHintFromHover(withCall, QStringLiteral("w")).isValid());

    // A trailing comment is not part of the type.
    Lsp::HoverInfo commented;
    commented.codeBlocks.append(
        {"cpp", QStringLiteral("int count // how many")});
    EXPECT_EQ(Lsp::typeHintFromHover(commented, QStringLiteral("count")).type,
              QStringLiteral("int"));
}

TEST(TypeHint, TemplateTypesSurviveIntact)
{
    // The reason the hint exists: `std::map<std::string, Widget>` is what
    // `auto` hid, and a naive "take the first word" rule would return
    // `std::map<std::string,`.
    Lsp::HoverInfo hover;
    hover.codeBlocks.append(
        {"cpp", QStringLiteral("std::map<std::string, Widget> byName")});
    const Lsp::TypeHint hint =
        Lsp::typeHintFromHover(hover, QStringLiteral("byName"));
    ASSERT_TRUE(hint.isValid());
    EXPECT_EQ(hint.type, QStringLiteral("std::map<std::string, Widget>"));
}

TEST(Hover, ACommentBlockIsDocumentationNotASignature)
{
    Lsp::HoverInfo commentOnly;
    commentOnly.codeBlocks.append(
        {"cpp", QStringLiteral("// Returns the widget's size.")});
    EXPECT_TRUE(commentOnly.hasDocumentation());
    EXPECT_TRUE(commentOnly.primaryCode().isEmpty());

    Lsp::HoverInfo prose;
    prose.markdown = QStringLiteral("line one\nline two");
    EXPECT_TRUE(prose.hasDocumentation());
}

// ---------------------------------------------------------------------------
// External tools: the template is the whole contract
// ---------------------------------------------------------------------------

namespace {

namespace Ext = LightCpp::Workbench::ExternalTools;

Ext::Context toolContext()
{
    Ext::Context context;
    context.file = QStringLiteral("/home/me/My Code/main.cpp");
    context.projectRoot = QStringLiteral("/home/me/My Code");
    context.buildDir = QStringLiteral("/home/me/My Code/build");
    context.selection = QStringLiteral("Widget");
    return context;
}

}  // namespace

TEST(ExternalTools, APathWithASpaceIsQuotedForTheShell)
{
    // The reason quoting exists. Unquoted, `clang-format /home/me/My Code/x`
    // is two arguments and clang-format reports a file that does not exist.
    const QString expanded =
        Ext::expand(QStringLiteral("--dry-run ${file}"), toolContext(),
                    Ext::Shell::Posix);
    EXPECT_EQ(expanded,
              QStringLiteral("--dry-run '/home/me/My Code/main.cpp'"));
}

TEST(ExternalTools, APathThatNeedsNoQuotingIsLeftAlone)
{
    // Quoting everything would be correct but unreadable in the command echo,
    // and a path without spaces is the common case.
    Ext::Context context;
    context.file = QStringLiteral("/src/a.cpp");
    EXPECT_EQ(Ext::expand(QStringLiteral("${file}"), context, Ext::Shell::Posix),
              QStringLiteral("/src/a.cpp"));
}

TEST(ExternalTools, ASingleQuoteInAPathIsEscapedNotDropped)
{
    // `it's.cpp` is a legal file name. Inside single quotes it would end the
    // quoting and the rest of the command would be interpreted.
    Ext::Context context;
    context.file = QStringLiteral("/tmp/it's.cpp");
    const QString expanded =
        Ext::expand(QStringLiteral("${file}"), context, Ext::Shell::Posix);
    EXPECT_EQ(expanded, QStringLiteral("'/tmp/it'\\''s.cpp'"));
    // And the result really is one shell word: count unescaped quotes.
    int quotes = 0;
    for (int i = 0; i < expanded.size(); ++i) {
        if (expanded.at(i) == QLatin1Char('\'')
            && (i == 0 || expanded.at(i - 1) != QLatin1Char('\\'))) {
            ++quotes;
        }
    }
    EXPECT_EQ(quotes % 2, 0);
}

TEST(ExternalTools, WindowsQuotingDoublesPercentAndQuote)
{
    Ext::Context context;
    context.file = QStringLiteral("C:\\Users\\me\\100% done\\a.cpp");
    const QString expanded =
        Ext::expand(QStringLiteral("${file}"), context, Ext::Shell::Windows);
    // A single `%` in cmd.exe starts a variable expansion; `%%` is a literal.
    EXPECT_TRUE(expanded.contains(QLatin1String("100%% done"))) << expanded.toStdString();
    EXPECT_TRUE(expanded.startsWith(QLatin1Char('"')));
}

TEST(ExternalTools, UnknownPlaceholdersAndShellVariablesPassThrough)
{
    // A template is often a shell fragment: `$HOME`, `$?`, `$$` all mean
    // something to the shell, and eating them would break the tool.
    const QString expanded = Ext::expand(
        QStringLiteral("echo $HOME && cat ${file} | grep ${unknown}"),
        toolContext(), Ext::Shell::Posix);
    EXPECT_TRUE(expanded.contains(QLatin1String("$HOME")));
    EXPECT_TRUE(expanded.contains(QLatin1String("${unknown}")));
    EXPECT_TRUE(expanded.contains(QLatin1String("main.cpp")));
}

TEST(ExternalTools, AMissingValueIsReportedInsteadOfExpandingToNothing)
{
    // `grep "" file` is not an error the user can act on. The caller is told
    // which placeholder was empty so it can refuse to run.
    Ext::Context context;  // no file, no project, no selection
    QStringList missing;
    const QString expanded =
        Ext::expand(QStringLiteral("${file} ${selection}"), context,
                    Ext::Shell::Posix, &missing);
    EXPECT_EQ(expanded.trimmed(), QString());
    EXPECT_TRUE(missing.contains(QStringLiteral("${file}")));
    EXPECT_TRUE(missing.contains(QStringLiteral("${selection}")));
    // And a placeholder that is not used is not reported.
    EXPECT_FALSE(missing.contains(QStringLiteral("${buildDir}")));
}

TEST(ExternalTools, TheSelectionIsNotQuotedBecauseItIsUsuallySeveralWords)
{
    Ext::Context context;
    context.selection = QStringLiteral("a.cpp b.cpp");
    EXPECT_EQ(Ext::expand(QStringLiteral("${selection}"), context,
                          Ext::Shell::Posix),
              QStringLiteral("a.cpp b.cpp"));
}

TEST(ExternalTools, DerivationsUseThePathsTheyAreGiven)
{
    Ext::Context context;
    context.file = QStringLiteral("/p/src/deep/a.cpp");
    EXPECT_EQ(Ext::expand(QStringLiteral("${fileDir}"), context, Ext::Shell::Posix),
              QStringLiteral("/p/src/deep"));
    EXPECT_EQ(Ext::expand(QStringLiteral("${fileName}"), context, Ext::Shell::Posix),
              QStringLiteral("a.cpp"));
    // A bare name has no directory; "." would be a working directory the
    // caller did not ask for.
    Ext::Context bare;
    bare.file = QStringLiteral("a.cpp");
    EXPECT_TRUE(Ext::directoryOf(bare.file).isEmpty());
}

TEST(ExternalTools, IdsAreStableAndUnique)
{
    EXPECT_EQ(Ext::makeId(QStringLiteral("Clang-format check"), {}),
              QStringLiteral("clang-format-check"));
    // Same name twice must not collide: the id is how "the one I ran last"
    // is remembered.
    const QStringList existing{QStringLiteral("clang-format-check")};
    EXPECT_EQ(Ext::makeId(QStringLiteral("Clang-format check"), existing),
              QStringLiteral("clang-format-check-2"));
    // A name with no alphanumerics still gets a usable id.
    EXPECT_EQ(Ext::makeId(QStringLiteral("***"), {}), QStringLiteral("tool"));
}

TEST(ExternalTools, WorkingDirectoryFallsBackInAUsefulOrder)
{
    const Ext::Context context = toolContext();
    // The tool's own template wins.
    EXPECT_EQ(Ext::workingDirectoryFor(QStringLiteral("${buildDir}"), context,
                                       Ext::Shell::Posix),
              context.buildDir);
    // Then the project root.
    EXPECT_EQ(Ext::workingDirectoryFor(QString(), context, Ext::Shell::Posix),
              context.projectRoot);
    // Then the file's directory.
    Ext::Context noProject = context;
    noProject.projectRoot.clear();
    EXPECT_EQ(Ext::workingDirectoryFor(QString(), noProject, Ext::Shell::Posix),
              QStringLiteral("/home/me/My Code"));
}

TEST(ExternalTools, DescribeRefusesAToolWithNoCommand)
{
    // An empty description is the caller's signal that the tool cannot run.
    EXPECT_TRUE(Ext::describe(QString(), QStringLiteral("--x")).isEmpty());
    EXPECT_TRUE(Ext::describe(QStringLiteral("   "), {}).isEmpty());
    EXPECT_EQ(Ext::describe(QStringLiteral("clang-format"),
                            QStringLiteral("--dry-run   ${file}"), 200),
              QStringLiteral("clang-format --dry-run ${file}"));
    // Long command lines are trimmed for a menu entry.
    const QString longText =
        Ext::describe(QStringLiteral("tool"),
                      QStringLiteral("--aaaaaaaa --bbbbbbbb --cccccccc"), 20);
    EXPECT_EQ(longText.size(), 20);
    EXPECT_TRUE(longText.endsWith(QLatin1String("...")));
}

// ---------------------------------------------------------------------------
// Terminal screen model: the sequences that break silently
// ---------------------------------------------------------------------------

namespace {

namespace Term = LightCpp::Workbench::Terminal;

/// The first `count` screen lines, joined — the shape assertions read best in.
QString screenText(const Term::Emulator& emulator, int count = -1)
{
    const QStringList lines = emulator.screenLines();
    if (count < 0) {
        return lines.join(QLatin1Char('\n'));
    }
    return QStringList(lines.mid(0, count)).join(QLatin1Char('\n'));
}

/// Feed text and drop the update: most tests care about the resulting screen,
/// not about which rows the controller would repaint.
void type(Term::Emulator& emulator, const QString& text)
{
    const Term::EmulatorUpdate update = emulator.feedText(text);
    Q_UNUSED(update)
}

}  // namespace

TEST(TerminalModel, PlainTextLandsOnTheGridAndTheCursorFollows)
{
    Term::Emulator emulator(10, 3);
    type(emulator, QStringLiteral("hi"));
    EXPECT_EQ(emulator.currentLine(), QStringLiteral("hi"));
    EXPECT_EQ(emulator.cursorColumn(), 2);
    EXPECT_EQ(emulator.cursorRow(), 0);
}

TEST(TerminalModel, AnSgrSequenceColoursWithoutPrintingItself)
{
    // The classic bug: a parser that consumes ESC but not the parameters
    // prints "[1;32m" into the buffer. The text after it is all that may
    // appear.
    Term::Emulator emulator(20, 2);
    type(emulator, QStringLiteral("\x1b[1;32mOK\x1b[0m done"));
    EXPECT_EQ(emulator.currentLine(), QStringLiteral("OK done"));
    const auto runs = emulator.screenRuns();
    ASSERT_FALSE(runs.isEmpty());
    ASSERT_FALSE(runs.first().isEmpty());
    EXPECT_TRUE(runs.first().first().attributes.bold);
    EXPECT_TRUE(runs.first().first().attributes.fgIsPalette);
    EXPECT_EQ(runs.first().first().attributes.fg, 2u);
    // The trailing " done" is back to the default attributes, so the row has
    // two runs rather than one.
    EXPECT_EQ(runs.first().size(), 2);
    EXPECT_FALSE(runs.first().last().attributes.bold);
}

TEST(TerminalModel, TruecolorAnd256ColourBothDecode)
{
    Term::Emulator emulator(20, 1);
    type(emulator, QStringLiteral("\x1b[38;2;18;52;86mX"));
    auto runs = emulator.screenRuns();
    ASSERT_FALSE(runs.first().isEmpty());
    EXPECT_EQ(runs.first().first().attributes.fg, 0x123456u);
    EXPECT_FALSE(runs.first().first().attributes.fgIsPalette);

    Term::Emulator palette(20, 1);
    type(palette, QStringLiteral("\x1b[48;5;208mX"));
    runs = palette.screenRuns();
    ASSERT_FALSE(runs.first().isEmpty());
    EXPECT_EQ(runs.first().first().attributes.bg, 208u);
    EXPECT_TRUE(runs.first().first().attributes.bgIsPalette);
}

TEST(TerminalModel, AutowrapWrapsOnTheNextGlyphNotOnTheLastCell)
{
    // Writing exactly to the last column must not scroll: the cursor parks
    // on the last cell and only wraps when another glyph arrives. Getting
    // this wrong adds a blank line after every full-width line.
    Term::Emulator emulator(4, 3);
    type(emulator, QStringLiteral("abcd"));
    EXPECT_EQ(emulator.cursorRow(), 0);
    EXPECT_EQ(emulator.cursorColumn(), 3);
    type(emulator, QStringLiteral("e"));
    EXPECT_EQ(emulator.cursorRow(), 1);
    EXPECT_EQ(emulator.cursorColumn(), 1);
    EXPECT_EQ(screenText(emulator, 3), QStringLiteral("abcd\ne\n"));
}

TEST(TerminalModel, CarriageReturnAndLineFeedDoDifferentThings)
{
    // A shell that sends only \n moves down without returning to column 0;
    // a model that treats them as one thing produces stair-stepped output.
    Term::Emulator emulator(20, 3);
    type(emulator, QStringLiteral("abc\nXY"));
    EXPECT_EQ(emulator.screenLines().at(0), QStringLiteral("abc"));

    Term::Emulator crlf(20, 3);
    type(crlf, QStringLiteral("abc\r\nXY"));
    EXPECT_EQ(crlf.screenLines().at(1), QStringLiteral("XY"));
}

TEST(TerminalModel, CursorPositioningIsOneBased)
{
    // `CSI 2;3 H` is row 2, column 3 in terminal coordinates — the source of
    // a whole family of off-by-one bugs.
    Term::Emulator emulator(20, 5);
    type(emulator, QStringLiteral("\x1b[2;3HX"));
    EXPECT_EQ(emulator.screenLines().at(1).mid(2, 1), QStringLiteral("X"));
    EXPECT_EQ(emulator.cursorRow(), 1);
    EXPECT_EQ(emulator.cursorColumn(), 3);
}

TEST(TerminalModel, EraseInLineClearsFromTheCursorRightwards)
{
    Term::Emulator emulator(20, 2);
    type(emulator, QStringLiteral("abcdef\x1b[1;4H\x1b[K"));
    EXPECT_EQ(emulator.screenLines().at(0), QStringLiteral("abc"));

    Term::Emulator whole(20, 2);
    type(whole, QStringLiteral("abcdef\x1b[1;4H\x1b[2K"));
    EXPECT_EQ(whole.screenLines().at(0), QString());
}

TEST(TerminalModel, ScrollRegionKeepsTheStatusLineInPlace)
{
    // A full-screen app puts its status bar in a scroll region's last row and
    // expects the text above to scroll without touching it. Scrolling the
    // whole screen moves the status bar off, which is what makes a
    // mis-implemented region look broken.
    Term::Emulator emulator(10, 4);
    // Rows 1..3 scroll; row 4 is the status line.
    type(emulator, QStringLiteral("\x1b[1;3r"));
    type(emulator, QStringLiteral("\x1b[4;1HSTATUS"));
    type(emulator, QStringLiteral("\x1b[1;1Ha\r\nb\r\nc\r\nd"));
    EXPECT_EQ(emulator.screenLines().at(3), QStringLiteral("STATUS"));
    EXPECT_EQ(emulator.screenLines().at(0), QStringLiteral("b"));
}

TEST(TerminalModel, ScrollbackKeepsWhatScrolledOffAndIgnoresWhatWasCleared)
{
    Term::Emulator emulator(10, 2, 100);
    type(emulator, QStringLiteral("one\r\ntwo\r\nthree\r\n"));
    // Two visible rows, three lines written: "one" and "two" have scrolled
    // off and must be recoverable from the history.
    EXPECT_EQ(emulator.screenLines().at(0), QStringLiteral("three"));
    EXPECT_TRUE(emulator.scrollbackLines().contains(QStringLiteral("one")));

    // `CSI 2 J` clears the screen but keeps the history: "clear" and
    // "clear scrollback" are different requests.
    const int before = emulator.scrollbackCount();
    type(emulator, QStringLiteral("\x1b[2J"));
    EXPECT_EQ(emulator.scrollbackCount(), before);

    // `CSI 3 J` is the one that clears it.
    type(emulator, QStringLiteral("\x1b[3J"));
    EXPECT_EQ(emulator.scrollbackCount(), 0);
}

TEST(TerminalModel, ScrollbackIsCappedInsteadOfGrowingForever)
{
    Term::Emulator emulator(10, 2, 3);
    for (int i = 0; i < 10; ++i) {
        type(emulator, QStringLiteral("line%1\r\n").arg(i));
    }
    EXPECT_LE(emulator.scrollbackCount(), 3);
    // The most recent lines are the ones kept.
    const QStringList lines = emulator.scrollbackLines();
    EXPECT_TRUE(lines.join(QLatin1Char('\n')).contains(QLatin1String("line8")));
    EXPECT_FALSE(lines.join(QLatin1Char('\n')).contains(QLatin1String("line0")));
}

TEST(TerminalModel, AlternateScreenHidesTheShellAndRestoresIt)
{
    // `vim` switches to the alternate screen and back; getting the restore
    // wrong leaves the user staring at a blank shell.
    Term::Emulator emulator(10, 3);
    type(emulator, QStringLiteral("$ ls"));
    type(emulator, QStringLiteral("\x1b[?1049h"));
    EXPECT_TRUE(emulator.isAlternateScreen());
    EXPECT_EQ(screenText(emulator, 3).trimmed(), QString());
    type(emulator, QStringLiteral("editor"));
    type(emulator, QStringLiteral("\x1b[?1049l"));
    EXPECT_FALSE(emulator.isAlternateScreen());
    EXPECT_EQ(emulator.screenLines().at(0), QStringLiteral("$ ls"));
}

TEST(TerminalModel, AUtf8CharacterSplitAcrossChunksIsOneCharacter)
{
    // A PTY read boundary can fall inside a multi-byte character. Decoding
    // each chunk independently turns "é" into two garbage cells.
    Term::Emulator emulator(10, 2);
    const QByteArray bytes = QStringLiteral("caf\u00e9").toUtf8();
    ASSERT_EQ(bytes.size(), 5);
    const Term::EmulatorUpdate partial = emulator.feed(bytes.left(4));
    Q_UNUSED(partial)
    EXPECT_EQ(emulator.currentLine(), QStringLiteral("caf"));
    const Term::EmulatorUpdate rest = emulator.feed(bytes.mid(4));
    Q_UNUSED(rest)
    EXPECT_EQ(emulator.currentLine(), QStringLiteral("caf\u00e9"));
}

TEST(TerminalModel, AWideCharacterOccupiesTwoCells)
{
    Term::Emulator emulator(10, 2);
    type(emulator, QStringLiteral("\u4f60a"));
    // Two cells for the CJK glyph, one for the ASCII letter.
    EXPECT_EQ(emulator.cursorColumn(), 3);
    EXPECT_EQ(emulator.currentLine(), QStringLiteral("\u4f60a"));
}

TEST(TerminalModel, AnEscapeSequenceSplitAcrossChunksIsNotPrinted)
{
    Term::Emulator emulator(20, 2);
    type(emulator, QStringLiteral("a\x1b[3"));
    EXPECT_EQ(emulator.currentLine(), QStringLiteral("a"));
    type(emulator, QStringLiteral("1mb"));
    EXPECT_EQ(emulator.currentLine(), QStringLiteral("ab"));
}

TEST(TerminalModel, AnOscTitleNeverReachesTheGrid)
{
    // A window title is text; a model that only knows CSI prints
    // "0;my title" at the cursor.
    Term::Emulator emulator(30, 2);
    const Term::EmulatorUpdate update =
        emulator.feedText(QStringLiteral("\x1b]0;my title\x07ok"));
    EXPECT_EQ(emulator.currentLine(), QStringLiteral("ok"));
    EXPECT_EQ(update.title, QStringLiteral("my title"));
}

TEST(TerminalModel, ATabStopsAtTheNextMultipleOfEight)
{
    Term::Emulator emulator(30, 2);
    type(emulator, QStringLiteral("ab\tc"));
    EXPECT_EQ(emulator.currentLine(), QStringLiteral("ab      c"));
}

TEST(TerminalModel, UnhandledCsiStillConsumesItsBytes)
{
    // The failure mode this guards: an unknown sequence leaves its characters
    // on the screen, so a program's output is littered with "[?25l".
    Term::Emulator emulator(30, 2);
    type(emulator, QStringLiteral("\x1b[?9999habc"));
    EXPECT_EQ(emulator.currentLine(), QStringLiteral("abc"));
}

TEST(TerminalModel, ResizingSmallerPushesTheTopRowsIntoScrollback)
{
    // Losing the top rows on a shrink would lose the compiler error the user
    // is looking at, which is exactly the moment a resize happens.
    Term::Emulator emulator(20, 5, 100);
    type(emulator, QStringLiteral("a\r\nb\r\nc\r\nd\r\ne"));
    emulator.resize(20, 3);
    EXPECT_EQ(emulator.screenLines().at(0), QStringLiteral("c"));
    const QStringList all = emulator.scrollbackLines();
    EXPECT_EQ(all.at(0), QStringLiteral("a"));
    EXPECT_EQ(all.at(1), QStringLiteral("b"));
}

TEST(TerminalModel, CopyingASelectionJoinsRowsWithNewlines)
{
    Term::Emulator emulator(10, 3);
    type(emulator, QStringLiteral("one\r\ntwo\r\nthree"));
    // A selection from the start of row 0 to column 2 of row 1: the first
    // row keeps its padding up to the line end (the selection is a
    // rectangle-ish range, and a terminal pads every row), the last row
    // stops at the given column.
    EXPECT_EQ(emulator.textBetween(0, 0, 1, 2), QStringLiteral("one       \ntwo"));
    // Reversed corners produce the same text.
    EXPECT_EQ(emulator.textBetween(1, 2, 0, 0), QStringLiteral("one       \ntwo"));
    // A single row is copied exactly, with the padding trimmed off the end.
    EXPECT_EQ(emulator.textBetween(0, 0, 0, 9), QStringLiteral("one"));
}

TEST(TerminalModel, TheLineOnlyDrawingCharsetMapsToBoxCharacters)
{
    // `ESC ( 0` followed by `qq` is a horizontal rule in the DEC charset;
    // printing "qq" instead makes every TUI's frame look wrong.
    Term::Emulator emulator(10, 2);
    type(emulator, QStringLiteral("\x1b(0qq\x1b(B q"));
    EXPECT_EQ(emulator.currentLine(), QStringLiteral("\u2500\u2500 q"));
}

TEST(TerminalModel, TheCursorVisibilityModeIsTracked)
{
    Term::Emulator emulator(10, 2);
    type(emulator, QStringLiteral("\x1b[?25l"));
    EXPECT_FALSE(emulator.cursorVisible());
    type(emulator, QStringLiteral("\x1b[?25h"));
    EXPECT_TRUE(emulator.cursorVisible());
}

TEST(TerminalModel, BracketedPasteAndApplicationCursorKeysAreTracked)
{
    // The controller asks for these before sending input: bracketed paste
    // tells the shell the text arrived as a paste, and the application
    // cursor mode changes what an arrow key must send.
    Term::Emulator emulator(10, 2);
    EXPECT_FALSE(emulator.bracketedPaste());
    type(emulator, QStringLiteral("\x1b[?2004h\x1b[?1h"));
    EXPECT_TRUE(emulator.bracketedPaste());
    EXPECT_TRUE(emulator.cursorKeysApplicationMode());
}

// ---------------------------------------------------------------------------
// Shell integration: OSC 133 marks, and what they let a terminal answer
// ---------------------------------------------------------------------------

namespace {

namespace Sh = LightCpp::Workbench::Shell;

Sh::Mark markOf(Sh::Mark::Kind kind, int row, int exitCode = -1)
{
    Sh::Mark mark;
    mark.kind = kind;
    mark.row = row;
    mark.exitCode = exitCode;
    return mark;
}

}  // namespace

TEST(ShellIntegration, TheFourMarksParseFromTheirPayloads)
{
    Sh::Mark mark;
    ASSERT_TRUE(Sh::parseOsc133(QStringLiteral("A"), 3, &mark));
    EXPECT_EQ(mark.kind, Sh::Mark::Kind::PromptStart);
    EXPECT_EQ(mark.row, 3);

    ASSERT_TRUE(Sh::parseOsc133(QStringLiteral("B"), 3, &mark));
    EXPECT_EQ(mark.kind, Sh::Mark::Kind::PromptEnd);

    ASSERT_TRUE(Sh::parseOsc133(QStringLiteral("C"), 4, &mark));
    EXPECT_EQ(mark.kind, Sh::Mark::Kind::CommandStart);

    ASSERT_TRUE(Sh::parseOsc133(QStringLiteral("D;0"), 4, &mark));
    EXPECT_EQ(mark.kind, Sh::Mark::Kind::CommandEnd);
    EXPECT_EQ(mark.exitCode, 0);
    ASSERT_TRUE(Sh::parseOsc133(QStringLiteral("D;127"), 5, &mark));
    EXPECT_EQ(mark.exitCode, 127);
}

TEST(ShellIntegration, AFailureAndANonFailureAreDifferentAnswers)
{
    // `D` with no code is the shell saying nothing, not saying "success".
    // Treating it as 0 would report every failed command as passed.
    Sh::Mark mark;
    ASSERT_TRUE(Sh::parseOsc133(QStringLiteral("D"), 1, &mark));
    EXPECT_EQ(mark.exitCode, -1);

    Sh::Tracker tracker;
    tracker.feed(markOf(Sh::Mark::Kind::PromptStart, 0));
    tracker.feed(markOf(Sh::Mark::Kind::CommandStart, 0));
    tracker.feed(mark);
    ASSERT_EQ(tracker.commands().size(), 1);
    EXPECT_EQ(tracker.commands().first().exitCode, -1);
}

TEST(ShellIntegration, TheWholePayloadWithItsPrefixIsAlsoAccepted)
{
    // Whether the caller strips "133;" is an implementation detail of the
    // caller; both spellings must work.
    Sh::Mark mark;
    EXPECT_TRUE(Sh::parseOsc133(QStringLiteral("133;A"), 0, &mark));
    EXPECT_EQ(mark.kind, Sh::Mark::Kind::PromptStart);
}

TEST(ShellIntegration, NonMarkOscPayloadsAreRefused)
{
    // OSC 133 shares its channel with titles and hyperlinks; a parser that
    // accepts everything here turns a window title into a prompt mark.
    Sh::Mark mark;
    EXPECT_FALSE(Sh::parseOsc133(QStringLiteral("0;my title"), 0, &mark));
    EXPECT_FALSE(Sh::parseOsc133(QStringLiteral("8;;http://x"), 0, &mark));
    EXPECT_FALSE(Sh::parseOsc133(QStringLiteral("P;Cwd=/tmp"), 0, &mark));
    EXPECT_FALSE(Sh::parseOsc133(QString(), 0, &mark));
}

TEST(ShellIntegration, ACompleteCommandBecomesOneRecord)
{
    Sh::Tracker tracker;
    tracker.feed(markOf(Sh::Mark::Kind::PromptStart, 0));
    tracker.feed(markOf(Sh::Mark::Kind::PromptEnd, 0));
    tracker.feed(markOf(Sh::Mark::Kind::CommandStart, 0));
    // The finish mark is on row 1 and reports exit status 1.
    tracker.feed(markOf(Sh::Mark::Kind::CommandEnd, 1, 1));
    tracker.feed(markOf(Sh::Mark::Kind::PromptStart, 2));

    ASSERT_EQ(tracker.commands().size(), 1);
    const Sh::CommandRecord record = tracker.lastCommand();
    // The record's prompt row is the row the *mark* was on, which here is 0.
    EXPECT_EQ(record.promptRow, 0);
    EXPECT_EQ(record.commandRow, 0);
    EXPECT_EQ(record.exitCode, 1);
    EXPECT_TRUE(record.finished);
    // The new prompt is open and nothing is running.
    EXPECT_EQ(tracker.currentPromptRow(), 2);
    EXPECT_FALSE(tracker.isCommandRunning());
}

TEST(ShellIntegration, ACommandInFlightIsNotYetACommand)
{
    Sh::Tracker tracker;
    tracker.feed(markOf(Sh::Mark::Kind::PromptStart, 0));
    tracker.feed(markOf(Sh::Mark::Kind::CommandStart, 0));
    EXPECT_TRUE(tracker.isCommandRunning());
    EXPECT_TRUE(tracker.commands().isEmpty());
    // lastCommand() of an empty history is a default record, flagged by
    // `finished` being false rather than by a sentinel exit code.
    EXPECT_FALSE(tracker.lastCommand().finished);
}

TEST(ShellIntegration, AnOutOfOrderFinishIsIgnored)
{
    // Integration installed mid-session: the shell emits `D` for a command
    // whose `C` was never seen. Recording it would give a command with no
    // start row, which the "jump to prompt" logic would then chase.
    Sh::Tracker tracker;
    tracker.feed(markOf(Sh::Mark::Kind::CommandEnd, 4, 0));
    EXPECT_TRUE(tracker.commands().isEmpty());
    EXPECT_FALSE(tracker.isCommandRunning());
}

TEST(ShellIntegration, SuccessIsNotAFailure)
{
    Sh::Tracker tracker;
    for (int i = 0; i < 3; ++i) {
        tracker.feed(markOf(Sh::Mark::Kind::PromptStart, i * 2));
        tracker.feed(markOf(Sh::Mark::Kind::CommandStart, i * 2));
        tracker.feed(markOf(Sh::Mark::Kind::CommandEnd, i * 2 + 1, i == 1 ? 2 : 0));
    }
    EXPECT_EQ(tracker.commands().size(), 3);
    const Sh::CommandRecord failure = tracker.lastFailure();
    EXPECT_TRUE(failure.finished);
    EXPECT_EQ(failure.exitCode, 2);
    EXPECT_EQ(failure.promptRow, 2);
}

TEST(ShellIntegration, HistoryIsCappedAndTheOldestGoesFirst)
{
    Sh::Tracker tracker;
    tracker.setHistoryLimit(2);
    for (int i = 0; i < 5; ++i) {
        tracker.feed(markOf(Sh::Mark::Kind::PromptStart, i));
        tracker.feed(markOf(Sh::Mark::Kind::CommandStart, i));
        tracker.feed(markOf(Sh::Mark::Kind::CommandEnd, i, i));
    }
    ASSERT_EQ(tracker.commands().size(), 2);
    EXPECT_EQ(tracker.commands().first().exitCode, 3);
    EXPECT_EQ(tracker.commands().last().exitCode, 4);
}

TEST(ShellIntegration, PromptRowsAreFoundForJumping)
{
    Sh::Tracker tracker;
    for (int row = 0; row < 10; row += 3) {
        tracker.feed(markOf(Sh::Mark::Kind::PromptStart, row));
        tracker.feed(markOf(Sh::Mark::Kind::CommandStart, row));
        tracker.feed(markOf(Sh::Mark::Kind::CommandEnd, row + 1, 0));
    }
    // Rows 3, 6, 9 — the prompt at 0 is outside the requested window.
    EXPECT_EQ(tracker.promptRowsBetween(2, 9), (QList<int>{3, 6, 9}));
    // Reversed bounds are the same window.
    EXPECT_EQ(tracker.promptRowsBetween(9, 2), (QList<int>{3, 6, 9}));
}

TEST(ShellIntegration, SplittingAPromptLineAtTheMarkGivesTheTypedCommand)
{
    // `CSI 133;B` records a column, not a text; the split is how "what is the
    // user typing" is answered without re-parsing the shell's prompt.
    const QString line = QStringLiteral("me@box:~/src$ make -j4");
    // "me@box:~/src$ " is 14 characters, so the command starts at column 14.
    EXPECT_EQ(Sh::promptText(line, 14), QStringLiteral("me@box:~/src$ "));
    EXPECT_EQ(Sh::commandText(line, 14), QStringLiteral("make -j4"));
    // A mark beyond the line (after a resize) is clamped, not trusted.
    EXPECT_EQ(Sh::commandText(line, 999), QString());
    EXPECT_EQ(Sh::promptText(line, 999), line);
}

TEST(TerminalPalette, TheFirstSixteenAreTheAnsiColours)
{
    // A tool that emits SGR "31" is asking for red as xterm defines it; if
    // the table disagreed, `ls --color` would come out the wrong hue.
    EXPECT_EQ(Term::Emulator::paletteColor(0), 0x000000u);
    EXPECT_EQ(Term::Emulator::paletteColor(1), 0xcd0000u);
    EXPECT_EQ(Term::Emulator::paletteColor(7), 0xe5e5e5u);
    EXPECT_EQ(Term::Emulator::paletteColor(9), 0xff0000u);
    EXPECT_EQ(Term::Emulator::paletteColor(15), 0xffffffu);
}

TEST(TerminalPalette, TheCubeAndTheGreyRampMatchEveryOtherTerminal)
{
    // Index 16 is the cube's origin, 231 its far corner, 232 the darkest
    // grey and 255 the lightest. These four pins catch a cube built with the
    // wrong step table, which is otherwise invisible until something uses
    // all 216 entries.
    EXPECT_EQ(Term::Emulator::paletteColor(16), 0x000000u);
    EXPECT_EQ(Term::Emulator::paletteColor(231), 0xffffffu);
    EXPECT_EQ(Term::Emulator::paletteColor(232), 0x080808u);
    EXPECT_EQ(Term::Emulator::paletteColor(255), 0xeeeeeeu);
    // A component taken from the middle of the ramp.
    EXPECT_EQ(Term::Emulator::paletteColor(16 + 36 * 2 + 6 * 2 + 2), 0x878787u);
}

TEST(TerminalPalette, IndicesOutsideTheTableResolveToNothing)
{
    EXPECT_EQ(Term::Emulator::paletteColor(-1), 0u);
    EXPECT_EQ(Term::Emulator::paletteColor(256), 0u);
    EXPECT_EQ(Term::Emulator::paletteColor(9999), 0u);
}

TEST(TerminalEmulator, TheTitleIsReadableAfterTheUpdateThatSetIt)
{
    // A tab that renames itself from the update alone loses the name as
    // soon as another byte arrives; the getter keeps it.
    Term::Emulator emulator(20, 3);
    EXPECT_EQ(emulator.title(), QString());
    const Term::EmulatorUpdate update =
        emulator.feedText(QStringLiteral("\x1b]2;vim lightcpp.cpp\x07"));
    EXPECT_EQ(update.title, QStringLiteral("vim lightcpp.cpp"));
    EXPECT_EQ(emulator.title(), QStringLiteral("vim lightcpp.cpp"));
    // OSC 0 is the same field.
    const Term::EmulatorUpdate second =
        emulator.feedText(QStringLiteral("\x1b]0;bash\x07"));
    EXPECT_EQ(second.title, QStringLiteral("bash"));
    EXPECT_EQ(emulator.title(), QStringLiteral("bash"));
    // An empty title is not a title: a shell that clears it must not blank
    // the tab back to "Terminal".
    const Term::EmulatorUpdate empty = emulator.feedText(QStringLiteral("\x1b]2;\x07"));
    EXPECT_EQ(empty.title, QString());
    EXPECT_EQ(emulator.title(), QStringLiteral("bash"));
}

TEST(NatvisOrder, AUserFileLoadsAfterTheToolchainAndThereforeWins)
{
    // Loading order *is* precedence for .natvis: the last file loaded owns
    // the type. Getting this backwards means the toolchain silently beats
    // the visualiser the developer just wrote.
    QList<Debug::NatvisCandidate> candidates;
    candidates.append({QStringLiteral("C:/ide/autoexp.natvis"), Debug::NatvisOrigin::Toolchain, {}});
    candidates.append({QStringLiteral("D:/proj/mine.natvis"), Debug::NatvisOrigin::User, {}});
    candidates.append({QStringLiteral("D:/proj/team.natvis"), Debug::NatvisOrigin::Public, {}});
    const auto ordered = Debug::orderNatvis(candidates);
    ASSERT_EQ(ordered.size(), 3);
    EXPECT_EQ(ordered.at(0).path, QStringLiteral("C:/ide/autoexp.natvis"));
    EXPECT_EQ(ordered.at(1).path, QStringLiteral("D:/proj/team.natvis"));
    EXPECT_EQ(ordered.at(2).path, QStringLiteral("D:/proj/mine.natvis"));
}

TEST(NatvisOrder, PrivateBeatsPublicAtTheSameLevel)
{
    // The documented rule: /public is the shared half, /private the local
    // half, and the local one is what a developer debugging their own
    // visualiser expects to see take effect.
    QList<Debug::NatvisCandidate> candidates;
    candidates.append({QStringLiteral("b.natvis"), Debug::NatvisOrigin::Private, {}});
    candidates.append({QStringLiteral("a.natvis"), Debug::NatvisOrigin::Public, {}});
    const auto ordered = Debug::orderNatvis(candidates);
    ASSERT_EQ(ordered.size(), 2);
    EXPECT_EQ(ordered.at(0).path, QStringLiteral("a.natvis"));
    EXPECT_EQ(ordered.at(1).path, QStringLiteral("b.natvis"));
}

TEST(NatvisOrder, RedefiningATypeMarksTheLaterFileAsAnOverride)
{
    QList<Debug::NatvisCandidate> candidates;
    candidates.append({QStringLiteral("tool.natvis"), Debug::NatvisOrigin::Toolchain,
                       {QStringLiteral("std::vector<int>")}});
    candidates.append({QStringLiteral("mine.natvis"), Debug::NatvisOrigin::User,
                       {QStringLiteral("std::vector<int>"), QStringLiteral("MyType")}});
    const auto ordered = Debug::orderNatvis(candidates);
    ASSERT_EQ(ordered.size(), 2);
    EXPECT_FALSE(ordered.at(0).overrides);
    EXPECT_TRUE(ordered.at(1).overrides);
}

TEST(NatvisOrder, AFileWithNoDeclaredTypesNeverClaimsAnOverride)
{
    // Without a parse there is nothing to claim, and a false "overrides"
    // badge is worse than a missing one: it sends the reader looking for a
    // conflict that does not exist.
    QList<Debug::NatvisCandidate> candidates;
    candidates.append({QStringLiteral("tool.natvis"), Debug::NatvisOrigin::Toolchain, {}});
    candidates.append({QStringLiteral("mine.natvis"), Debug::NatvisOrigin::User, {}});
    const auto ordered = Debug::orderNatvis(candidates);
    ASSERT_EQ(ordered.size(), 2);
    EXPECT_FALSE(ordered.at(1).overrides);
}

TEST(NatvisOrder, EmptyPathsAreDroppedRatherThanSendingADotCommand)
{
    QList<Debug::NatvisCandidate> candidates;
    candidates.append({QStringLiteral("   "), Debug::NatvisOrigin::User, {}});
    candidates.append({QStringLiteral("ok.natvis"), Debug::NatvisOrigin::User, {}});
    EXPECT_EQ(Debug::orderNatvis(candidates).size(), 1);
    EXPECT_TRUE(Debug::natvisCommands(Debug::Backend::Cdb,
                                      {Debug::NatvisCandidate{QString(), Debug::NatvisOrigin::User, {}}})
                    .isEmpty());
}

TEST(NatvisOrder, TheCdbCommandsFollowTheSameOrder)
{
    QList<Debug::NatvisCandidate> candidates;
    candidates.append({QStringLiteral("mine.natvis"), Debug::NatvisOrigin::User, {}});
    candidates.append({QStringLiteral("tool.natvis"), Debug::NatvisOrigin::Toolchain, {}});
    const auto commands = Debug::natvisCommands(Debug::Backend::Cdb, candidates);
    ASSERT_EQ(commands.size(), 2);
    EXPECT_EQ(commands.at(0), QStringLiteral(".nvload tool.natvis"));
    EXPECT_EQ(commands.at(1), QStringLiteral(".nvload mine.natvis"));
}

TEST(NatvisOrder, ATypeTwoFilesDefineIsReportedWithBothFilesLoserFirst)
{
    QList<Debug::NatvisCandidate> candidates;
    candidates.append({QStringLiteral("mine.natvis"), Debug::NatvisOrigin::User,
                       {QStringLiteral("Foo"), QStringLiteral("OnlyMine")}});
    candidates.append({QStringLiteral("tool.natvis"), Debug::NatvisOrigin::Toolchain,
                       {QStringLiteral("Foo")}});
    const auto conflicts = Debug::natvisConflicts(candidates);
    ASSERT_EQ(conflicts.size(), 1);
    EXPECT_EQ(conflicts.at(0).first, QStringLiteral("Foo"));
    ASSERT_EQ(conflicts.at(0).second.size(), 2);
    EXPECT_EQ(conflicts.at(0).second.at(0), QStringLiteral("tool.natvis"));
    EXPECT_EQ(conflicts.at(0).second.at(1), QStringLiteral("mine.natvis"));
}

TEST(NatvisOrder, TypesAreScannedOutOfARealDocument)
{
    const QString xml = QStringLiteral(
        "<?xml version=\"1.0\"?>\n"
        "<AutoVisualizer xmlns=\"http://schemas.microsoft.com/vstudio/debugger/natvis/2010\">\n"
        "  <Type Name=\"Widget\">\n"
        "    <DisplayString>{id}</DisplayString>\n"
        "  </Type>\n"
        "  <Type Name=\"std::vector&lt;*\">\n"
        "    <DisplayString>size {_M_finish - _M_start}</DisplayString>\n"
        "  </Type>\n"
        "</AutoVisualizer>\n");
    const auto types = Debug::natvisTypesIn(xml);
    ASSERT_EQ(types.size(), 2);
    EXPECT_EQ(types.at(0), QStringLiteral("Widget"));
    // A wildcard is part of the claim, so it is kept verbatim.
    EXPECT_EQ(types.at(1), QStringLiteral("std::vector&lt;*"));
}

TEST(NatvisOrder, AMalformedDocumentYieldsWhatCouldBeReadAndNoCrash)
{
    // The debugger is about to reject this file; the IDE must not die first.
    EXPECT_TRUE(Debug::natvisTypesIn(QStringLiteral("<Type Name=\"Unclosed")).isEmpty());
    EXPECT_TRUE(Debug::natvisTypesIn(QString()).isEmpty());
    EXPECT_EQ(Debug::natvisTypesIn(QStringLiteral("<Type Name=\"A\"><Type Name=\"B\">")).size(), 2);
}
