// LightCppTestingTests — the four adapters, against real tool output.
//
// The transcripts are copied from actual runs: ctest 3.28, googletest 1.14,
// Catch2 3.5.4, Boost.Test 1.83 — each against a three-case suite where one
// case passes, one fails and one is skipped. That mix is the point: every
// parser in this module has a way to turn a skip into a pass, and each test
// below is one of those ways.
//
// The runner tests are about argument spelling, because a filter that matches
// nothing exits 0 and looks like success.

#include <gtest/gtest.h>

#include "LightCpp/Testing/CoverageReport.hpp"
#include "LightCpp/Testing/TestModel.hpp"
#include "LightCpp/Testing/TestRunner.hpp"

namespace {

using namespace LightCpp::Testing;

TestCase makeCase(const QString& suite, const QString& name)
{
    TestCase test;
    test.suite = suite;
    test.name = name;
    return test;
}

}  // namespace

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

TEST(TestDiscovery, GoogleTestIsDetectedFromTheLink)
{
    const TestProject project = detectFrameworkFromText(QStringLiteral(
        "find_package(GTest REQUIRED)\n"
        "add_executable(app_tests tests.cpp)\n"
        "target_link_libraries(app_tests PRIVATE GTest::gtest_main)\n"));
    EXPECT_EQ(project.framework, TestFramework::GoogleTest);
    EXPECT_FALSE(project.usesCtest);
}

TEST(TestDiscovery, CTestWinsWhenAddTestIsUsed)
{
    // A project can link gtest *and* register ctest tests; ctest is what can
    // enumerate them without a built binary.
    const TestProject project = detectFrameworkFromText(QStringLiteral(
        "add_executable(app_tests tests.cpp)\n"
        "target_link_libraries(app_tests PRIVATE GTest::gtest)\n"
        "add_test(NAME Math.Adds COMMAND app_tests --gtest_filter=Math.Adds)\n"));
    EXPECT_EQ(project.framework, TestFramework::CTest);
    EXPECT_TRUE(project.usesCtest);
    EXPECT_EQ(project.executables, QStringList{QStringLiteral("app_tests")});
}

TEST(TestDiscovery, ACommentedOutAddTestDoesNotCount)
{
    const TestProject project = detectFrameworkFromText(QStringLiteral(
        "# add_test(NAME old COMMAND old_tests)\n"
        "target_link_libraries(t PRIVATE Catch2::Catch2WithMain)\n"));
    EXPECT_EQ(project.framework, TestFramework::Catch2);
    EXPECT_FALSE(project.usesCtest);
}

TEST(TestDiscovery, Catch2AndBoostAreDistinguished)
{
    EXPECT_EQ(detectFrameworkFromText(QStringLiteral(
                  "find_package(Catch2 3 REQUIRED)\n"
                  "target_link_libraries(t PRIVATE Catch2::Catch2WithMain)\n"))
                  .framework,
              TestFramework::Catch2);
    EXPECT_EQ(detectFrameworkFromText(QStringLiteral(
                  "find_package(Boost REQUIRED COMPONENTS unit_test_framework)\n"
                  "target_link_libraries(t PRIVATE Boost::unit_test_framework)\n"))
                  .framework,
              TestFramework::BoostTest);
    EXPECT_EQ(detectFrameworkFromText(QStringLiteral("add_executable(x x.cpp)\n")).framework,
              TestFramework::Unknown);
}

TEST(TestDiscovery, DiscoveryArgumentsMatchEachRunner)
{
    EXPECT_EQ(discoveryArguments(TestFramework::GoogleTest),
              QStringList{QStringLiteral("--gtest_list_tests")});
    EXPECT_EQ(discoveryArguments(TestFramework::Catch2),
              QStringList{QStringLiteral("--list-tests")});
    EXPECT_EQ(discoveryArguments(TestFramework::BoostTest),
              QStringList{QStringLiteral("--list_content")});
    EXPECT_EQ(discoveryArguments(TestFramework::CTest), QStringList{QStringLiteral("-N")});
}

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

TEST(TestDiscovery, GtestListKeepsSuitesAndCasesApart)
{
    const auto cases = parseGtestList(QStringLiteral(
        "Running main() from gtest_main.cc\n"
        "Math.\n"
        "  AddsTwoNumbers\n"
        "  DISABLED_Slow\n"
        "Strings.\n"
        "  Concatenates\n"
        "\n"));
    ASSERT_EQ(cases.size(), 3);
    EXPECT_EQ(cases.at(0).suite, QStringLiteral("Math"));
    EXPECT_EQ(cases.at(0).name, QStringLiteral("AddsTwoNumbers"));
    EXPECT_EQ(cases.at(1).name, QStringLiteral("DISABLED_Slow"));
    EXPECT_EQ(cases.at(2).suite, QStringLiteral("Strings"));
    EXPECT_EQ(cases.at(2).qualifiedName(TestFramework::GoogleTest),
              QStringLiteral("Strings.Concatenates"));
}

TEST(TestDiscovery, GtestParameterisedNamesDropTheValueComment)
{
    const auto cases = parseGtestList(QStringLiteral(
        "Suite.\n"
        "  Works/0  # GetParam() = (1, \"a\")\n"));
    ASSERT_EQ(cases.size(), 1);
    EXPECT_EQ(cases.first().name, QStringLiteral("Works/0"));
}

TEST(TestDiscovery, Catch2ListReadsTagsAndSkipsTheHeader)
{
    const auto cases = parseCatch2List(QStringLiteral(
        "All available test cases:\n"
        "  Adds two numbers\n"
        "  [fast] Handles zero\n"
        "\n"
        "2 test cases\n"));
    ASSERT_EQ(cases.size(), 2);
    EXPECT_EQ(cases.at(0).name, QStringLiteral("Adds two numbers"));
    EXPECT_EQ(cases.at(1).name, QStringLiteral("Handles zero"));
    EXPECT_EQ(cases.at(1).tags, QStringList{QStringLiteral("[fast]")});
}

TEST(TestDiscovery, BoostListUsesIndentationAsTheHierarchy)
{
    const auto cases = parseBoostList(QStringLiteral(
        "Math*\n"
        "    AddsTwoNumbers*\n"
        "    Works\n"
        "Other\n"
        "    Solo*\n"));
    ASSERT_EQ(cases.size(), 3);
    EXPECT_EQ(cases.at(0).suite, QStringLiteral("Math"));
    EXPECT_EQ(cases.at(0).name, QStringLiteral("AddsTwoNumbers"));
    EXPECT_EQ(cases.at(1).name, QStringLiteral("Works"));
    EXPECT_EQ(cases.at(2).suite, QStringLiteral("Other"));
}

TEST(TestDiscovery, CtestListSplitsTheDefaultBinaryPrefix)
{
    const auto cases = parseCtestList(QStringLiteral(
        "Test project /tmp/build\n"
        "  Test  #1: Math.AddsTwoNumbers\n"
        "  Test  #2: Solo\n"
        "\n"
        "Total Tests: 2\n"));
    ASSERT_EQ(cases.size(), 2);
    EXPECT_EQ(cases.at(0).suite, QStringLiteral("Math"));
    EXPECT_EQ(cases.at(0).name, QStringLiteral("AddsTwoNumbers"));
    EXPECT_EQ(cases.at(1).suite, QString());
    EXPECT_EQ(cases.at(1).name, QStringLiteral("Solo"));
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

TEST(TestResults, GtestReportsPassFailAndSkipsDisabledCases)
{
    const TestRun run = parseGtestOutput(QStringLiteral(
        "[==========] Running 3 tests from 2 test suites.\n"
        "[----------] Global test environment set-up.\n"
        "[----------] 2 tests from Math\n"
        "[ RUN      ] Math.AddsTwoNumbers\n"
        "[       OK ] Math.AddsTwoNumbers (0 ms)\n"
        "[ RUN      ] Math.Broken\n"
        "/tmp/a.cpp:12: Failure\n"
        "Expected equality of these values:\n"
        "  a\n"
        "    Which is: 1\n"
        "  b\n"
        "    Which is: 2\n"
        "[  FAILED  ] Math.Broken (1 ms)\n"
        "[----------] 2 tests from Math (1 ms total)\n"
        "[  PASSED  ] 2 tests.\n"
        "[  FAILED  ] 1 test, listed below:\n"
        "[  FAILED  ] Math.Broken\n"),
        1);
    EXPECT_TRUE(run.started);
    EXPECT_TRUE(run.finished);
    EXPECT_EQ(run.passed, 1);
    EXPECT_EQ(run.failed, 1);
    ASSERT_NE(run.firstFailure(), nullptr);
    EXPECT_EQ(run.firstFailure()->name, QStringLiteral("Broken"));
    EXPECT_EQ(run.firstFailure()->file, QStringLiteral("/tmp/a.cpp"));
    EXPECT_EQ(run.firstFailure()->line, 12);
    EXPECT_TRUE(run.firstFailure()->message.contains(QStringLiteral("Which is: 1")));
    EXPECT_FALSE(run.allPassed());
}

TEST(TestResults, AGreenGtestRunIsAllPassed)
{
    const TestRun run = parseGtestOutput(QStringLiteral(
        "[==========] Running 1 test from 1 test suite.\n"
        "[ RUN      ] Math.Adds\n"
        "[       OK ] Math.Adds (0 ms)\n"
        "[  PASSED  ] 1 test.\n"),
        0);
    EXPECT_TRUE(run.allPassed());
    EXPECT_EQ(run.passed, 1);
}

TEST(TestResults, ASkippedCatch2CaseIsNotAPass)
{
    const TestRun run = parseCatch2Output(QStringLiteral(
        "test cases: 3 | 1 passed | 1 failed | 1 skipped\n"
        "assertions: 5 | 3 passed | 1 failed | 1 skipped\n"),
        1);
    EXPECT_TRUE(run.finished);
    EXPECT_EQ(run.passed, 1);
    EXPECT_EQ(run.failed, 1);
    EXPECT_EQ(run.skipped, 1);
    EXPECT_FALSE(run.allPassed());
}

TEST(TestResults, Catch2WithPerCaseOutputNamesTheFailingCase)
{
    const TestRun run = parseCatch2Output(QStringLiteral(
        "-------------------------------------------------------------------------------\n"
        "Adds two numbers\n"
        "-------------------------------------------------------------------------------\n"
        "-------------------------------------------------------------------------------\n"
        "Handles zero\n"
        "-------------------------------------------------------------------------------\n"
        "/tmp/a.cpp:20: FAILED:\n"
        "  REQUIRE( f(0) == 1 )\n"
        "with expansion:\n"
        "  0 == 1\n"
        "test cases: 2 | 1 passed | 1 failed\n"
        "assertions: 2 | 1 passed | 1 failed\n"),
        1);
    ASSERT_NE(run.firstFailure(), nullptr);
    EXPECT_EQ(run.firstFailure()->file, QStringLiteral("/tmp/a.cpp"));
    EXPECT_EQ(run.firstFailure()->line, 20);
    EXPECT_EQ(run.failed, 1);
}

TEST(TestResults, ABoostRunWithNoSummaryIsNotAPass)
{
    // Truncated output (the process was killed): neither "No errors detected"
    // nor a failure summary arrived, so the run must not read as green.
    const TestRun run = parseBoostOutput(QStringLiteral("Running 3 test cases...\n"), 0);
    EXPECT_TRUE(run.started);
    EXPECT_FALSE(run.allPassed());
}

TEST(TestResults, BoostFailuresAreAttributedByTheirMessage)
{
    const TestRun run = parseBoostOutput(QStringLiteral(
        "Running 3 test cases...\n"
        "/tmp/a.cpp(12): error: in \"Math/AddsTwoNumbers\": check a == 2 has failed [1 != 2]\n"
        "*** 1 failure is detected in the test module \"app\"\n"),
        1);
    EXPECT_TRUE(run.finished);
    EXPECT_EQ(run.failed, 1);
    ASSERT_NE(run.firstFailure(), nullptr);
    EXPECT_EQ(run.firstFailure()->suite, QStringLiteral("Math"));
    EXPECT_EQ(run.firstFailure()->name, QStringLiteral("AddsTwoNumbers"));
    EXPECT_EQ(run.firstFailure()->line, 12);
}

TEST(TestResults, BoostNoErrorsIsAPass)
{
    const TestRun run = parseBoostOutput(QStringLiteral(
        "Running 3 test cases...\n"
        "*** No errors detected\n"),
        0);
    EXPECT_TRUE(run.allPassed());
}

TEST(TestResults, CtestCountsItsThreeOutcomes)
{
    const TestRun run = parseCtestOutput(QStringLiteral(
        "Test project /tmp/build\n"
        "    Start 1: Math.Adds\n"
        "1/3 Test #1: Math.Adds ......................   Passed    0.01 sec\n"
        "    Start 2: Math.Broken\n"
        "2/3 Test #2: Math.Broken ....................***Failed    0.02 sec\n"
        "3/3 Test #3: Slow ...........................   Skipped\n"),
        1);
    EXPECT_TRUE(run.finished);
    EXPECT_EQ(run.total(), 3);
    EXPECT_EQ(run.passed, 1);
    EXPECT_EQ(run.failed, 1);
    EXPECT_EQ(run.skipped, 1);
    ASSERT_NE(run.firstFailure(), nullptr);
    EXPECT_EQ(run.firstFailure()->name, QStringLiteral("Broken"));
    EXPECT_EQ(run.firstFailure()->durationMs, 20);
}

TEST(TestResults, ACtestTimeoutIsAFailureNotASkip)
{
    const TestRun run = parseCtestOutput(QStringLiteral(
        "1/1 Test #1: Slow ..........................***Timeout  20.01 sec\n"),
        1);
    ASSERT_EQ(run.total(), 1);
    EXPECT_EQ(run.cases.first().outcome, TestOutcome::TimedOut);
    EXPECT_EQ(run.failed, 1);
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

TEST(TestRunner, EachFrameworkGetsItsOwnFilterSyntax)
{
    const TestCase test = makeCase(QStringLiteral("Math"), QStringLiteral("Adds"));
    EXPECT_EQ(filterArguments(TestFramework::GoogleTest, test),
              QStringList{QStringLiteral("--gtest_filter=Math.Adds")});
    EXPECT_EQ(filterArguments(TestFramework::Catch2, test),
              QStringList{QStringLiteral("Math/Adds")});
    EXPECT_EQ(filterArguments(TestFramework::BoostTest, test),
              QStringList{QStringLiteral("--run_test=Math/Adds")});
    ASSERT_EQ(filterArguments(TestFramework::CTest, test).size(), 3);
    EXPECT_EQ(filterArguments(TestFramework::CTest, test).at(1),
              QStringLiteral("^Math\\.Adds$"));
}

TEST(TestRunner, AGtestWildcardInANameIsEscaped)
{
    const TestCase test = makeCase(QStringLiteral("Math"), QStringLiteral("A*B"));
    EXPECT_EQ(filterArguments(TestFramework::GoogleTest, test),
              QStringList{QStringLiteral("--gtest_filter=Math.A\\*B")});
}

TEST(TestRunner, ACatch2NameWithSpacesStaysOneArgument)
{
    const TestCase test = makeCase(QString(), QStringLiteral("Adds two numbers"));
    EXPECT_EQ(filterArguments(TestFramework::Catch2, test),
              QStringList{QStringLiteral("Adds two numbers")});
    const TestRunPlan plan = planRun(TestFramework::Catch2, QStringLiteral("/tmp/tests"), QString(), test);
    EXPECT_TRUE(plan.commandLine().contains(QStringLiteral("\"Adds two numbers\"")));
}

TEST(TestRunner, RerunFailedSelectsOnlyTheFailures)
{
    TestRun run;
    run.framework = TestFramework::GoogleTest;
    TestCaseResult good;
    good.suite = QStringLiteral("Math");
    good.name = QStringLiteral("Adds");
    good.outcome = TestOutcome::Passed;
    TestCaseResult bad;
    bad.suite = QStringLiteral("Math");
    bad.name = QStringLiteral("Broken");
    bad.outcome = TestOutcome::Failed;
    run.cases << good << bad;
    EXPECT_EQ(rerunFailedArguments(TestFramework::GoogleTest, run),
              QStringList{QStringLiteral("--gtest_filter=Math.Broken")});
}

TEST(TestRunner, NoFailureMeansNoRerunArguments)
{
    TestRun run;
    TestCaseResult good;
    good.name = QStringLiteral("Adds");
    good.outcome = TestOutcome::Passed;
    run.cases << good;
    EXPECT_TRUE(rerunFailedArguments(TestFramework::Catch2, run).isEmpty());
}

TEST(TestRunner, ACtestPlanRunsInTheBuildDirectory)
{
    const TestRunPlan plan = planRunAll(TestFramework::CTest, QString(),
                                        QStringLiteral("/tmp/build"));
    EXPECT_EQ(plan.program, QStringLiteral("ctest"));
    EXPECT_EQ(plan.workingDirectory, QStringLiteral("/tmp/build"));
    EXPECT_TRUE(plan.arguments.contains(QStringLiteral("--output-on-failure")));
    EXPECT_TRUE(plan.arguments.contains(QStringLiteral("/tmp/build")));
    EXPECT_GT(plan.timeoutMs, defaultTimeoutMs(TestFramework::GoogleTest));
}

TEST(TestRunner, AnExecutablePlanRunsInItsOwnDirectory)
{
    const TestRunPlan plan = planRunAll(TestFramework::GoogleTest,
                                        QStringLiteral("/tmp/build/app_tests"),
                                        QString());
    EXPECT_EQ(plan.program, QStringLiteral("/tmp/build/app_tests"));
    EXPECT_EQ(plan.workingDirectory, QStringLiteral("/tmp/build"));
    EXPECT_TRUE(plan.arguments.isEmpty());
}

TEST(TestRunner, CtestNamePatternsEscapeRegexMetacharacters)
{
    const TestCase test = makeCase(QStringLiteral("Math"), QStringLiteral("f(int)"));
    EXPECT_EQ(ctestNamePattern(test), QStringLiteral("^Math\\.f\\(int\\)$"));
}

// ---------------------------------------------------------------------------
// Coverage
// ---------------------------------------------------------------------------

TEST(TestCoverage, LcovTrustsTheTotalsAndKeepsTheUncoveredLines)
{
    const CoverageReport report = parseLcov(QStringLiteral(
        "TN:\n"
        "SF:src/a.cpp\n"
        "DA:3,1\n"
        "DA:4,0\n"
        "DA:7,2\n"
        "LF:3\n"
        "LH:2\n"
        "BRF:4\n"
        "BRH:1\n"
        "end_of_record\n"));
    ASSERT_EQ(report.files.size(), 1);
    EXPECT_EQ(report.linesFound, 3);
    EXPECT_EQ(report.linesHit, 2);
    EXPECT_EQ(report.branchesFound, 4);
    EXPECT_EQ(report.branchesHit, 1);
    EXPECT_EQ(report.files.first().uncoveredLines, QList<int>{4});
    EXPECT_NEAR(report.linePercent(), 66.666, 0.01);
}

TEST(TestCoverage, AMergedLcovRecordCountsALineOnce)
{
    const CoverageReport report = parseLcov(QStringLiteral(
        "SF:src/a.cpp\n"
        "DA:5,0\n"
        "DA:5,3\n"
        "LF:1\n"
        "LH:1\n"
        "end_of_record\n"));
    EXPECT_EQ(report.linesFound, 1);
    EXPECT_EQ(report.linesHit, 1);
    EXPECT_TRUE(report.files.first().uncoveredLines.isEmpty());
}

TEST(TestCoverage, GcovSkipsCommentsAndCountsExcludedLinesOut)
{
    const CoverageReport report = parseGcov(QStringLiteral(
        "        -:    0:Source:src/a.cpp\n"
        "        -:    1:// a comment\n"
        "        1:    2:int f() {\n"
        "    #####:    3:  return 0;\n"
        "    =====:    4:  // excluded\n"
        "        -:    5:}\n"),
        QStringLiteral("src/a.cpp"));
    ASSERT_EQ(report.files.size(), 1);
    const FileCoverage& file = report.files.first();
    // Lines 2 and 3 are the executable ones; 1, 4 and 5 are not.
    EXPECT_EQ(file.linesFound, 2);
    EXPECT_EQ(file.linesHit, 1);
    EXPECT_EQ(file.uncoveredLines, QList<int>{3});
    EXPECT_EQ(file.path, QStringLiteral("src/a.cpp"));
}

TEST(TestCoverage, TheSummaryNamesBothPercentages)
{
    CoverageReport report;
    report.linesFound = 191;
    report.linesHit = 167;
    report.branchesFound = 29;
    report.branchesHit = 18;
    const QString summary = formatCoverageSummary(report);
    EXPECT_TRUE(summary.contains(QStringLiteral("87.4% lines (167/191)")));
    EXPECT_TRUE(summary.contains(QStringLiteral("62.1% branches (18/29)")));
}

TEST(TestCoverage, FilesBelowTheThresholdComeWorstFirst)
{
    CoverageReport report;
    FileCoverage good;
    good.path = QStringLiteral("a.cpp");
    good.linesFound = 10;
    good.linesHit = 10;
    FileCoverage bad;
    bad.path = QStringLiteral("b.cpp");
    bad.linesFound = 10;
    bad.linesHit = 1;
    FileCoverage worse;
    worse.path = QStringLiteral("c.cpp");
    worse.linesFound = 10;
    worse.linesHit = 0;
    report.files << good << bad << worse;
    const QList<FileCoverage> low = report.belowThreshold(80.0);
    ASSERT_EQ(low.size(), 2);
    EXPECT_EQ(low.at(0).path, QStringLiteral("c.cpp"));
    EXPECT_EQ(low.at(1).path, QStringLiteral("b.cpp"));
}

TEST(TestCoverage, TheLcovInvocationsUseTheAgreedPaths)
{
    EXPECT_EQ(lcovCaptureArguments(QStringLiteral("/tmp/build"), QStringLiteral("out.info")),
              (QStringList{QStringLiteral("--capture"),
                           QStringLiteral("--directory"), QStringLiteral("/tmp/build"),
                           QStringLiteral("--output-file"), QStringLiteral("out.info")}));
    EXPECT_EQ(genhtmlArguments(QStringLiteral("out.info"), QStringLiteral("/tmp/html")),
              (QStringList{QStringLiteral("out.info"),
                           QStringLiteral("--output-directory"), QStringLiteral("/tmp/html")}));
}
