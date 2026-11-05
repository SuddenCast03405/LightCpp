// TextColumnsTests — the compiler's column read in the editor's units.
//
// R2-44: gcc counts columns in display cells, QTextDocument counts UTF-16 code
// units, and the two agree only on ASCII. Measured on this machine with
//
//     int 变量 = 未定义;
//
// gcc reported column 16 by default (15 once the one-based origin is removed)
// where the token really starts at character 13 — one cell too far for each CJK
// character before it. The first test below is that measured line.

#include "LightCpp/App/TextColumns.hpp"

#include <gtest/gtest.h>

#include <QString>

using LightCpp::App::TextColumns::utf16ColumnForDisplayColumn;

TEST(TextColumns, AsciiColumnsAreTheirOwnOffsets)
{
    const QString line = QStringLiteral("int x = 1;");
    EXPECT_EQ(utf16ColumnForDisplayColumn(line, 0), 0);
    EXPECT_EQ(utf16ColumnForDisplayColumn(line, 4), 4);   // the 'x'
    EXPECT_EQ(utf16ColumnForDisplayColumn(line, 10), 10);
}

TEST(TextColumns, TheMeasuredGccLine)
{
    // The exact line the column unit was measured on: `未定义` starts 15
    // display cells in (gcc says 16, one-based) and 13 characters in.
    const QString line = QStringLiteral("    int \u53D8\u91CF = \u672A\u5B9A\u4E49;");
    ASSERT_EQ(line.size(), 17);
    const int column = utf16ColumnForDisplayColumn(line, 15);
    EXPECT_EQ(column, 13);
    EXPECT_EQ(line.at(column), QChar(0x672A));   // '未' — what gcc pointed at
    // Handing gcc's two spare cells straight to the editor lands two
    // characters further along, on '义'.
    EXPECT_EQ(line.at(15), QChar(0x4E49));
}

TEST(TextColumns, EachCjkCharacterIsTwoCellsButOneUnit)
{
    // "中文x" — the 'x' is cell 4 but character 2.
    const QString line = QStringLiteral("\u4E2D\u6587x");
    EXPECT_EQ(utf16ColumnForDisplayColumn(line, 0), 0);
    EXPECT_EQ(utf16ColumnForDisplayColumn(line, 2), 1);   // '文'
    EXPECT_EQ(utf16ColumnForDisplayColumn(line, 4), 2);   // 'x'
}

TEST(TextColumns, AColumnInsideAWideCharacterResolvesToIt)
{
    // Cell 1 is the second half of '中'; the compiler never reports it, but a
    // mangled line must not push the caret past the character.
    const QString line = QStringLiteral("\u4E2D\u6587x");
    EXPECT_EQ(utf16ColumnForDisplayColumn(line, 1), 0);
    EXPECT_EQ(utf16ColumnForDisplayColumn(line, 3), 1);
}

TEST(TextColumns, ASupplementaryCharacterIsTwoCellsAndTwoUnits)
{
    // U+1F600: one code point, two cells wide, a surrogate pair in UTF-16.
    const QString line = QStringLiteral("\U0001F600x");
    ASSERT_EQ(line.size(), 3);                            // 2 units + 'x'
    EXPECT_EQ(utf16ColumnForDisplayColumn(line, 2), 2);
}

TEST(TextColumns, ACombiningMarkTakesNoCell)
{
    // "é" written as 'e' + U+0301 is three bytes and two code units but one
    // cell — the case that makes a naive "one cell per character" wrong.
    const QString line = QStringLiteral("e\u0301x");
    ASSERT_EQ(line.size(), 3);
    EXPECT_EQ(utf16ColumnForDisplayColumn(line, 1), 2);   // 'x'
}

TEST(TextColumns, AColumnPastTheEndStopsAtTheLineEnd)
{
    const QString line = QStringLiteral("\u4E2D\u6587");
    EXPECT_EQ(utf16ColumnForDisplayColumn(line, 999), 2);
}

TEST(TextColumns, NoColumnIsColumnZero)
{
    EXPECT_EQ(utf16ColumnForDisplayColumn(QStringLiteral("\u4E2D\u6587"), 0), 0);
    EXPECT_EQ(utf16ColumnForDisplayColumn(QStringLiteral("\u4E2D\u6587"), -5), 0);
    EXPECT_EQ(utf16ColumnForDisplayColumn(QString(), 3), 0);
}
