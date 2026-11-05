// DebugViewsModelTests — the request/answer protocol per backend.
//
// No debugger runs here: the sink records what the model would have typed,
// and the test feeds back the transcript that debugger would have printed.
// That is enough to pin the two things that actually break:
//
//   1. the command for a view is the one that backend understands
//      (`-stack-list-frames`, `thread backtrace -c`, `~*k`, ...)
//   2. an answer lands in the right snapshot, and a *rejected* answer does
//      not (a `^error` must leave the previous view intact — a stale stack
//      that looks current is worse than an empty one)
//
// The token rule matters for the same reason: GDB answers out of order and
// an untagged `^done` belongs to nothing.

#include <gtest/gtest.h>

#include "LightCpp/App/Debug/DebugViewsModel.hpp"

namespace {

using LightCpp::App::Debug::Backend;
using LightCpp::App::Debug::DebugViewsModel;

class Harness {
public:
    explicit Harness(Backend backend)
    {
        model.setBackend(backend);
        model.setTokenAllocator([this] { return nextToken_++; });
        model.setCommandSink([this](const QString& command) { sent << command; });
    }

    QStringList take()
    {
        const QStringList copy = sent;
        sent.clear();
        return copy;
    }

    DebugViewsModel model;
    QStringList sent;

private:
    quint64 nextToken_ = 1;
};

}  // namespace

TEST(DebugViewsModel, GdbQueriesAreTaggedAndSpeakMi)
{
    Harness harness(Backend::Gdb);
    harness.model.requestCallStack(20);
    EXPECT_EQ(harness.take(), QStringList{QStringLiteral("1-stack-list-frames 0 20")});

    harness.model.requestThreads();
    EXPECT_EQ(harness.take(), QStringList{QStringLiteral("2-thread-info")});

    harness.model.requestLocals();
    EXPECT_EQ(harness.take(),
              QStringList{QStringLiteral("3-stack-list-variables --all-values")});

    harness.model.requestMemory(QStringLiteral("0x1000"), 32);
    EXPECT_EQ(harness.take(),
              QStringList{QStringLiteral("4-data-read-memory-bytes 0x1000 32")});

    harness.model.requestDisassembly(QStringLiteral("main"), 10);
    EXPECT_EQ(harness.take(),
              QStringList{QStringLiteral("5-data-disassemble -a main -- 0")});
}

TEST(DebugViewsModel, LldbAndCdbUseTheirOwnViewCommands)
{
    {
        Harness lldb(Backend::Lldb);
        lldb.model.requestCallStack(20);
        lldb.model.requestThreads();
        lldb.model.requestLocals();
        lldb.model.requestRegisters();
        lldb.model.requestMemory(QStringLiteral("$rsp"), 16);
        EXPECT_EQ(lldb.take(),
                  (QStringList{QStringLiteral("thread backtrace -c 20"),
                               QStringLiteral("thread list"),
                               QStringLiteral("frame variable"),
                               QStringLiteral("register read"),
                               QStringLiteral("memory read -c 16 $rsp")}));
    }
    {
        Harness cdb(Backend::Cdb);
        cdb.model.requestCallStack();
        cdb.model.requestThreads();
        cdb.model.requestLocals();
        cdb.model.requestRegisters();
        cdb.model.requestMemory(QStringLiteral("esp"), 16);
        EXPECT_EQ(cdb.take(),
                  (QStringList{QStringLiteral("k"),
                               QStringLiteral("~*k"),
                               QStringLiteral("dv /t /v"),
                               QStringLiteral("r"),
                               QStringLiteral("db esp L?16")}));
    }
}

TEST(DebugViewsModel, GdbRegisterNamesAreAskedForOnceThenReused)
{
    Harness harness(Backend::Gdb);
    harness.model.requestRegisters();
    EXPECT_EQ(harness.take(),
              QStringList{QStringLiteral("1-data-list-register-names")});

    // Answering the names query must immediately ask for the values.
    harness.model.observeLine(
        QStringLiteral("1^done,register-names=[\"rax\",\"rbx\",\"\",\"\",\"rip\"]"));
    EXPECT_EQ(harness.take(),
              QStringList{QStringLiteral("2-data-list-register-values x")});

    harness.model.observeLine(
        QStringLiteral("2^done,register-values=[{number=\"0\",value=\"0x3\"},"
                       "{number=\"4\",value=\"0x401000\"}]"));
    ASSERT_EQ(harness.model.registers().size(), 2);
    EXPECT_EQ(harness.model.registers().at(0).name, QStringLiteral("rax"));
    EXPECT_EQ(harness.model.registers().at(1).name, QStringLiteral("rip"));
}

TEST(DebugViewsModel, AGdbStackAnswerFillsTheCallStack)
{
    Harness harness(Backend::Gdb);
    harness.model.requestCallStack();
    harness.take();
    harness.model.observeLine(
        QStringLiteral("1^done,stack=[frame={level=\"0\",addr=\"0x1\",func=\"main\","
                       "file=\"/tmp/a.cpp\",line=\"8\"},"
                       "frame={level=\"1\",addr=\"0x2\",func=\"start\",line=\"0\"}]"));
    ASSERT_EQ(harness.model.frames().size(), 2);
    EXPECT_EQ(harness.model.frames().at(0).function, QStringLiteral("main"));
    EXPECT_EQ(harness.model.frames().at(1).line, 0);
}

TEST(DebugViewsModel, AnErrorAnswerReleasesTheTokenAndKeepsTheOldView)
{
    Harness harness(Backend::Gdb);
    harness.model.requestCallStack();
    harness.take();
    harness.model.observeLine(
        QStringLiteral("1^done,stack=[frame={level=\"0\",func=\"main\",line=\"8\"}]"));
    ASSERT_EQ(harness.model.frames().size(), 1);

    harness.model.requestCallStack();
    harness.take();
    harness.model.observeLine(QStringLiteral("2^error,msg=\"No stack.\""));
    // The previous answer stands: an empty stack would read as "no frames"
    // when the truth is "the query failed".
    EXPECT_EQ(harness.model.frames().size(), 1);

    // And the next query gets a fresh token, not the failed one.
    harness.model.requestThreads();
    EXPECT_EQ(harness.take(), QStringList{QStringLiteral("3-thread-info")});
}

TEST(DebugViewsModel, AnAnswerForAnotherTokenIsIgnored)
{
    Harness harness(Backend::Gdb);
    harness.model.requestCallStack();
    harness.take();
    // Token 99 is not ours (it belongs to a breakpoint command).
    harness.model.observeLine(QStringLiteral("99^done,stack=[frame={level=\"0\",func=\"x\"}]"));
    EXPECT_TRUE(harness.model.frames().isEmpty());
}

TEST(DebugViewsModel, EvaluationReportsTheExpressionItAnswered)
{
    Harness harness(Backend::Gdb);
    QString answered;
    QString value;
    bool ok = false;
    QObject::connect(&harness.model, &DebugViewsModel::evaluationFinished,
                     [&](const QString& expression, const QString& text, bool success) {
                         answered = expression;
                         value = text;
                         ok = success;
                     });
    harness.model.evaluate(QStringLiteral("x + y"));
    EXPECT_EQ(harness.take(),
              QStringList{QStringLiteral("1-data-evaluate-expression \"x + y\"")});
    harness.model.observeLine(QStringLiteral("1^done,value=\"7\""));
    EXPECT_EQ(answered, QStringLiteral("x + y"));
    EXPECT_EQ(value, QStringLiteral("7"));
    EXPECT_TRUE(ok);

    harness.model.evaluate(QStringLiteral("nope"));
    harness.take();
    harness.model.observeLine(QStringLiteral("2^error,msg=\"No symbol nope\""));
    EXPECT_EQ(answered, QStringLiteral("nope"));
    EXPECT_FALSE(ok);
}

TEST(DebugViewsModel, AWatchedVariableKeepsItsChildren)
{
    Harness harness(Backend::Gdb);
    harness.model.addWatch(QStringLiteral("m"));
    EXPECT_EQ(harness.take(), QStringList{QStringLiteral("1-var-create - * \"m\"")});

    harness.model.observeLine(
        QStringLiteral("1^done,name=\"var1\",numchild=\"2\",value=\"{...}\","
                       "type=\"std::map<int,int>\",has_more=\"0\""));
    ASSERT_EQ(harness.model.watches().size(), 1);
    EXPECT_TRUE(harness.model.watches().first().hasChildren);
    EXPECT_EQ(harness.model.watches().first().type,
              QStringLiteral("std::map<int,int>"));

    harness.model.expandVariable(QStringLiteral("m"), QStringLiteral("var1"));
    EXPECT_EQ(harness.take(),
              QStringList{QStringLiteral("2-var-list-children --all-values \"m\"")});

    harness.model.observeLine(
        QStringLiteral("2^done,numchild=\"2\",children=["
                       "child={name=\"var1.[1]\",exp=\"[1]\",numchild=\"0\","
                       "value=\"10\",type=\"int\"},"
                       "child={name=\"var1.[2]\",exp=\"[2]\",numchild=\"0\","
                       "value=\"20\",type=\"int\"}]"));
    ASSERT_EQ(harness.model.watches().first().children.size(), 2);
    EXPECT_EQ(harness.model.watches().first().children.at(1).value,
              QStringLiteral("20"));
}

TEST(DebugViewsModel, ALldbAnswerEndsAtThePrompt)
{
    Harness harness(Backend::Lldb);
    harness.model.requestLocals();
    harness.take();
    harness.model.observeLine(QStringLiteral("    (int) i = 3"));
    harness.model.observeLine(QStringLiteral("    (double) d = 1.5"));
    // Nothing published yet: the answer is only complete at the prompt.
    EXPECT_TRUE(harness.model.locals().isEmpty());
    harness.model.observeLine(QStringLiteral("(lldb) "));
    ASSERT_EQ(harness.model.locals().size(), 2);
    EXPECT_EQ(harness.model.locals().at(1).name, QStringLiteral("d"));
}

TEST(DebugViewsModel, ACdbAnswerEndsAtItsPromptToo)
{
    Harness harness(Backend::Cdb);
    harness.model.requestLocals();
    harness.take();
    harness.model.observeLine(QStringLiteral("    int i = 0n3"));
    harness.model.observeLine(QStringLiteral("0:000> "));
    ASSERT_EQ(harness.model.locals().size(), 1);
    EXPECT_EQ(harness.model.locals().first().value, QStringLiteral("0n3"));
}

TEST(DebugViewsModel, SelectingAFrameRebuildsTheLocals)
{
    Harness harness(Backend::Gdb);
    harness.model.selectFrame(2);
    EXPECT_EQ(harness.take(), QStringList{QStringLiteral("1-stack-select-frame 2")});
    harness.model.observeLine(QStringLiteral("1^done"));
    EXPECT_EQ(harness.take(),
              QStringList{QStringLiteral("2-stack-list-variables --all-values")});
}

TEST(DebugViewsModel, AttachRemoteAndCoreSpeakPerBackend)
{
    {
        Harness gdb(Backend::Gdb);
        EXPECT_EQ(gdb.model.attachCommands(4242),
                  QStringList{QStringLiteral("-target-attach 4242")});
        EXPECT_EQ(gdb.model.remoteCommands(QStringLiteral("localhost:1234")),
                  QStringList{QStringLiteral("-target-select remote localhost:1234")});
        EXPECT_EQ(gdb.model.coreCommands(QStringLiteral("/tmp/core.1")),
                  QStringList{QStringLiteral("-target-select core \"/tmp/core.1\"")});
    }
    {
        Harness lldb(Backend::Lldb);
        EXPECT_EQ(lldb.model.attachCommands(7),
                  QStringList{QStringLiteral("process attach --pid 7")});
        EXPECT_EQ(lldb.model.coreCommands(QStringLiteral("/tmp/core.1")),
                  QStringList{QStringLiteral("target create --core \"/tmp/core.1\"")});
    }
    {
        Harness cdb(Backend::Cdb);
        // cdb takes the pid as a launch flag, not a command.
        EXPECT_TRUE(cdb.model.attachCommands(7).isEmpty());
        EXPECT_EQ(cdb.model.coreCommands(QStringLiteral("C:/core.dmp")),
                  QStringList{QStringLiteral(".opendump \"C:/core.dmp\"")});
    }
}

TEST(DebugViewsModel, RefreshOnStopAsksForEveryCheapViewInOrder)
{
    Harness harness(Backend::Gdb);
    harness.model.refreshOnStop();
    EXPECT_EQ(harness.take(),
              (QStringList{QStringLiteral("1-stack-list-frames 0 40"),
                           QStringLiteral("2-thread-info"),
                           QStringLiteral("3-stack-list-variables --all-values"),
                           QStringLiteral("4-data-list-register-names")}));
}

TEST(DebugViewsModel, ClearDropsEverySnapshotButKeepsTheWatches)
{
    Harness harness(Backend::Gdb);
    harness.model.addWatch(QStringLiteral("x"));
    harness.model.observeLine(
        QStringLiteral("1^done,name=\"var1\",numchild=\"0\",value=\"3\""));
    harness.model.requestCallStack();
    harness.model.observeLine(
        QStringLiteral("2^done,stack=[frame={level=\"0\",func=\"main\"}]"));
    ASSERT_EQ(harness.model.frames().size(), 1);

    harness.model.clear();
    EXPECT_TRUE(harness.model.frames().isEmpty());
    EXPECT_EQ(harness.model.watches().size(), 1);
    EXPECT_FALSE(harness.model.watches().first().valid);
}
