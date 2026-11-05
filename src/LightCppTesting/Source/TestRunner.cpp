// TestRunner — implementation. See the header for the per-framework forms.

#include "LightCpp/Testing/TestRunner.hpp"

#include <QDir>

namespace LightCpp::Testing {
namespace {

QString quoteIfNeeded(const QString& argument)
{
    if (argument.isEmpty()) {
        return QStringLiteral("\"\"");
    }
    const bool needsQuotes = argument.contains(QLatin1Char(' '))
                             || argument.contains(QLatin1Char('\t'))
                             || argument.contains(QLatin1Char('"'));
    if (!needsQuotes) {
        return argument;
    }
    QString escaped = argument;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

/// gtest's filter syntax: `*` and `?` are wildcards, so a literal one in a
/// test name has to be neutralised or the filter selects more than asked.
QString escapeGtestPattern(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar ch : text) {
        if (ch == QLatin1Char('*') || ch == QLatin1Char('?') || ch == QLatin1Char('\\')) {
            out.append(QLatin1Char('\\'));
        }
        out.append(ch);
    }
    return out;
}

}  // namespace

QString TestRunPlan::commandLine() const
{
    QStringList parts;
    parts << quoteIfNeeded(program);
    for (const QString& argument : arguments) {
        parts << quoteIfNeeded(argument);
    }
    return parts.join(QLatin1Char(' '));
}

QStringList filterArguments(TestFramework framework, const TestCase& test)
{
    if (test.name.isEmpty() && test.suite.isEmpty()) {
        return {};
    }
    switch (framework) {
    case TestFramework::GoogleTest: {
        // gtest names the case `Suite.Case`; when discovery gave only a case
        // name, filter on the case across suites with a leading wildcard.
        const QString pattern = test.suite.isEmpty()
            ? QStringLiteral("*.") + escapeGtestPattern(test.name)
            : escapeGtestPattern(test.suite) + QLatin1Char('.')
                  + escapeGtestPattern(test.name);
        return {QStringLiteral("--gtest_filter=%1").arg(pattern)};
    }
    case TestFramework::Catch2:
        // A positional argument; the whole qualified name stays one argument.
        return {test.qualifiedName(TestFramework::Catch2)};
    case TestFramework::BoostTest:
        return {QStringLiteral("--run_test=%1")
                    .arg(test.qualifiedName(TestFramework::BoostTest))};
    case TestFramework::CTest:
        return {QStringLiteral("-R"), ctestNamePattern(test), QStringLiteral("--output-on-failure")};
    case TestFramework::Unknown:
        break;
    }
    return {};
}

QStringList rerunFailedArguments(TestFramework framework, const TestRun& run)
{
    QStringList failedNames;
    for (const TestCaseResult& result : run.cases) {
        if (result.outcome != TestOutcome::Failed
            && result.outcome != TestOutcome::TimedOut) {
            continue;
        }
        if (result.name.isEmpty()) {
            continue;
        }
        failedNames << (result.suite.isEmpty()
                            ? result.name
                            : result.suite + QLatin1Char('/') + result.name);
    }
    if (failedNames.isEmpty()) {
        return {};
    }
    switch (framework) {
    case TestFramework::GoogleTest: {
        // One filter, colon-separated — gtest's own list form.
        QStringList patterns;
        for (const QString& name : failedNames) {
            patterns << escapeGtestPattern(QString(name).replace(QLatin1Char('/'), QLatin1Char('.')));
        }
        return {QStringLiteral("--gtest_filter=%1").arg(patterns.join(QLatin1Char(':')))};
    }
    case TestFramework::Catch2: {
        QStringList arguments;
        for (const QString& name : failedNames) {
            arguments << name;
        }
        return arguments;
    }
    case TestFramework::BoostTest:
        return {QStringLiteral("--run_test=%1").arg(failedNames.join(QLatin1Char(',')))};
    case TestFramework::CTest: {
        QStringList patterns;
        for (const QString& name : failedNames) {
            TestCase temp;
            const qsizetype slash = name.indexOf(QLatin1Char('/'));
            if (slash > 0) {
                temp.suite = name.left(slash);
                temp.name = name.mid(slash + 1);
            } else {
                temp.name = name;
            }
            patterns << ctestNamePattern(temp);
        }
        return {QStringLiteral("-R"),
                patterns.join(QLatin1Char('|')),
                QStringLiteral("--output-on-failure")};
    }
    case TestFramework::Unknown:
        break;
    }
    return {};
}

TestRunPlan planRun(TestFramework framework, const QString& executable,
                    const QString& buildDirectory, const TestCase& test)
{
    TestRunPlan plan;
    plan.timeoutMs = defaultTimeoutMs(framework);
    if (runsThroughCtest(framework)) {
        plan.program = QStringLiteral("ctest");
        plan.workingDirectory = buildDirectory;
        plan.arguments << QStringLiteral("--test-dir") << buildDirectory;
        plan.arguments << filterArguments(framework, test);
        return plan;
    }
    plan.program = executable;
    plan.workingDirectory = QFileInfo(executable).absolutePath();
    plan.arguments << filterArguments(framework, test);
    return plan;
}

TestRunPlan planRunAll(TestFramework framework, const QString& executable,
                       const QString& buildDirectory)
{
    TestRunPlan plan;
    plan.timeoutMs = defaultTimeoutMs(framework);
    if (runsThroughCtest(framework)) {
        plan.program = QStringLiteral("ctest");
        plan.workingDirectory = buildDirectory;
        plan.arguments << QStringLiteral("--test-dir") << buildDirectory
                       << QStringLiteral("--output-on-failure");
        return plan;
    }
    plan.program = executable;
    plan.workingDirectory = QFileInfo(executable).absolutePath();
    return plan;
}

int defaultTimeoutMs(TestFramework framework)
{
    switch (framework) {
    case TestFramework::GoogleTest: return 5 * 60 * 1000;
    case TestFramework::Catch2:     return 5 * 60 * 1000;
    case TestFramework::BoostTest:  return 5 * 60 * 1000;
    case TestFramework::CTest:      return 20 * 60 * 1000;
    case TestFramework::Unknown:    break;
    }
    return 5 * 60 * 1000;
}

QStringList ctestReportArguments()
{
    return {QStringLiteral("--output-on-failure")};
}

bool runsThroughCtest(TestFramework framework)
{
    return framework == TestFramework::CTest;
}

QString ctestNamePattern(const TestCase& test)
{
    // ctest names a test "suite.case" only when add_test() was not given a
    // NAME; either way the full name as printed is what -R must match.
    const QString full = test.suite.isEmpty()
        ? test.name
        : test.suite + QLatin1Char('.') + test.name;
    QString escaped;
    escaped.reserve(full.size() + 8);
    for (const QChar ch : full) {
        if (QStringLiteral("\\^$.|?*+()[]{}").contains(ch)) {
            escaped.append(QLatin1Char('\\'));
        }
        escaped.append(ch);
    }
    return QStringLiteral("^%1$").arg(escaped);
}

QString formatCommand(const TestRunPlan& plan)
{
    return plan.commandLine();
}

}  // namespace LightCpp::Testing
