// ConsoleDecoderTests — the seam behaviour of chunked console output.
//
// ConsoleDecoder is not a widget: it is a small state machine that decides
// where one line ends in a stream that arrives in arbitrary chunks. Its bugs
// are therefore invisible in a screenshot and easy to get wrong — the "\r\n"
// pair split across two reads is the one that shows up as a phantom blank
// line between every two lines of a program's output, which is exactly the
// kind of thing nobody reports.

#include "LightCpp/App/ConsoleDecoder.hpp"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>

using LightCpp::App::ConsoleDecoder;

namespace {

/// Readable form of a decoded result: newlines shown as "\n".
QString visible(const QString& text)
{
    QString out = text;
    out.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return out;
}

}  // namespace

TEST(ConsoleDecoder, WholeCrLfIsOneBreak)
{
    ConsoleDecoder decoder;
    EXPECT_EQ(visible(decoder.decode(QByteArray("a\r\nb\n"))),
              QStringLiteral("a\\nb\\n"));
}

TEST(ConsoleDecoder, BareCarriageReturnFlushesItsLine)
{
    // A progress bar carries its updates on a bare '\r' with no '\n' in
    // sight; waiting for a '\n' would make the program look hung.
    ConsoleDecoder decoder;
    EXPECT_EQ(visible(decoder.decode(QByteArray("50%\r"))),
              QStringLiteral("50%\\n"));
}

TEST(ConsoleDecoder, CrLfSplitAcrossChunksIsOneBreak)
{
    // The bug: chunk one ends on the '\r' (emitted as a break), chunk two
    // opens with that pair's '\n'. Counting the '\n' separately inserted a
    // blank line the program never printed.
    ConsoleDecoder decoder;
    EXPECT_EQ(visible(decoder.decode(QByteArray("line\r"))),
              QStringLiteral("line\\n"));
    EXPECT_EQ(visible(decoder.decode(QByteArray("\nnext\n"))),
              QStringLiteral("next\\n"));
    EXPECT_EQ(visible(decoder.flush()), QString());
}

TEST(ConsoleDecoder, SplitCrLfLeavesNothingPending)
{
    // Same split, but the pair is the last thing the program writes: the '\n'
    // must not survive to flush() as a stray break either.
    ConsoleDecoder decoder;
    EXPECT_EQ(visible(decoder.decode(QByteArray("only\r"))),
              QStringLiteral("only\\n"));
    EXPECT_EQ(visible(decoder.decode(QByteArray("\n"))), QString());
    EXPECT_EQ(visible(decoder.flush()), QString());
}

TEST(ConsoleDecoder, LoneCarriageReturnIsNotLostAcrossChunks)
{
    // A bare '\r' followed by unrelated text (a real progress update, not a
    // CRLF): the break still happens and the next line is untouched.
    ConsoleDecoder decoder;
    EXPECT_EQ(visible(decoder.decode(QByteArray("10%\r"))),
              QStringLiteral("10%\\n"));
    EXPECT_EQ(visible(decoder.decode(QByteArray("20%\r"))),
              QStringLiteral("20%\\n"));
    EXPECT_EQ(visible(decoder.decode(QByteArray("done\n"))),
              QStringLiteral("done\\n"));
}

TEST(ConsoleDecoder, HoldsBackAnIncompleteLine)
{
    // No terminator yet: nothing is emitted (the tail could be half of a
    // multi-byte character), and flush() delivers it at end of output.
    ConsoleDecoder decoder;
    EXPECT_EQ(visible(decoder.decode(QByteArray("partial"))), QString());
    EXPECT_EQ(visible(decoder.flush()), QStringLiteral("partial"));
}

TEST(ConsoleDecoder, MultiByteCharacterSplitAcrossChunksIsWhole)
{
    // U+4E2D is E4 B8 AD: splitting the bytes must not corrupt it — the
    // decoder only commits complete lines, so the seam is always on ASCII.
    ConsoleDecoder decoder;
    EXPECT_EQ(visible(decoder.decode(QByteArray("\xE4\xB8"))), QString());
    EXPECT_EQ(visible(decoder.decode(QByteArray("\xAD\n"))),
              QString::fromUtf8("\xE4\xB8\xAD") + QStringLiteral("\\n"));
}

TEST(ConsoleDecoder, ChunkWithoutTerminatorReturnsEmpty)
{
    ConsoleDecoder decoder;
    EXPECT_TRUE(decoder.decode(QByteArray("no newline here")).isEmpty());
}
