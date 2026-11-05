// LightCppAppTests — the App layer's first suite.
//
// The App layer is mostly Qt widgets, which is why it had no suite; the parts
// that are not widgets but state machines over QProcess are the ones that can
// break without anything on screen saying so, and those are what belongs here.
// CodeFormatter is the first (R2-07).
//
// A note on what needs clang-format: the command-line tests run anywhere, and
// the process tests skip when no clang-format can be found rather than failing
// a machine that simply does not have one.

#include "LightCpp/App/CodeFormatter.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <optional>

using LightCpp::App::CodeFormatter;

namespace {

constexpr int kAnswerTimeoutMs = 8000;

bool haveClangFormat()
{
    return CodeFormatter::isAvailable();
}

/// Whether this host lets a process create its pipes at all.
///
/// Not a given on this machine: started from an environment whose handle table
/// is exhausted, QProcess reports "CreateFile failed. (all pipe instances are
/// in use.)" for *every* child — the same thing that stops windeployqt and
/// once made the language server fail to start. Nothing here is a test of that,
/// so the process tests stand down in such an environment instead of reporting
/// a formatter fault that is not one.
bool canSpawnProcesses()
{
    const QString tool = CodeFormatter::toolPath();
    if (tool.isEmpty()) {
        return false;
    }
    QProcess probe;
    probe.setProgram(tool);
    probe.setArguments({QStringLiteral("--version")});
    probe.start();
    if (!probe.waitForStarted(5000)) {
        return false;
    }
    probe.closeWriteChannel();
    probe.waitForFinished(5000);
    return true;
}

/// Why the process tests cannot run here, or nullopt when they can.
std::optional<QString> processTestBlocker()
{
    if (!haveClangFormat()) {
        return QStringLiteral("no clang-format on this machine");
    }
    if (!canSpawnProcesses()) {
        return QStringLiteral(
            "this environment cannot create process pipes (QProcess: "
            "CreateFile failed), so no child process can be run");
    }
    return std::nullopt;
}

/// Large enough that clang-format is still running when the next request
/// arrives. That is what makes the second request *supersede* the first
/// instead of queueing behind it, and the supersede path is the one that used
/// to leave the formatter unable to answer anything ever again.
QString bigBuffer()
{
    QString text;
    text.reserve(700 * 1024);
    for (int i = 0; i < 20000; ++i) {
        text += QStringLiteral("int   value_%1   =   %1;\n").arg(i);
    }
    return text;
}

/// Run the event loop until the formatter answers, or the deadline passes.
/// Returns the id that was formatted, or -1 when nothing came back in time.
/// A failure counts as an answer for the deadline's purposes and has its id
/// collected in `refused`, so the caller can tell "wrong answer" from "no
/// answer at all".
int waitForAnswer(CodeFormatter* formatter, QList<int>* refused)
{
    int formatted = -1;
    QEventLoop loop;
    const QMetaObject::Connection onFormatted = QObject::connect(
        formatter, &CodeFormatter::formatted, &loop,
        [&formatted, &loop](int id, const QString&) {
            formatted = id;
            loop.quit();
        });
    const QMetaObject::Connection onFailed = QObject::connect(
        formatter, &CodeFormatter::failed, &loop,
        [refused, &loop](int id, const QString&) {
            if (refused != nullptr) {
                refused->append(id);
            }
            loop.quit();
        });
    QTimer::singleShot(kAnswerTimeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    QObject::disconnect(onFormatted);
    QObject::disconnect(onFailed);
    return formatted;
}

}  // namespace

TEST(CodeFormatterArguments, NameTheStyleAndTheAssumedFile)
{
    const QStringList args = CodeFormatter::argumentsFor(QStringLiteral("a.cpp"));

    EXPECT_TRUE(args.contains(QStringLiteral("-style=file")));
    EXPECT_TRUE(args.contains(
        QStringLiteral("--fallback-style=") + QString::fromLatin1(CodeFormatter::kFallbackStyle)));
    EXPECT_TRUE(args.contains(QStringLiteral("--assume-filename=a.cpp")));
}

TEST(CodeFormatterArguments, OmitTheAssumedFileWhenTheBufferHasNone)
{
    const QStringList args = CodeFormatter::argumentsFor(QString());

    for (const QString& arg : args) {
        // toStdString(), not the QString itself: gtest prints through
        // std::ostream, and Qt's own operator<< takes a QDebug.
        EXPECT_FALSE(arg.startsWith(QStringLiteral("--assume-filename")))
            << arg.toStdString();
    }
}

TEST(CodeFormatterArguments, FileAsAFallbackStyleKeepsTheBuiltInOne)
{
    CodeFormatter formatter;
    // "file" is what -style=file already does, so passing it on would be a
    // no-op dressed up as a setting; the built-in preset stays in charge.
    formatter.setFallbackStyle(QStringLiteral("file"));
    EXPECT_EQ(formatter.resolvedFallbackStyle(),
              QString::fromLatin1(CodeFormatter::kFallbackStyle));

    formatter.setFallbackStyle(QStringLiteral("Google"));
    EXPECT_EQ(formatter.resolvedFallbackStyle(), QStringLiteral("Google"));

    formatter.setFallbackStyle(QString());
    EXPECT_EQ(formatter.resolvedFallbackStyle(),
              QString::fromLatin1(CodeFormatter::kFallbackStyle));
}

TEST(CodeFormatterArguments, ToolPathOverrideWinsOverTheBuiltInLocation)
{
    CodeFormatter formatter;
    formatter.setToolPath(QStringLiteral("D:/tools/clang-format.exe"));
    EXPECT_EQ(formatter.resolvedToolPath(), QStringLiteral("D:/tools/clang-format.exe"));

    formatter.setToolPath(QString());
    EXPECT_EQ(formatter.resolvedToolPath(), CodeFormatter::toolPath());
}

// ---------------------------------------------------------------------------
// The process state machine
// ---------------------------------------------------------------------------

TEST(CodeFormatterProcess, AnswersTheRequestThatSupersedesAnother)
{
    if (const std::optional<QString> blocker = processTestBlocker()) {
        GTEST_SKIP() << blocker->toStdString();
    }
    CodeFormatter formatter;
    QList<int> refused;
    QObject::connect(&formatter, &CodeFormatter::failed,
                     [&refused](int id, const QString&) { refused.append(id); });

    const QString text = bigBuffer();
    const int superseded = formatter.format(text, QStringLiteral("first.cpp"));
    const int current = formatter.format(text, QStringLiteral("second.cpp"));
    ASSERT_NE(superseded, current);

    // The current request must be answered on its own. Superseding used to
    // leave the QProcess alive but with every signal disconnected, and
    // format() only reconnects when its pointer is null — so from the second
    // request on, clang-format ran, its output went nowhere, and the only
    // thing the user ever saw was the 15 s timeout blaming clang-format.
    const int answered = waitForAnswer(&formatter, &refused);
    EXPECT_EQ(answered, current);
    EXPECT_FALSE(refused.contains(current));
    // ...and the abandoned run must not report back as if it were current.
    EXPECT_NE(answered, superseded);
}

TEST(CodeFormatterProcess, KeepsAnsweringAfterASupersede)
{
    if (const std::optional<QString> blocker = processTestBlocker()) {
        GTEST_SKIP() << blocker->toStdString();
    }
    CodeFormatter formatter;
    QList<int> refused;
    QObject::connect(&formatter, &CodeFormatter::failed,
                     [&refused](int id, const QString&) { refused.append(id); });

    const QString text = bigBuffer();
    formatter.format(text, QStringLiteral("first.cpp"));
    const int second = formatter.format(text, QStringLiteral("second.cpp"));
    EXPECT_EQ(waitForAnswer(&formatter, &refused), second);

    // A third request, after the one that superseded. This is the part that
    // stayed broken for the rest of the session: the supersede had already
    // spent the only reconnect.
    const int third = formatter.format(text, QStringLiteral("third.cpp"));
    EXPECT_EQ(waitForAnswer(&formatter, &refused), third);
    EXPECT_TRUE(refused.isEmpty());
}

TEST(CodeFormatterProcess, FormatsTextWhenNothingIsInFlight)
{
    if (const std::optional<QString> blocker = processTestBlocker()) {
        GTEST_SKIP() << blocker->toStdString();
    }
    CodeFormatter formatter;
    QString formattedText;
    QObject::connect(&formatter, &CodeFormatter::formatted,
                     [&formattedText](int, const QString& result) {
                         formattedText = result;
                     });

    const int id = formatter.format(QStringLiteral("int   a=1;\n"),
                                    QStringLiteral("plain.cpp"));
    QList<int> refused;
    EXPECT_EQ(waitForAnswer(&formatter, &refused), id);
    EXPECT_TRUE(refused.isEmpty());
    // clang-format's Microsoft style collapses the extra spaces.
    EXPECT_TRUE(formattedText.contains(QStringLiteral("int a = 1;")));
    EXPECT_FALSE(formattedText.contains(QStringLiteral("int   a")));
}

int main(int argc, char** argv)
{
    // A QCoreApplication, not a QApplication: these run headless and none of
    // them may drag in the GUI stack.
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
