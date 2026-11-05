// TestDiscovery — which framework, and which tests exist. See TestModel.hpp.

#include "LightCpp/Testing/TestModel.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

namespace LightCpp::Testing {
namespace {

/// Strip a CMake comment, respecting quotes: `# comment` starts a comment,
/// `"a#b"` does not.
QString stripComment(const QString& line)
{
    bool inQuotes = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == QLatin1Char('"') && (i == 0 || line.at(i - 1) != QLatin1Char('\\'))) {
            inQuotes = !inQuotes;
        } else if (ch == QLatin1Char('#') && !inQuotes) {
            return line.left(i);
        }
    }
    return line;
}

QStringList splitArguments(const QString& inside)
{
    QStringList args;
    QString current;
    bool inQuotes = false;
    for (const QChar ch : inside) {
        if (ch == QLatin1Char('"')) {
            inQuotes = !inQuotes;
            continue;
        }
        if (ch.isSpace() && !inQuotes) {
            if (!current.isEmpty()) {
                args << current;
                current.clear();
            }
            continue;
        }
        current.append(ch);
    }
    if (!current.isEmpty()) {
        args << current;
    }
    return args;
}

}  // namespace

QString frameworkName(TestFramework framework)
{
    switch (framework) {
    case TestFramework::GoogleTest: return QStringLiteral("GoogleTest");
    case TestFramework::Catch2:     return QStringLiteral("Catch2");
    case TestFramework::BoostTest:  return QStringLiteral("Boost.Test");
    case TestFramework::CTest:      return QStringLiteral("CTest");
    case TestFramework::Unknown:    break;
    }
    return QStringLiteral("Unknown");
}

QString frameworkId(TestFramework framework)
{
    switch (framework) {
    case TestFramework::GoogleTest: return QStringLiteral("gtest");
    case TestFramework::Catch2:     return QStringLiteral("catch2");
    case TestFramework::BoostTest:  return QStringLiteral("boost-test");
    case TestFramework::CTest:      return QStringLiteral("ctest");
    case TestFramework::Unknown:    break;
    }
    return QStringLiteral("unknown");
}

TestFramework frameworkFromId(const QString& id)
{
    if (id == QLatin1String("gtest")) {
        return TestFramework::GoogleTest;
    }
    if (id == QLatin1String("catch2")) {
        return TestFramework::Catch2;
    }
    if (id == QLatin1String("boost-test") || id == QLatin1String("boost")) {
        return TestFramework::BoostTest;
    }
    if (id == QLatin1String("ctest")) {
        return TestFramework::CTest;
    }
    return TestFramework::Unknown;
}

QString outcomeName(TestOutcome outcome)
{
    switch (outcome) {
    case TestOutcome::Passed:   return QStringLiteral("passed");
    case TestOutcome::Failed:   return QStringLiteral("failed");
    case TestOutcome::Skipped:  return QStringLiteral("skipped");
    case TestOutcome::Disabled: return QStringLiteral("disabled");
    case TestOutcome::TimedOut: return QStringLiteral("timed out");
    case TestOutcome::NotRun:   break;
    }
    return QStringLiteral("not run");
}

QString TestCase::qualifiedName(TestFramework framework) const
{
    if (suite.isEmpty()) {
        return name;
    }
    // gtest uses `Suite.Case`; Catch2 and Boost use `Suite/Case` on the
    // command line (`--run_test=suite/case`), and ctest's names are already
    // whatever add_test() called them.
    const QChar separator = framework == TestFramework::GoogleTest
                                ? QLatin1Char('.')
                                : QLatin1Char('/');
    return suite + separator + name;
}

QString TestCase::displayName() const
{
    return suite.isEmpty() ? name : suite + QStringLiteral(".") + name;
}

const TestCaseResult* TestRun::firstFailure() const
{
    for (const TestCaseResult& result : cases) {
        if (result.outcome == TestOutcome::Failed
            || result.outcome == TestOutcome::TimedOut) {
            return &result;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Framework detection
// ---------------------------------------------------------------------------

TestProject detectFramework(const QString& cmakeListsPath)
{
    QFile file(cmakeListsPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QTextStream stream(&file);
    return detectFrameworkFromText(stream.readAll());
}

TestProject detectFrameworkFromText(const QString& cmakeText)
{
    TestProject project;
    // Count evidence rather than first-match: a project can link gtest and
    // also register ctest tests, and CTest wins because it is the thing that
    // can actually enumerate them without a built binary.
    int gtest = 0;
    int catch2 = 0;
    int boost = 0;
    int addTest = 0;
    bool gtestMain = false;

    const QStringList lines = cmakeText.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = stripComment(raw).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QString lower = line.toLower();
        if (lower.contains(QLatin1String("gtest_main"))) {
            gtestMain = true;
        }
        if (lower.contains(QLatin1String("gtest")) || lower.contains(QLatin1String("gmock"))
            || lower.contains(QLatin1String("googletest"))) {
            ++gtest;
        }
        if (lower.contains(QLatin1String("catch2")) || lower.contains(QLatin1String("catch_discover_tests"))) {
            ++catch2;
        }
        if (lower.contains(QLatin1String("boost_test"))
            || lower.contains(QLatin1String("boost/test"))
            || lower.contains(QLatin1String("unit_test_framework"))
            || lower.contains(QLatin1String("boost"))) {
            ++boost;
        }
        if (lower.startsWith(QLatin1String("add_test("))) {
            ++addTest;
            // add_test(NAME foo COMMAND bar ...) — the executable is the word
            // after COMMAND, up to the next argument keyword.
            const qsizetype open = line.indexOf(QLatin1Char('('));
            const qsizetype close = line.lastIndexOf(QLatin1Char(')'));
            if (open > 0 && close > open) {
                const QStringList args = splitArguments(line.mid(open + 1, close - open - 1));
                for (qsizetype i = 0; i + 1 < args.size(); ++i) {
                    if (args.at(i).compare(QLatin1String("COMMAND"), Qt::CaseInsensitive) == 0) {
                        const QString target = args.at(i + 1);
                        if (!target.startsWith(QLatin1String("$<"))
                            && !target.startsWith(QLatin1String("${"))) {
                            project.executables << target;
                        }
                        break;
                    }
                }
            }
        }
    }

    if (addTest > 0) {
        project.usesCtest = true;
        project.framework = TestFramework::CTest;
        return project;
    }
    if (catch2 >= gtest && catch2 >= boost && catch2 > 0) {
        project.framework = TestFramework::Catch2;
    } else if (gtest > 0 && gtest >= boost) {
        project.framework = TestFramework::GoogleTest;
        Q_UNUSED(gtestMain);
    } else if (boost > 0) {
        project.framework = TestFramework::BoostTest;
    }
    project.executables.removeDuplicates();
    return project;
}

QStringList discoveryArguments(TestFramework framework)
{
    switch (framework) {
    case TestFramework::GoogleTest:
        return {QStringLiteral("--gtest_list_tests")};
    case TestFramework::Catch2:
        return {QStringLiteral("--list-tests")};
    case TestFramework::BoostTest:
        return {QStringLiteral("--list_content")};
    case TestFramework::CTest:
        // `-N` is "show what would run" — no test is executed.
        return {QStringLiteral("-N")};
    case TestFramework::Unknown:
        break;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

QList<TestCase> parseGtestList(const QString& text)
{
    //   Math.
    //     AddsTwoNumbers
    //     DISABLED_Slow
    //   Other.
    //     Works
    QList<TestCase> cases;
    QString suite;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        if (raw.trimmed().isEmpty() || raw.startsWith(QLatin1String("Running main()"))) {
            continue;
        }
        const bool indented = raw.startsWith(QLatin1Char(' ')) || raw.startsWith(QLatin1Char('\t'));
        const QString line = raw.trimmed();
        if (!indented) {
            // A suite line ends with a dot; anything else is a note.
            if (line.endsWith(QLatin1Char('.'))) {
                suite = line.left(line.size() - 1);
            }
            continue;
        }
        TestCase test;
        test.suite = suite;
        // The trailing "  # GetParam() = 4" comment names the parameterised
        // instance; keep the name before it, which is what the runner takes.
        const qsizetype comment = line.indexOf(QLatin1String("  #"));
        test.name = comment > 0 ? line.left(comment).trimmed() : line;
        if (test.name.isEmpty()) {
            continue;
        }
        cases << test;
    }
    return cases;
}

QList<TestCase> parseCatch2List(const QString& text)
{
    // Catch2 v3:
    //   All available test cases:
    //     Adds two numbers
    //     [fast] Handles zero
    //   2 test cases
    QList<TestCase> cases;
    static const QRegularExpression tagPattern(QStringLiteral("^((?:\\[[^\\]]+\\]\\s*)+)(.*)$"));
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || raw.startsWith(QLatin1String("All available"))
            || line.endsWith(QLatin1String("test cases"))
            || line.startsWith(QLatin1String("Matching test cases"))) {
            continue;
        }
        // Only the indented block is the list; Catch2's header lines are not.
        if (!raw.startsWith(QLatin1Char(' ')) && !raw.startsWith(QLatin1Char('\t'))) {
            continue;
        }
        TestCase test;
        QString name = line;
        if (const auto tags = tagPattern.match(line); tags.hasMatch()) {
            for (const QString& tag : tags.captured(1).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                test.tags << tag;
            }
            name = tags.captured(2).trimmed();
        }
        if (name.isEmpty()) {
            continue;
        }
        test.name = name;
        // Catch2 has no suites, but a name with '/' is a nested section; the
        // runner takes it whole, so keep it whole.
        cases << test;
    }
    return cases;
}

QList<TestCase> parseBoostList(const QString& text)
{
    // Boost.Test --list_content:
    //   Math*
    //       AddsTwoNumbers*
    //   Other
    //       Works
    QList<TestCase> cases;
    QString suite;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        if (raw.trimmed().isEmpty()) {
            continue;
        }
        const int indent = static_cast<int>(raw.size() - raw.trimmed().size());
        QString name = raw.trimmed();
        // The trailing '*' marks a test unit ("enabled"), not part of the name.
        if (name.endsWith(QLatin1Char('*'))) {
            name.chop(1);
        }
        if (name.isEmpty()) {
            continue;
        }
        if (indent == 0) {
            suite = name;
            continue;
        }
        TestCase test;
        test.suite = suite;
        test.name = name;
        cases << test;
    }
    return cases;
}

QList<TestCase> parseCtestList(const QString& text)
{
    //   Test project /build
    //     Test  #1: Math.AddsTwoNumbers
    //     Test  #2: Other.Works
    //   Total Tests: 2
    QList<TestCase> cases;
    static const QRegularExpression pattern(
        QStringLiteral("^\\s*Test\\s+#(\\d+):\\s*(.+?)\\s*$"));
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const auto match = pattern.match(raw);
        if (!match.hasMatch()) {
            continue;
        }
        TestCase test;
        test.name = match.captured(2);
        // ctest names default to "binary.test" when add_test() was not given
        // a NAME: show the part after the dot as the case, keep the rest.
        const qsizetype dot = test.name.indexOf(QLatin1Char('.'));
        if (dot > 0) {
            test.suite = test.name.left(dot);
            test.name = test.name.mid(dot + 1);
        }
        cases << test;
    }
    return cases;
}

QList<TestCase> parseList(TestFramework framework, const QString& text)
{
    switch (framework) {
    case TestFramework::GoogleTest: return parseGtestList(text);
    case TestFramework::Catch2:     return parseCatch2List(text);
    case TestFramework::BoostTest:  return parseBoostList(text);
    case TestFramework::CTest:      return parseCtestList(text);
    case TestFramework::Unknown:    break;
    }
    return {};
}

}  // namespace LightCpp::Testing
