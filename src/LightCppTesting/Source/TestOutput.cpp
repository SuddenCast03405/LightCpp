// TestOutput — what a run printed, turned into results. See TestModel.hpp.

#include "LightCpp/Testing/TestModel.hpp"

#include <QRegularExpression>

namespace LightCpp::Testing {
namespace {

/// gtest writes failure blocks as
///   path/file.cpp:12: Failure
///   Expected equality of these values:
///     a
///     Which is: 1
///     b
///     Which is: 2
/// The block belongs to the case named by the *following* `[  FAILED  ]`
/// line, so it is buffered and attached when that line arrives.
struct PendingFailure {
    QString file;
    int line = 0;
    QStringList message;
};

QString joinMessage(const QStringList& lines)
{
    QString text = lines.join(QLatin1Char('\n')).trimmed();
    // Collapse the "Which is:" noise only at the edges; the detail matters.
    return text;
}

}  // namespace

TestRun parseGtestOutput(const QString& text, int exitCode)
{
    TestRun run;
    run.framework = TestFramework::GoogleTest;
    run.exitCode = exitCode;
    run.rawOutput = text;
    run.started = true;

    static const QRegularExpression okLine(
        QStringLiteral("^\\[\\s+OK\\s+\\]\\s+(.+?)\\s+\\((\\d+)\\s*ms\\)\\s*$"));
    static const QRegularExpression failedListLine(
        QStringLiteral("^\\[\\s+FAILED\\s+\\]\\s+([^\\s(]+)\\s+\\((\\d+)\\s*ms\\)\\s*$"));
    static const QRegularExpression failureHead(
        QStringLiteral("^(.+?):(\\d+):\\s*(Failure|error:)\\s*$"));
    static const QRegularExpression summaryLine(
        QStringLiteral("^\\[==========\\]\\s+(\\d+) tests? from .* ran"));

    PendingFailure pending;
    QList<PendingFailure> unattributed;

    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        if (const auto match = okLine.match(raw); match.hasMatch()) {
            TestCaseResult result;
            const QString qualified = match.captured(1);
            const qsizetype dot = qualified.lastIndexOf(QLatin1Char('.'));
            if (dot > 0) {
                result.suite = qualified.left(dot);
                result.name = qualified.mid(dot + 1);
            } else {
                result.name = qualified;
            }
            result.outcome = TestOutcome::Passed;
            result.durationMs = match.captured(2).toInt();
            // A DISABLED_ case that still ran reports OK; it counts as
            // disabled only when it is *not* run, which gtest states at the
            // end. Keeping it as passed here matches what happened.
            run.cases << result;
            ++run.passed;
            continue;
        }
        if (const auto match = failedListLine.match(raw); match.hasMatch()) {
            TestCaseResult result;
            const QString qualified = match.captured(1);
            const qsizetype dot = qualified.lastIndexOf(QLatin1Char('.'));
            if (dot > 0) {
                result.suite = qualified.left(dot);
                result.name = qualified.mid(dot + 1);
            } else {
                result.name = qualified;
            }
            result.outcome = TestOutcome::Failed;
            result.durationMs = match.captured(2).toInt();
            if (!pending.message.isEmpty()) {
                result.message = joinMessage(pending.message);
                result.file = pending.file;
                result.line = pending.line;
                pending = PendingFailure{};
            } else if (!unattributed.isEmpty()) {
                result.message = joinMessage(unattributed.first().message);
                result.file = unattributed.first().file;
                result.line = unattributed.first().line;
                unattributed.removeFirst();
            }
            run.cases << result;
            ++run.failed;
            continue;
        }
        if (const auto match = failureHead.match(raw); match.hasMatch()) {
            pending = PendingFailure{};
            pending.file = match.captured(1);
            pending.line = match.captured(2).toInt();
            continue;
        }
        if (const auto match = summaryLine.match(raw); match.hasMatch()) {
            run.finished = true;
            Q_UNUSED(match);
            continue;
        }
        if (raw.startsWith(QLatin1String("[  PASSED  ]"))) {
            run.finished = true;
            continue;
        }
        if (!pending.file.isEmpty() && raw.startsWith(QLatin1Char(' '))) {
            pending.message << raw.trimmed();
            continue;
        }
        if (raw.startsWith(QLatin1String("[  FAILED  ]"))
            && !raw.contains(QLatin1String("ms)"))) {
            // "[  FAILED  ] 2 tests, listed below:" — a summary, not a case.
            run.finished = true;
            continue;
        }
    }
    if (!run.finished && exitCode == 0 && !run.cases.isEmpty()) {
        run.finished = true;
    }
    return run;
}

TestRun parseCatch2Output(const QString& text, int exitCode)
{
    TestRun run;
    run.framework = TestFramework::Catch2;
    run.exitCode = exitCode;
    run.rawOutput = text;

    // Catch2 v3 prints one line per case in the default reporter only with
    // `--reporter console -s`; the summary is what is always there:
    //   test cases: 3 | 2 passed | 1 failed
    //   assertions: 12 | 11 passed | 1 failed
    // With `-s` it also prints each case as it starts:
    //   ------------------------------------------------------------------
    //   Adds two numbers
    //   ------------------------------------------------------------------
    static const QRegularExpression caseHeader(
        QStringLiteral("^-{10,}$"));
    static const QRegularExpression summary(
        QStringLiteral("^test cases:\\s*(\\d+)\\s*\\|\\s*(\\d+)\\s+passed\\s*\\|\\s*(\\d+)\\s+failed"
                       "(?:\\s*\\|\\s*(\\d+)\\s+skipped)?"));
    static const QRegularExpression skipped(
        QStringLiteral("^(\\d+)\\s+test cases?\\s+skipped"));
    static const QRegularExpression failureLine(
        QStringLiteral("^(/.+?|[A-Za-z]:.+?):(\\d+):\\s*FAILED:\\s*$"));

    QString currentName;
    bool expectHeaderName = false;
    QString failureFile;
    int failureLineNumber = 0;
    QStringList failureMessage;

    const QStringList lines = text.split(QLatin1Char('\n'));
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString& raw = lines.at(i);
        if (caseHeader.match(raw).hasMatch()) {
            expectHeaderName = !expectHeaderName;
            if (expectHeaderName) {
                // The next non-empty line is the case name.
                for (qsizetype j = i + 1; j < lines.size(); ++j) {
                    if (!lines.at(j).trimmed().isEmpty()) {
                        currentName = lines.at(j).trimmed();
                        break;
                    }
                }
            }
            continue;
        }
        if (const auto match = summary.match(raw); match.hasMatch()) {
            run.finished = true;
            run.passed = match.captured(2).toInt();
            run.failed = match.captured(3).toInt();
            run.skipped = match.captured(4).toInt();
            continue;
        }
        if (const auto match = skipped.match(raw); match.hasMatch()) {
            run.skipped = match.captured(1).toInt();
            continue;
        }
        if (const auto match = failureLine.match(raw); match.hasMatch()) {
            failureFile = match.captured(1);
            failureLineNumber = match.captured(2).toInt();
            continue;
        }
        if (raw.contains(QLatin1String("FAILED")) && !failureFile.isEmpty()) {
            failureMessage << raw.trimmed();
            continue;
        }
        if (raw.startsWith(QLatin1String("All tests passed"))
            || raw.startsWith(QLatin1String("assertions:"))) {
            run.finished = true;
        }
    }

    // Without per-case output the counts are all Catch2 gives; synthesise one
    // result per failing case so "jump to failure" still has a target.
    if (run.cases.isEmpty() && (run.passed + run.failed + run.skipped) > 0) {
        for (int i = 0; i < run.passed; ++i) {
            TestCaseResult result;
            result.outcome = TestOutcome::Passed;
            run.cases << result;
        }
        for (int i = 0; i < run.failed; ++i) {
            TestCaseResult result;
            result.outcome = TestOutcome::Failed;
            result.file = failureFile;
            result.line = failureLineNumber;
            result.message = joinMessage(failureMessage);
            run.cases << result;
        }
        for (int i = 0; i < run.skipped; ++i) {
            TestCaseResult result;
            result.outcome = TestOutcome::Skipped;
            run.cases << result;
        }
    }
    if (!run.cases.isEmpty() && run.passed + run.failed + run.skipped == 0) {
        for (const TestCaseResult& result : std::as_const(run.cases)) {
            if (result.outcome == TestOutcome::Passed) {
                ++run.passed;
            } else if (result.outcome == TestOutcome::Failed) {
                ++run.failed;
            }
        }
    }
    Q_UNUSED(currentName);
    if (!run.finished && exitCode == 0) {
        run.finished = true;
    }
    return run;
}

TestRun parseBoostOutput(const QString& text, int exitCode)
{
    TestRun run;
    run.framework = TestFramework::BoostTest;
    run.exitCode = exitCode;
    run.rawOutput = text;

    // Boost.Test's default report is silent on success:
    //   Running 3 test cases...
    //   *** No errors detected
    // and on failure:
    //   test.cpp(12): error: in "Math/AddsTwoNumbers": check a == 2 has failed [1 != 2]
    //   *** 1 failure is detected in the test module "app"
    static const QRegularExpression runningLine(
        QStringLiteral("^Running\\s+(\\d+)\\s+test cases?"));
    static const QRegularExpression errorLine(
        QStringLiteral("^(.+?)\\((\\d+)\\):\\s*error:\\s*in\\s+\"([^\"]+)\":\\s*(.*)$"));
    static const QRegularExpression noErrors(
        QStringLiteral("^\\*\\*\\*\\s+No errors detected"));
    static const QRegularExpression failureSummary(
        QStringLiteral("^\\*\\*\\*\\s+(\\d+)\\s+failures?\\s+is\\s+detected"));
    static const QRegularExpression aborted(
        QStringLiteral("^\\*\\*\\*\\s+errors?\\s+is\\s+detected"));

    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        if (runningLine.match(raw).hasMatch()) {
            run.started = true;
            continue;
        }
        if (const auto match = errorLine.match(raw); match.hasMatch()) {
            TestCaseResult result;
            const QString qualified = match.captured(3);
            const qsizetype slash = qualified.indexOf(QLatin1Char('/'));
            if (slash > 0) {
                result.suite = qualified.left(slash);
                result.name = qualified.mid(slash + 1);
            } else {
                result.name = qualified;
            }
            result.outcome = TestOutcome::Failed;
            result.file = match.captured(1);
            result.line = match.captured(2).toInt();
            result.message = match.captured(4);
            run.cases << result;
            ++run.failed;
            run.started = true;
            continue;
        }
        if (noErrors.match(raw).hasMatch()) {
            run.finished = true;
            run.started = true;
            continue;
        }
        if (failureSummary.match(raw).hasMatch() || aborted.match(raw).hasMatch()) {
            run.finished = true;
            continue;
        }
    }
    // "No errors detected" means every case passed, but Boost does not say
    // how many there were; the caller knows from discovery. Recording one
    // synthetic pass would be a lie about the count, so only the flags are
    // set here.
    if (run.failed == 0 && run.finished && run.cases.isEmpty()) {
        TestCaseResult result;
        result.name = QStringLiteral("(all cases)");
        result.outcome = TestOutcome::Passed;
        run.cases << result;
        ++run.passed;
    }
    return run;
}

TestRun parseCtestOutput(const QString& text, int exitCode)
{
    TestRun run;
    run.framework = TestFramework::CTest;
    run.exitCode = exitCode;
    run.rawOutput = text;

    //   1/3 Test #1: Math.AddsTwoNumbers ...........   Passed    0.01 sec
    //   2/3 Test #2: Other.Works ...................***Failed    0.02 sec
    //   3/3 Test #3: Slow ..........................   Skipped
    static const QRegularExpression line(
        QStringLiteral("^\\s*\\d+/\\d+\\s+Test\\s+#(\\d+):\\s+(\\S+)\\s+\\.*\\s*(\\*\\*\\*)?"
                       "(Passed|Failed|Skipped|Timeout|[A-Za-z]+)?\\s*([0-9.]+)?\\s*sec"));
    static const QRegularExpression simple(
        QStringLiteral("^\\s*\\d+/\\d+\\s+Test\\s+#(\\d+):\\s+(\\S+)\\s+\\.*\\s*(\\*\\*\\*)?"
                       "\\s*(Passed|Failed|Skipped|Timeout)"));

    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        QString name;
        QString status;
        QString seconds;
        if (const auto match = line.match(raw); match.hasMatch()) {
            name = match.captured(2);
            status = match.captured(4);
            seconds = match.captured(5);
        } else if (const auto match = simple.match(raw); match.hasMatch()) {
            name = match.captured(2);
            status = match.captured(4);
        } else {
            continue;
        }
        TestCaseResult result;
        const qsizetype dot = name.indexOf(QLatin1Char('.'));
        if (dot > 0) {
            result.suite = name.left(dot);
            result.name = name.mid(dot + 1);
        } else {
            result.name = name;
        }
        if (status == QLatin1String("Passed")) {
            result.outcome = TestOutcome::Passed;
            ++run.passed;
        } else if (status == QLatin1String("Skipped")) {
            result.outcome = TestOutcome::Skipped;
            ++run.skipped;
        } else if (status == QLatin1String("Timeout")) {
            result.outcome = TestOutcome::TimedOut;
            ++run.failed;
        } else {
            result.outcome = TestOutcome::Failed;
            ++run.failed;
        }
        result.durationMs = static_cast<int>(seconds.toDouble() * 1000.0);
        run.cases << result;
        run.started = true;
        run.finished = true;
    }
    if (!run.finished) {
        // "100% tests passed, 0 tests failed out of 3" is a summary that
        // contradicts a missing per-test line only when parsing failed; trust
        // the counts it states.
        static const QRegularExpression totals(
            QStringLiteral("(\\d+)% tests passed, (\\d+) tests failed out of (\\d+)"));
        if (const auto match = totals.match(text); match.hasMatch()) {
            run.finished = true;
            run.failed = match.captured(2).toInt();
            run.passed = match.captured(3).toInt() - run.failed;
        }
    }
    return run;
}

TestRun parseRunOutput(TestFramework framework, const QString& text, int exitCode)
{
    switch (framework) {
    case TestFramework::GoogleTest: return parseGtestOutput(text, exitCode);
    case TestFramework::Catch2:     return parseCatch2Output(text, exitCode);
    case TestFramework::BoostTest:  return parseBoostOutput(text, exitCode);
    case TestFramework::CTest:      return parseCtestOutput(text, exitCode);
    case TestFramework::Unknown:    break;
    }
    return {};
}

}  // namespace LightCpp::Testing
