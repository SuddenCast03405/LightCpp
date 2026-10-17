// MsvcNamesTests — what the toolchain is called, and where 32-bit programs are.
//
// R2-57 reported two separate slips that both made an installed Visual Studio
// look absent or unnamed:
//   * the label printed the installer's own version back at the user —
//     "Visual Studio 17.10", which is not a product that exists;
//   * when the ProgramFiles(x86) variable was missing, the vswhere lookup fell
//     back to a hard-coded "C:/Program Files (x86)" — a path that is wrong both
//     when Windows lives elsewhere and when 32-bit programs were relocated, and
//     whose failure reads as "No Visual Studio installation was found".

#include "LightCpp/App/MsvcNames.hpp"

#include <gtest/gtest.h>

#include <QString>

using LightCpp::App::MsvcNames::productName;
using LightCpp::App::MsvcNames::programFilesX86From;

TEST(MsvcNames, TheInstallersVersionIsNotTheProductName)
{
    // vswhere reports "17.10.34427.2"; the score keeps major and minor.
    EXPECT_EQ(productName(17010), QStringLiteral("Visual Studio 2022 (17.10)"));
    // The reported string: there is no "Visual Studio 17.10".
    EXPECT_NE(productName(17010), QStringLiteral("Visual Studio 17.10"));
}

TEST(MsvcNames, AWholeYearReleaseNeedsNoParenthesis)
{
    EXPECT_EQ(productName(17000), QStringLiteral("Visual Studio 2022"));
    EXPECT_EQ(productName(16000), QStringLiteral("Visual Studio 2019"));
    EXPECT_EQ(productName(15000), QStringLiteral("Visual Studio 2017"));
    EXPECT_EQ(productName(14000), QStringLiteral("Visual Studio 2015"));
}

TEST(MsvcNames, TheOldestLayoutsStillNameThemselves)
{
    // Visual C++ 6.0 has no marketing year; the numbers are all there is.
    EXPECT_EQ(productName(6000), QStringLiteral("Visual Studio 6.0"));
    EXPECT_EQ(productName(7000), QStringLiteral("Visual Studio 2002"));
}

TEST(MsvcNames, AnUnknownMajorFallsBackToNumbersInsteadOfGuessingAYear)
{
    // A release newer than this table must not be labelled with the previous
    // release's year.
    EXPECT_EQ(productName(19000), QStringLiteral("Visual Studio 19.0"));
    EXPECT_EQ(productName(19010), QStringLiteral("Visual Studio 19.10"));
}

TEST(MsvcNames, NothingReadYetIsNotAVersion)
{
    EXPECT_EQ(productName(0), QStringLiteral("Visual Studio"));
}

TEST(MsvcNames, TheEnvironmentVariableWinsOverTheRegistryAndTheSystemDrive)
{
    EXPECT_EQ(programFilesX86From(QStringLiteral("D:/Program Files (x86)"),
                                  QStringLiteral("E:/Program Files (x86)"),
                                  QStringLiteral("F:")),
              QStringLiteral("D:/Program Files (x86)"));
}

TEST(MsvcNames, TheRegistryAnswerIsUsedWhenTheVariableIsMissing)
{
    // The registry stores native separators; the answer is a path Qt can join.
    EXPECT_EQ(programFilesX86From(QString(),
                                  QStringLiteral("D:\\Program Files (x86)"),
                                  QStringLiteral("C:")),
              QStringLiteral("D:/Program Files (x86)"));
}

TEST(MsvcNames, TheSystemDriveIsTheLastThingAsked)
{
    // Note the drive: this is the case the hard-coded C: got wrong.
    EXPECT_EQ(programFilesX86From(QString(), QString(), QStringLiteral("D:")),
              QStringLiteral("D:/Program Files (x86)"));
    // A drive that already ends in a separator must not produce a double one.
    EXPECT_EQ(programFilesX86From(QString(), QString(), QStringLiteral("D:/")),
              QStringLiteral("D:/Program Files (x86)"));
}

TEST(MsvcNames, WithNothingKnownThereIsNoGuessedPath)
{
    // The whole point: no "C:/Program Files (x86)" appears out of thin air.
    EXPECT_TRUE(programFilesX86From(QString(), QString(), QString()).isEmpty());
    // Whitespace is not an answer either — an empty variable is empty.
    EXPECT_TRUE(programFilesX86From(QStringLiteral("   "), QStringLiteral("\t"),
                                    QStringLiteral(" "))
                    .isEmpty());
}
