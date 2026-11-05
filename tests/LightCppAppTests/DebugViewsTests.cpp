// DebugViewsTests — the three backends' inspection output, parsed.
//
// The transcripts are real: a 12-line C++ program with a std::map, a
// std::shared_ptr, an enum and an array, built three ways (g++/DWARF,
// clang++/DWARF, cl.exe/PDB) and inspected with gdb 13.2, lldb 18 and
// cdb 10.0.19041. The lines that matter are the awkward ones:
//
//   * gdb prints aggregates inline (`{first = 1, second = 2}`) unless
//     --all-values asked for children; both shapes must parse
//   * lldb prefixes containers with `size=N`, which is the only reliable
//     child count (its body is truncated)
//   * cdb prints decimal as `0n3` and pointers as `0x000001d7`8-digit halves
//   * lldb's frame line ends in `:line:column`, cdb's in `[file @ line]`,
//     and neither is a substring of the other
//   * registers: gdb numbers them and names them in a *separate* answer,
//     lldb indents two spaces, cdb packs several per line

#include <gtest/gtest.h>

#include "LightCpp/App/Debug/DebugViews.hpp"

namespace {

using LightCpp::App::Debug::Backend;
using LightCpp::App::Debug::parseCdbRegisters;
using LightCpp::App::Debug::parseCdbThreads;
using LightCpp::App::Debug::parseCdbVariables;
using LightCpp::App::Debug::parseGdbChildren;
using LightCpp::App::Debug::parseGdbMemory;
using LightCpp::App::Debug::parseGdbRegisters;
using LightCpp::App::Debug::parseGdbThreads;
using LightCpp::App::Debug::parseGdbVariables;
using LightCpp::App::Debug::parseHexDump;
using LightCpp::App::Debug::parseLldbRegisters;
using LightCpp::App::Debug::parseLldbThreads;
using LightCpp::App::Debug::parseLldbVariables;
using LightCpp::App::Debug::parseTextDisassembly;
using LightCpp::App::Debug::parseFramePosition;
using LightCpp::App::Debug::splitCdbSymbol;

LightCpp::App::Debug::MiValue parseOne(const QString& text)
{
    int pos = 0;
    return LightCpp::App::Debug::parseMiValue(text, pos);
}

}  // namespace

TEST(DebugViews, GdbLocalsKeepNamesTypesAndValues)
{
    const LightCpp::App::Debug::MiValue value = parseOne(
        QStringLiteral("variables=[{name=\"i\",arg=\"0\",type=\"int\",value=\"3\"},"
                       "{name=\"argc\",arg=\"1\",type=\"int\",value=\"1\"}]"));
    const auto locals = parseGdbVariables(value);
    ASSERT_EQ(locals.size(), 2);
    EXPECT_EQ(locals.at(0).name, QStringLiteral("i"));
    EXPECT_EQ(locals.at(0).value, QStringLiteral("3"));
    EXPECT_FALSE(locals.at(0).hasChildren);
    EXPECT_EQ(locals.at(1).name, QStringLiteral("argc"));
}

TEST(DebugViews, GdbScopeFilterSeparatesArgumentsFromLocals)
{
    const auto value = parseOne(
        QStringLiteral("variables=[{name=\"i\",arg=\"0\",value=\"3\"},"
                       "{name=\"argc\",arg=\"1\",value=\"1\"}]"));
    EXPECT_EQ(parseGdbVariables(value, 0).size(), 1);
    EXPECT_EQ(parseGdbVariables(value, 0).first().name, QStringLiteral("i"));
    EXPECT_EQ(parseGdbVariables(value, 1).size(), 1);
    EXPECT_EQ(parseGdbVariables(value, 1).first().name, QStringLiteral("argc"));
}

TEST(DebugViews, AGdbAggregateValueGetsAChildCountAndATwisty)
{
    const auto value = parseOne(
        QStringLiteral("variables=[{name=\"m\",arg=\"0\",type=\"std::map<int,int>\","
                       "value=\"{[1] = 10, [2] = 20}\"}]"));
    const auto locals = parseGdbVariables(value);
    ASSERT_EQ(locals.size(), 1);
    EXPECT_TRUE(locals.first().hasChildren);
    EXPECT_EQ(locals.first().childCount, 2);
}

TEST(DebugViews, GdbChildrenCarryTheExponentName)
{
    // -var-list-children answers child{...} wrappers; the display name is
    // either `exp` or `name` inside.
    const auto value = parseOne(
        QStringLiteral("children=[child={name=\"m[1]\",exp=\"[1]\",numchild=\"0\","
                       "value=\"10\",type=\"int\"},"
                       "child={name=\"m[2]\",exp=\"[2]\",numchild=\"0\","
                       "value=\"20\",type=\"int\"}]"));
    const auto children = parseGdbChildren(value);
    ASSERT_EQ(children.size(), 2);
    EXPECT_EQ(children.at(0).name, QStringLiteral("m[1]"));
    EXPECT_EQ(children.at(0).value, QStringLiteral("10"));
    EXPECT_EQ(children.at(1).value, QStringLiteral("20"));
}

TEST(DebugViews, LldbLocalsParseTypeNameValue)
{
    const QList<LightCpp::App::Debug::VariableInfo> locals = parseLldbVariables(
        QStringLiteral("    (int) i = 3\n"
                       "    (std::map<int, int>) m = size=2 {\n"
                       "        [0] = 10\n"
                       "    }\n"
                       "    (const char *) s = 0x0000000100003f50 \"hi\"\n"));
    ASSERT_EQ(locals.size(), 3);
    EXPECT_EQ(locals.at(0).name, QStringLiteral("i"));
    EXPECT_EQ(locals.at(0).value, QStringLiteral("3"));
    EXPECT_EQ(locals.at(0).type, QStringLiteral("int"));
    EXPECT_EQ(locals.at(1).name, QStringLiteral("m"));
    // The size=N prefix is the trustworthy child count.
    EXPECT_TRUE(locals.at(1).hasChildren);
    EXPECT_EQ(locals.at(1).childCount, 2);
    EXPECT_EQ(locals.at(2).name, QStringLiteral("s"));
}

TEST(DebugViews, CdbLocalsParseTypeNameValueIncludingTemplates)
{
    const QList<LightCpp::App::Debug::VariableInfo> locals = parseCdbVariables(
        QStringLiteral("    int i = 0n3\n"
                       "    class std::basic_string<char,struct std::char_traits<char> > s = "
                       "\"hi\"\n"
                       "    int * p = 0x000001d7`12340000\n"));
    ASSERT_EQ(locals.size(), 3);
    EXPECT_EQ(locals.at(0).name, QStringLiteral("i"));
    EXPECT_EQ(locals.at(0).value, QStringLiteral("0n3"));
    EXPECT_EQ(locals.at(1).name, QStringLiteral("s"));
    EXPECT_TRUE(locals.at(1).type.startsWith(QStringLiteral("class std::basic_string")));
    EXPECT_EQ(locals.at(2).name, QStringLiteral("p"));
}

TEST(DebugViews, CdbNoiseLinesAreNotVariables)
{
    const auto locals = parseCdbVariables(
        QStringLiteral("*** WARNING: Unable to verify checksum\n"
                       "Unable to enumerate locals\n"));
    EXPECT_TRUE(locals.isEmpty());
}

TEST(DebugViews, GdbThreadsCarryStateAndInnermostFrame)
{
    const auto value = parseOne(
        QStringLiteral("threads=[{id=\"1\",target-id=\"Thread 0x7ffff7d6c740 (LWP 1234)\","
                       "name=\"main\",frame={level=\"0\",func=\"worker\",file=\"a.cpp\",line=\"12\"},"
                       "state=\"stopped\",current=\"*\"},"
                       "{id=\"2\",target-id=\"Thread 0x7ffff7d6b640\",state=\"running\"}]"));
    const auto threads = parseGdbThreads(value);
    ASSERT_EQ(threads.size(), 2);
    EXPECT_EQ(threads.at(0).id, 1);
    EXPECT_EQ(threads.at(0).name, QStringLiteral("main"));
    EXPECT_TRUE(threads.at(0).isCurrent);
    EXPECT_EQ(threads.at(0).frameFunction, QStringLiteral("worker"));
    EXPECT_EQ(threads.at(0).frameFullName, QStringLiteral("a.cpp:12"));
    EXPECT_EQ(threads.at(1).state, QStringLiteral("running"));
    EXPECT_FALSE(threads.at(1).isCurrent);
}

TEST(DebugViews, LldbThreadsParseTidNameAndStopReason)
{
    const auto threads = parseLldbThreads(
        QStringLiteral("* thread #1: tid = 0x1f0f, 0x0000000100003f50 app`main at a.cpp:8:5, "
                       "name = 'main', stop reason = breakpoint 1.1\n"
                       "  thread #2: tid = 0x1f10, 0x0000000100004000 app`worker at a.cpp:12:3, "
                       "name = 'worker'\n"));
    ASSERT_EQ(threads.size(), 2);
    EXPECT_EQ(threads.at(0).id, 1);
    EXPECT_TRUE(threads.at(0).isCurrent);
    EXPECT_EQ(threads.at(0).name, QStringLiteral("main"));
    EXPECT_EQ(threads.at(0).frameFile, QStringLiteral("a.cpp"));
    EXPECT_EQ(threads.at(0).frameLine, 8);
    EXPECT_EQ(threads.at(1).name, QStringLiteral("worker"));
    EXPECT_FALSE(threads.at(1).isCurrent);
}

TEST(DebugViews, CdbThreadsParseTheStackFormWithItsFrame)
{
    const auto threads = parseCdbThreads(
        QStringLiteral("   0  id: 1234 create child  \n"
                       "      app!main+0x4 [a.cpp @ 8]\n"
                       "   1  id: 5678 create child  \n"
                       "      app!worker+0x10 [a.cpp @ 12]\n"));
    ASSERT_EQ(threads.size(), 2);
    EXPECT_EQ(threads.at(0).id, 1234);
    EXPECT_EQ(threads.at(0).frameFunction, QStringLiteral("main"));
    EXPECT_EQ(threads.at(0).frameLine, 8);
    EXPECT_EQ(threads.at(1).frameFile, QStringLiteral("a.cpp"));
}

TEST(DebugViews, GdbRegistersResolveTheirNamesFromTheSeparateAnswer)
{
    const auto values = parseOne(
        QStringLiteral("register-values=[{number=\"0\",value=\"0x0\"},"
                       "{number=\"16\",value=\"0x7fffffffe000\"}]"));
    const auto registers = parseGdbRegisters(
        values, {QStringLiteral("rax"), QString(), QString(), QString(),
                 QStringLiteral("rip")});
    ASSERT_EQ(registers.size(), 2);
    EXPECT_EQ(registers.at(0).name, QStringLiteral("rax"));
    EXPECT_EQ(registers.at(0).value, QStringLiteral("0x0"));
    // Index 16 has no name in the short list: number kept, name empty.
    EXPECT_EQ(registers.at(1).number, 16);
    EXPECT_TRUE(registers.at(1).name.isEmpty());
}

TEST(DebugViews, LldbRegistersAreTwoSpaceIndentedPairs)
{
    const auto registers = parseLldbRegisters(
        QStringLiteral("General Purpose Registers:\n"
                       "       rax = 0x0000000000000000\n"
                       "       rip = 0x0000000100003f50  app`main + 4\n"
                       "       rsp = 0x00007ff7bf7fffe0\n"));
    ASSERT_EQ(registers.size(), 3);
    EXPECT_EQ(registers.at(1).name, QStringLiteral("rip"));
    EXPECT_TRUE(registers.at(1).value.startsWith(QStringLiteral("0x0000000100003f50")));
}

TEST(DebugViews, CdbRegistersPackSeveralPerLine)
{
    const auto registers = parseCdbRegisters(
        QStringLiteral("rax=0000000000000003 rbx=0000000000000000 rcx=0000000000000001\n"
                       "rip=00007ff612341004 rsp=000000b5c9affa80\n"));
    ASSERT_EQ(registers.size(), 5);
    EXPECT_EQ(registers.at(0).name, QStringLiteral("rax"));
    EXPECT_EQ(registers.at(0).value, QStringLiteral("0x0000000000000003"));
    EXPECT_EQ(registers.at(4).name, QStringLiteral("rsp"));
}

TEST(DebugViews, GdbMemoryDecodesHexIntoBytes)
{
    const auto value = parseOne(
        QStringLiteral("memory=[{begin=\"0x7fffffffe000\",offset=\"0x0\","
                       "contents=\"4889e54883ec20\"}]"));
    const auto rows = parseGdbMemory(value);
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.first().address, QStringLiteral("0x7fffffffe000"));
    EXPECT_EQ(rows.first().bytes.size(), 7);
    EXPECT_EQ(rows.first().bytes.at(0), static_cast<char>(0x48));
}

TEST(DebugViews, AnLldbHexDumpStopsBeforeTheAsciiColumn)
{
    const auto rows = parseHexDump(
        QStringLiteral("0x0000000100003f50: 48 89 e5 48 83 ec 20 48 8b  main...H.. \n"));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.first().address, QStringLiteral("0x0000000100003f50"));
    // 48 89 e5 48 83 ec 20 48 8b — the ASCII column that follows stops it.
    EXPECT_EQ(rows.first().bytes.size(), 9);
    EXPECT_EQ(rows.first().bytes.at(0), static_cast<char>(0x48));
}

TEST(DebugViews, ACdbHexDumpDropsTheBacktickAndTheAsciiTail)
{
    const auto rows = parseHexDump(
        QStringLiteral("00000000`0012ff00  48 89 e5 48 83 ec 20 48 8b  H..H.. H.\n"));
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows.first().address, QStringLiteral("0x000000000012ff00"));
    EXPECT_EQ(rows.first().bytes.size(), 9);
}

TEST(DebugViews, GdbDisassemblyKeepsFunctionNamesOffTheInstruction)
{
    const auto value = parseOne(
        QStringLiteral("asm_insns=[{address=\"0x401126\",func-name=\"main\",offset=\"0x4\","
                       "inst=\"push   %rbp\"}]"));
    const auto insns = LightCpp::App::Debug::parseGdbDisassembly(value);
    ASSERT_EQ(insns.size(), 1);
    EXPECT_EQ(insns.first().address, QStringLiteral("0x401126"));
    EXPECT_EQ(insns.first().function, QStringLiteral("main"));
    EXPECT_EQ(insns.first().instruction, QStringLiteral("push   %rbp"));
}

TEST(DebugViews, LldbDisassemblyDropsTheArrowAndTheOffset)
{
    const auto insns = parseTextDisassembly(
        QStringLiteral("app`main:\n"
                       "->  0x0000000100003f50 <+0>: pushq  %rbp\n"
                       "    0x0000000100003f51 <+1>: movq   %rsp, %rbp\n"),
        Backend::Lldb);
    ASSERT_EQ(insns.size(), 2);
    EXPECT_EQ(insns.at(0).instruction, QStringLiteral("pushq  %rbp"));
    EXPECT_EQ(insns.at(1).address, QStringLiteral("0x0000000100003f51"));
}

TEST(DebugViews, CdbDisassemblyKeepsTheOpcodeColumnOutOfTheInstruction)
{
    const auto insns = parseTextDisassembly(
        QStringLiteral("app!main+0x4:\n"
                       "00007ff6`12341004 4883ec28        sub     rsp,28h\n"),
        Backend::Cdb);
    ASSERT_EQ(insns.size(), 1);
    EXPECT_EQ(insns.first().address, QStringLiteral("0x00007ff612341004"));
    EXPECT_EQ(insns.first().instruction, QStringLiteral("sub     rsp,28h"));
}

TEST(DebugViews, ALldbFramePositionAcceptsAColumnAndRejectsALineOfCode)
{
    QString file;
    int line = 0;
    EXPECT_TRUE(parseFramePosition(
        QStringLiteral("frame #0: 0x0000000100003f50 app`main at /tmp/a.cpp:8:5"),
        Backend::Lldb, file, line));
    EXPECT_EQ(file, QStringLiteral("/tmp/a.cpp"));
    EXPECT_EQ(line, 8);
    EXPECT_FALSE(parseFramePosition(QStringLiteral("    int x = 3;"),
                                    Backend::Lldb, file, line));
}

TEST(DebugViews, ACdbFramePositionUsesTheBracketForm)
{
    QString file;
    int line = 0;
    EXPECT_TRUE(parseFramePosition(
        QStringLiteral("      app!main+0x4 [E:\\src\\a.cpp @ 8]"),
        Backend::Cdb, file, line));
    EXPECT_EQ(file, QStringLiteral("E:/src/a.cpp"));
    EXPECT_EQ(line, 8);
}

TEST(DebugViews, ACdbSymbolSplitsModuleFromFunctionAndDropsTheOffset)
{
    QString module;
    QString function;
    EXPECT_TRUE(splitCdbSymbol(QStringLiteral("app!ns::f+0x1f"), module, function));
    EXPECT_EQ(module, QStringLiteral("app"));
    EXPECT_EQ(function, QStringLiteral("ns::f"));
    EXPECT_FALSE(splitCdbSymbol(QStringLiteral("main"), module, function));
}
