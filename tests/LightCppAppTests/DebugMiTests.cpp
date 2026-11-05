// DebugMiTests — the GDB/MI grammar, pinned down.
//
// Every case below is a line a real GDB 13 or 14 printed in a session against
// a small C++ program; the shape of MI output is stable across versions but
// its *nesting* is not, which is the whole reason `parseMiRecord()` exists
// instead of a handful of QRegularExpressions.
//
// The cases that would break a regex-based reader first:
//   * a value that itself contains `,` `=` `{` `}` (a std::map printed inline)
//   * a `}` inside a string (`printf "%s" "{x}"`)
//   * a token in front of the sigil, and none in front of the next record
//   * `^done` with no results at all
//   * an unrecognised record — it must come back as Unknown with its text
//     intact, because that is what the log shows verbatim

#include <gtest/gtest.h>

#include "LightCpp/App/Debug/GdbMi.hpp"

namespace {

using LightCpp::App::Debug::MiRecord;
using LightCpp::App::Debug::MiValue;
using LightCpp::App::Debug::framesFromMi;
using LightCpp::App::Debug::parseMiRecord;
using LightCpp::App::Debug::parseMiValue;
using LightCpp::App::Debug::unescapeC;

MiValue parseOne(const QString& text)
{
    int pos = 0;
    return parseMiValue(text, pos);
}

}  // namespace

TEST(DebugMi, AResultRecordKeepsItsTokenAndClassWord)
{
    const MiRecord record = parseMiRecord(QStringLiteral("12^done,bkpt={number=\"1\"}"));
    EXPECT_EQ(record.kind, MiRecord::Kind::Result);
    EXPECT_EQ(record.name, QStringLiteral("done"));
    EXPECT_EQ(record.token, QStringLiteral("12"));
    ASSERT_NE(record.results.find(QStringLiteral("bkpt")), nullptr);
    EXPECT_EQ(record.results.find(QStringLiteral("bkpt"))->value(QStringLiteral("number")),
              QStringLiteral("1"));
}

TEST(DebugMi, ARecordWithoutATokenStillParses)
{
    const MiRecord record = parseMiRecord(QStringLiteral("*stopped,reason=\"breakpoint-hit\""));
    EXPECT_EQ(record.kind, MiRecord::Kind::ExecAsync);
    EXPECT_EQ(record.token, QString());
    EXPECT_EQ(record.reason(), QStringLiteral("breakpoint-hit"));
}

TEST(DebugMi, AStoppedRecordCarriesTheFrame)
{
    const MiRecord record = parseMiRecord(
        QStringLiteral("*stopped,reason=\"breakpoint-hit\",disp=\"keep\",bkptno=\"1\","
                       "frame={addr=\"0x0000555555555169\",func=\"main\",args=[],"
                       "file=\"a.cpp\",fullname=\"/tmp/a.cpp\",line=\"8\"},"
                       "thread-id=\"1\",stopped-threads=\"all\""));
    const MiValue* frame = record.results.find(QStringLiteral("frame"));
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->value(QStringLiteral("func")), QStringLiteral("main"));
    EXPECT_EQ(frame->value(QStringLiteral("line")), QStringLiteral("8"));
    EXPECT_EQ(frame->number(QStringLiteral("line")), 8);
}

TEST(DebugMi, AValueInsideAValueKeepsItsOwnCommas)
{
    // A std::map printed with --all-values: the inner `{...}` must stay one
    // child, not become three top-level members of the outer tuple.
    const MiValue value = parseOne(
        QStringLiteral("{value=\"{1 = \\\"one\\\", 2 = \\\"two\\\"}\",type=\"std::map\"}"));
    ASSERT_EQ(value.kind(), MiValue::Kind::Tuple);
    EXPECT_EQ(value.value(QStringLiteral("value")),
              QStringLiteral("{1 = \"one\", 2 = \"two\"}"));
    EXPECT_EQ(value.value(QStringLiteral("type")), QStringLiteral("std::map"));
}

TEST(DebugMi, AClosingBraceInsideAStringDoesNotEndTheTuple)
{
    const MiValue value = parseOne(QStringLiteral("{v=\"a}b\", w=\"c\"}"));
    EXPECT_EQ(value.value(QStringLiteral("v")), QStringLiteral("a}b"));
    EXPECT_EQ(value.value(QStringLiteral("w")), QStringLiteral("c"));
}

TEST(DebugMi, ALIstOfFramesIsAListOfTuples)
{
    const MiValue value = parseOne(
        QStringLiteral("[frame={level=\"0\",func=\"main\",file=\"a.cpp\",line=\"8\"},"
                       "frame={level=\"1\",func=\"start\",file=\"crt1.c\",line=\"100\"}]"));
    ASSERT_EQ(value.kind(), MiValue::Kind::List);
    ASSERT_EQ(value.children().size(), 2u);
    // `frame={...}` inside the list is parsed as a named member, so the item's
    // *value* is the frame tuple itself.
    const MiValue& first = value.children().at(0).second;
    EXPECT_EQ(value.children().at(0).first, QStringLiteral("frame"));
    EXPECT_EQ(first.value(QStringLiteral("func")), QStringLiteral("main"));
}

TEST(DebugMi, AnEmptyDoneHasNoResults)
{
    const MiRecord record = parseMiRecord(QStringLiteral("3^done"));
    EXPECT_EQ(record.kind, MiRecord::Kind::Result);
    EXPECT_EQ(record.name, QStringLiteral("done"));
    EXPECT_EQ(record.results.kind(), MiValue::Kind::Const);
    EXPECT_TRUE(record.results.children().isEmpty());
}

TEST(DebugMi, ConsoleTargetAndLogRecordsCarryTheirPayload)
{
    EXPECT_EQ(parseMiRecord(QStringLiteral("~\"Reading symbols...\\n\"")).text,
              QStringLiteral("Reading symbols...\n"));
    const MiRecord target = parseMiRecord(QStringLiteral("@\"hello\\n\""));
    EXPECT_EQ(target.kind, MiRecord::Kind::Target);
    EXPECT_EQ(target.text, QStringLiteral("hello\n"));
    const MiRecord log = parseMiRecord(QStringLiteral("&\"warning: x\\n\""));
    EXPECT_EQ(log.kind, MiRecord::Kind::Log);
    EXPECT_EQ(log.text, QStringLiteral("warning: x\n"));
}

TEST(DebugMi, AConsoleRecordWithEmbeddedQuotesKeepsThem)
{
    // GDB escapes inner quotes; the body must come back with real quotes.
    const MiRecord record = parseMiRecord(QStringLiteral("~\"say \\\"hi\\\"\\n\""));
    EXPECT_EQ(record.text, QStringLiteral("say \"hi\"\n"));
}

TEST(DebugMi, NotifyAndStatusRecordsAreRecognised)
{
    const MiRecord notify =
        parseMiRecord(QStringLiteral("=thread-group-added,id=\"i1\""));
    EXPECT_EQ(notify.kind, MiRecord::Kind::NotifyAsync);
    EXPECT_EQ(notify.name, QStringLiteral("thread-group-added"));
    EXPECT_EQ(notify.results.value(QStringLiteral("id")), QStringLiteral("i1"));

    const MiRecord status = parseMiRecord(QStringLiteral("+download,{section=\"x\"}"));
    EXPECT_EQ(status.kind, MiRecord::Kind::StatusAsync);
    EXPECT_EQ(status.name, QStringLiteral("download"));
}

TEST(DebugMi, ThePromptIsItsOwnKind)
{
    const MiRecord record = parseMiRecord(QStringLiteral("(gdb)"));
    EXPECT_EQ(record.kind, MiRecord::Kind::Prompt);
}

TEST(DebugMi, GarbageSurvivesAsUnknownWithItsText)
{
    const MiRecord record = parseMiRecord(QStringLiteral("warning: File a.cpp not found"));
    EXPECT_EQ(record.kind, MiRecord::Kind::Unknown);
    EXPECT_EQ(record.text, QStringLiteral("warning: File a.cpp not found"));
}

TEST(DebugMi, OctalAndControlEscapesDecode)
{
    EXPECT_EQ(unescapeC(QStringLiteral("a\\101b")), QStringLiteral("aAb"));
    EXPECT_EQ(unescapeC(QStringLiteral("tab\\there")), QStringLiteral("tab\there"));
    // A genuine backslash arrives doubled (GDB escapes it), and an escape
    // the grammar does not define keeps its backslash rather than vanishing.
    EXPECT_EQ(unescapeC(QStringLiteral("C:\\\\temp")), QStringLiteral("C:\\temp"));
    EXPECT_EQ(unescapeC(QStringLiteral("a\\qb")), QStringLiteral("a\\qb"));
}

TEST(DebugMi, AFrameListWithAnAddressOnlyFrameIsStillAList)
{
    // `-stack-list-frames` emits addr-only frames for frames without debug
    // info; the parser must not drop them (the UI shows the address).
    const MiValue value = parseOne(
        QStringLiteral("stack=[frame={level=\"0\",addr=\"0x1122\",func=\"??\"}]"));
    const QList<LightCpp::App::Debug::FrameInfo> frames = framesFromMi(value);
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(frames.first().address, QStringLiteral("0x1122"));
    EXPECT_EQ(frames.first().function, QStringLiteral("??"));
    EXPECT_EQ(frames.first().line, 0);
}

TEST(DebugMi, FramesFromTheThreadNestedFormAreFlattened)
{
    // `-thread-apply-all -stack-list-frames` nests frames under threads.
    const MiValue value = parseOne(
        QStringLiteral("[{thread-id=\"1\",frame=[{level=\"0\",func=\"f\",file=\"a.cpp\",line=\"3\"}]},"
                       "{thread-id=\"2\",frame=[{level=\"0\",func=\"g\",file=\"b.cpp\",line=\"9\"}]}]"));
    const QList<LightCpp::App::Debug::FrameInfo> frames = framesFromMi(value);
    ASSERT_EQ(frames.size(), 2);
    EXPECT_EQ(frames.at(0).function, QStringLiteral("f"));
    EXPECT_EQ(frames.at(1).function, QStringLiteral("g"));
}

TEST(DebugMi, ANestedValueIsAddressableByPath)
{
    const MiValue value = parseOne(
        QStringLiteral("{a={b={c=\"deep\"}},list=[1,2,3]}"));
    const MiValue* a = value.find(QStringLiteral("a"));
    ASSERT_NE(a, nullptr);
    const MiValue* b = a->find(QStringLiteral("b"));
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(b->value(QStringLiteral("c")), QStringLiteral("deep"));
    EXPECT_EQ(value.find(QStringLiteral("list"))->children().size(), 3u);
}

TEST(DebugMi, NumberParsingReportsZeroForNonNumbers)
{
    const MiValue value = parseOne(QStringLiteral("{n=\"42\",s=\"x\"}"));
    EXPECT_EQ(value.number(QStringLiteral("n")), 42);
    EXPECT_EQ(value.number(QStringLiteral("s")), 0);
    EXPECT_EQ(value.number(QStringLiteral("absent")), 0);
}
