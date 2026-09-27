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
