// GTest coverage for the compiler-output parser feeding the
// "Problems (Compiler)" list: GNU/Clang and MSVC diagnostic lines, plus the
// driver-level errors that have no source location.

#include "LightCpp/Project/CompilerDiagnostics.hpp"

#include <gtest/gtest.h>

namespace {

using LightCpp::Project::parseCompilerDiagnostic;

TEST(CompilerDiagnostics, GnuErrorWithColumn)
{
    const auto d = parseCompilerDiagnostic(
        "E:/msys64/tmp/lightcpp-compile/Untitled-1.cpp:12:34: error: "
        "'x' was not declared in this scope");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->file, "E:/msys64/tmp/lightcpp-compile/Untitled-1.cpp");
    EXPECT_EQ(d->line, 11);      // stored 0-based
    EXPECT_EQ(d->column, 33);
    EXPECT_EQ(d->severity, 1);
    EXPECT_EQ(d->message, "'x' was not declared in this scope");
}

TEST(CompilerDiagnostics, GnuErrorWithoutColumn)
{
    const auto d = parseCompilerDiagnostic(
        "main.cpp:3: error: expected ';' before 'return'");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->file, "main.cpp");
    EXPECT_EQ(d->line, 2);
    EXPECT_EQ(d->column, 0);
    EXPECT_EQ(d->severity, 1);
    EXPECT_EQ(d->message, "expected ';' before 'return'");
}

TEST(CompilerDiagnostics, GnuFatalError)
{
    const auto d = parseCompilerDiagnostic(
        "foo.h:1:10: fatal error: no_such.h: No such file or directory");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->file, "foo.h");
    EXPECT_EQ(d->line, 0);
    EXPECT_EQ(d->column, 9);
    EXPECT_EQ(d->severity, 1);
}

TEST(CompilerDiagnostics, GnuWarning)
{
    const auto d = parseCompilerDiagnostic(
        "E:/src/a.cpp:8:5: warning: unused variable 'y' [-Wunused-variable]");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->file, "E:/src/a.cpp");
    EXPECT_EQ(d->line, 7);
    EXPECT_EQ(d->column, 4);
    EXPECT_EQ(d->severity, 2);
    EXPECT_EQ(d->message, "unused variable 'y' [-Wunused-variable]");
}

TEST(CompilerDiagnostics, GnuRelativePathWithDirectories)
{
    const auto d = parseCompilerDiagnostic(
        "src/sub/dir/file.cpp:100:1: error: something broke");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->file, "src/sub/dir/file.cpp");
    EXPECT_EQ(d->line, 99);
    EXPECT_EQ(d->column, 0);
}

TEST(CompilerDiagnostics, MsvcErrorWithCode)
{
    const auto d = parseCompilerDiagnostic(
        "E:/proj/main.cpp(5,3): error C2065: 'x': undeclared identifier");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->file, "E:/proj/main.cpp");
    EXPECT_EQ(d->line, 4);
    EXPECT_EQ(d->column, 2);
    EXPECT_EQ(d->severity, 1);
    EXPECT_EQ(d->message, "'x': undeclared identifier");
}

TEST(CompilerDiagnostics, MsvcErrorWithoutColumnAndCode)
{
    const auto d = parseCompilerDiagnostic(
        "main.cpp(12): error: syntax error");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->file, "main.cpp");
    EXPECT_EQ(d->line, 11);
    EXPECT_EQ(d->column, 0);
    EXPECT_EQ(d->severity, 1);
}

TEST(CompilerDiagnostics, MsvcWarningWithCode)
{
    const auto d = parseCompilerDiagnostic(
        "E:/proj/main.cpp(7): warning C4101: 'y': unreferenced local variable");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->file, "E:/proj/main.cpp");
    EXPECT_EQ(d->line, 6);
    EXPECT_EQ(d->severity, 2);
}

TEST(CompilerDiagnostics, MsvcFatalErrorInclude)
{
    const auto d = parseCompilerDiagnostic(
        "main.cpp(3): fatal error C1083: Cannot open include file: "
        "'nope.h': No such file or directory");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->severity, 1);
    EXPECT_EQ(d->line, 2);
}

TEST(CompilerDiagnostics, DriverErrorClang)
{
    // The bug that motivated this module: clang rejects -fexec-charset=GBK.
    const auto d = parseCompilerDiagnostic(
        "clang++: error: invalid value 'GBK' in '-fexec-charset=GBK'");
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(d->file.empty());
    EXPECT_EQ(d->line, 0);
    EXPECT_EQ(d->column, 0);
    EXPECT_EQ(d->severity, 1);
    EXPECT_EQ(d->message, "invalid value 'GBK' in '-fexec-charset=GBK'");
}

TEST(CompilerDiagnostics, DriverErrorCollect2)
{
    const auto d = parseCompilerDiagnostic(
        "collect2.exe: error: ld returned 1 exit status");
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(d->file.empty());
    EXPECT_EQ(d->severity, 1);
    EXPECT_EQ(d->message, "ld returned 1 exit status");
}

TEST(CompilerDiagnostics, IgnoresNonDiagnosticLines)
{
    EXPECT_FALSE(parseCompilerDiagnostic("").has_value());
    EXPECT_FALSE(parseCompilerDiagnostic(
        "--- Compiling: clang++.exe -Wall -O0 main.cpp ---").has_value());
    EXPECT_FALSE(parseCompilerDiagnostic(
        "In file included from main.cpp:2:").has_value());
    EXPECT_FALSE(parseCompilerDiagnostic(
        "main.cpp:3:5: note: declared here").has_value());
    EXPECT_FALSE(parseCompilerDiagnostic(
        "Scanning dependencies of target app").has_value());
    EXPECT_FALSE(parseCompilerDiagnostic(
        "[ 50%] Building CXX object CMakeFiles/app.dir/main.cpp.obj"
        ).has_value());
    EXPECT_FALSE(parseCompilerDiagnostic(
        "Build finished, 1 error").has_value());
}

TEST(CompilerDiagnostics, GnuLinesThatAreNotReports)
{
    // "file:line:" without a severity word must not be misread.
    EXPECT_FALSE(parseCompilerDiagnostic(
        "main.cpp:3:5: something else entirely").has_value());
    // A drive letter alone is not a path anchor.
    EXPECT_FALSE(parseCompilerDiagnostic(
        "E:/some/directory exists").has_value());
}

}  // namespace

// --- localized MSVC output ----------------------------------------------

TEST(CompilerDiagnostics, ChineseMsvcErrorParses)
{
    // A localized Visual Studio prints 错误/警告 instead of error/warning;
    // the bytes arrive as UTF-8 (the console decoder already normalized).
    // "错误" = E9 94 99 E8 AF AF, "警告" = E8 AD A6 E5 91 8A.
    const std::string line =
        "E:/proj/main.cpp(12,5): \xE9\x94\x99\xE8\xAF\xAF C2065: "
        "\xE6\x9C\xAA\xE5\xA3\xB0\xE6\x98\x8E";   // 未声明
    const auto d = parseCompilerDiagnostic(line);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->file, "E:/proj/main.cpp");
    EXPECT_EQ(d->line, 11);
    EXPECT_EQ(d->column, 4);
    EXPECT_EQ(d->severity, 1);
}

TEST(CompilerDiagnostics, ChineseMsvcWarningParses)
{
    const std::string line =
        "main.cpp(7): \xE8\xAD\xA6\xE5\x91\x8A C4101: unused";
    const auto d = parseCompilerDiagnostic(line);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->severity, 2);
    EXPECT_EQ(d->line, 6);
}

TEST(CompilerDiagnostics, ChineseMsvcFatalErrorParses)
{
    // 致命错误 = fatal error
    const std::string line =
        "main.cpp(3): \xE8\x87\xB4\xE5\x91\xBD\xE9\x94\x99\xE8\xAF\xAF C1083: nope";
    const auto d = parseCompilerDiagnostic(line);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->severity, 1);
}

TEST(CompilerDiagnostics, MsvcLineWithParenthesisInTheMessage)
{
    // The LAST '(' in the line is inside the message; the location
    // parenthesis is the first one whose contents parse as numbers.
    const auto d = parseCompilerDiagnostic(
        "main.cpp(4): error C2059: syntax error: unmatched '('");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->file, "main.cpp");
    EXPECT_EQ(d->line, 3);
    EXPECT_EQ(d->severity, 1);
}

TEST(CompilerDiagnostics, MsvcLineWithParenthesisInPath)
{
    // Parenthesised directories ("Program Files (x86)") stay intact.
    const auto d = parseCompilerDiagnostic(
        "E:/Tools (x86)/proj/main.cpp(9,2): error C2143: missing ';'");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->file, "E:/Tools (x86)/proj/main.cpp");
    EXPECT_EQ(d->line, 8);
    EXPECT_EQ(d->column, 1);
}

TEST(CompilerDiagnostics, StripsMsbuildLinePrefix)
{
    // msbuild's console logger prefixes every line with "<n>>". Left in the
    // file part, the Problems entry reads "1>main.cpp" and the double-click
    // cannot open it.
    const auto msvc = parseCompilerDiagnostic(
        "1>main.cpp(10): error C2065: 'x': undeclared identifier");
    ASSERT_TRUE(msvc.has_value());
    EXPECT_EQ(msvc->file, "main.cpp");
    EXPECT_EQ(msvc->line, 9);
    EXPECT_EQ(msvc->severity, 1);

    const auto gnu = parseCompilerDiagnostic(
        "2>src/util.cpp:12:7: warning: unused variable 'y'");
    ASSERT_TRUE(gnu.has_value());
    EXPECT_EQ(gnu->file, "src/util.cpp");
    EXPECT_EQ(gnu->line, 11);
    EXPECT_EQ(gnu->column, 6);
    EXPECT_EQ(gnu->severity, 2);
}

TEST(CompilerDiagnostics, LeavesDigitColonLinesAlone)
{
    // A leading digit is only a prefix when a '>' follows; "3:x.cpp..." is a
    // line number and must not be trimmed.
    EXPECT_FALSE(parseCompilerDiagnostic(
        "3:x.cpp: not a report").has_value());
}

TEST(CompilerDiagnostics, DriverLineWithSpaceBeforeColon)
{
    // GNU linker output: "main.obj : error LNK2019: ...". The space before the
    // colon used to make the program name fail looksLikeDriverName, dropping
    // the line — a link failure then produced no Problems entry at all.
    const auto d = parseCompilerDiagnostic(
        "main.obj : error LNK2019: unresolved external symbol _main");
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(d->file.empty());
    EXPECT_EQ(d->line, 0);
    EXPECT_EQ(d->severity, 1);
    EXPECT_EQ(d->message,
              "unresolved external symbol _main");
}

TEST(CompilerDiagnostics, MakeSummaryLineIsAnError)
{
    // "make: *** [...] Error 1" carries no "error:" word — the word is
    // capitalised and behind "***" — so nothing matched it and a build that
    // failed at the make level reached Problems as zero errors.
    const std::string line = "make: *** [Makefile.win:12: all] Error 1";
    const auto d = parseCompilerDiagnostic(line);
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(d->file.empty());
    EXPECT_EQ(d->line, 0);
    EXPECT_EQ(d->column, 0);
    EXPECT_EQ(d->severity, 1);
    EXPECT_EQ(d->message, line);  // the tool's name is the subject here
}

TEST(CompilerDiagnostics, MakeSummaryUnderItsFullPathIsRecognised)
{
    // make prints itself by whatever name invoked it, full path included.
    const auto d = parseCompilerDiagnostic(
        "E:/msys64/ucrt64/bin/mingw32-make.exe: *** [Makefile.win:5: all] Error 2");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->severity, 1);
    EXPECT_TRUE(d->file.empty());
}

TEST(CompilerDiagnostics, MakeNoRuleSummaryIsAnError)
{
    const auto d = parseCompilerDiagnostic(
        "mingw32-make: *** No rule to make target 'main.o', needed by 'hello.exe'.  Stop.");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->severity, 1);
    EXPECT_TRUE(d->file.empty());
}

TEST(CompilerDiagnostics, NinjaStoppedIsAnError)
{
    const auto d = parseCompilerDiagnostic("ninja: build stopped: subcommand failed.");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->severity, 1);
    EXPECT_TRUE(d->file.empty());
}

TEST(CompilerDiagnostics, OtherProgramsBehindStarsAreNotDiagnostics)
{
    // The allowlist is the point: "***" alone is not a diagnostic, and a
    // wrong match here would put a bogus entry in Problems.
    EXPECT_FALSE(parseCompilerDiagnostic(
        "myapp: *** something went wrong").has_value());
    EXPECT_FALSE(parseCompilerDiagnostic(
        "make.bat: *** [x] Error 1").has_value());
}

TEST(CompilerDiagnostics, ARealDiagnosticStillWinsOverTheSummaryShape)
{
    // A source line echoed by a compiler could contain "***"; the located
    // forms are tried first, so they keep their own meaning.
    const auto d = parseCompilerDiagnostic("main.cpp:3:5: error: bad *** thing");
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->file, "main.cpp");
    EXPECT_EQ(d->line, 2);
    EXPECT_EQ(d->column, 4);
}


