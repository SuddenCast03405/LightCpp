// ConsoleDecoderTests — the seam behaviour of chunked console output.
//
// ConsoleDecoder is not a widget: it is a small state machine that decides
// where one line ends in a stream that arrives in arbitrary chunks. Its bugs
// are therefore invisible in a screenshot and easy to get wrong — the "\r\n"
// pair split across two reads is the one that shows up as a phantom blank
// line between every two lines of a program's output, which is exactly the
// kind of thing nobody reports.
//
// ConsoleStreamDecoder is the same probe for a stream whose lines do not end
// (the embedded terminal, R2-41). What it must not do is the old workaround
// — decode each chunk raw — because that splits characters at the seam and
// never probes for UTF-8. So the tests below state both halves: an
// unterminated tail comes out now, and a character cut in half stays one
// character.

#include "LightCpp/App/ConsoleDecoder.hpp"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QString>

using LightCpp::App::ConsoleDecoder;
using LightCpp::App::ConsoleStreamDecoder;

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

// ---------------------------------------------------------------------------
// The streaming variant: same probe, no waiting for a terminator.
// ---------------------------------------------------------------------------

TEST(ConsoleStreamDecoder, AnUnterminatedPromptComesOutAtOnce)
{
    // The reason this decoder exists. A shell prompt has no newline for
    // minutes; the terminal channel used to bypass the decoder for exactly
    // this, and paid for it at the seam instead.
    ConsoleStreamDecoder decoder;
    EXPECT_EQ(visible(decoder.decode(QByteArray("D:\\>"))),
              QStringLiteral("D:\\>"));
}

TEST(ConsoleStreamDecoder, AUtf8CharacterSplitAcrossChunksIsOneCharacter)
{
    // U+4E2D is E4 B8 AD. The chunk ends mid-character: nothing is emitted
    // for that half, and the character arrives whole with its continuation.
    ConsoleStreamDecoder decoder;
    EXPECT_EQ(visible(decoder.decode(QByteArray("\xE4\xB8"))), QString());
    EXPECT_EQ(decoder.decode(QByteArray("\xAD")),
              QString::fromUtf8("\xE4\xB8\xAD"));
}

TEST(ConsoleStreamDecoder, ATrailingAsciiCharacterIsNeverHeldBack)
{
    // Holding back more than half a character would make the terminal lag;
    // an ASCII byte cannot be the front of anything, so it goes out now.
    ConsoleStreamDecoder decoder;
    EXPECT_EQ(visible(decoder.decode(QByteArray("abc"))), QStringLiteral("abc"));
    EXPECT_TRUE(decoder.flush().isEmpty());
}

TEST(ConsoleStreamDecoder, ALocalEncodedCharacterSplitAcrossChunksIsOneCharacter)
{
    // The same seam, in the encoding a zh-CN console actually speaks. GBK is
    // double-byte with its lead in 0x81..0xFE, which needs no case of its
    // own: a lone lead reads as a 2-byte lead and is held.
    const QString zhong = QString::fromUtf8("\xE4\xB8\xAD");
    const QByteArray local = zhong.toLocal8Bit();
    if (local.size() != 2) {
        GTEST_SKIP() << "this machine's local code page is not double-byte";
    }
    ConsoleStreamDecoder decoder;
    EXPECT_TRUE(decoder.decode(local.left(1)).isEmpty());
    EXPECT_EQ(decoder.decode(local.mid(1)), zhong);
}

TEST(ConsoleStreamDecoder, Utf8OutputIsProbedNotAssumedToBeLocal)
{
    // The second half of R2-41: nothing probed for UTF-8, so a program that
    // speaks UTF-8 printed mojibake on a GBK machine. Bytes and expected
    // text come from the same source, so this holds whatever the machine's
    // code page is — what it rules out is "decoded as the local code page".
    const QString line = QString::fromUtf8("\xE4\xB8\xAD\xE6\x96\x87\n");
    ConsoleStreamDecoder decoder;
    EXPECT_EQ(visible(decoder.decode(line.toUtf8())),
              visible(QString::fromUtf8("\xE4\xB8\xAD\xE6\x96\x87\n")));
}

TEST(ConsoleStreamDecoder, CrLfSplitAcrossChunksIsStillOneBreak)
{
    // The streaming decoder emits without waiting, so the '\r' is out on its
    // own chunk; the '\n' of the same pair must not become a blank line.
    ConsoleStreamDecoder decoder;
    EXPECT_EQ(visible(decoder.decode(QByteArray("line\r"))),
              QStringLiteral("line\\n"));
    EXPECT_EQ(visible(decoder.decode(QByteArray("\nnext\r\n"))),
              QStringLiteral("next\\n"));
}

TEST(ConsoleStreamDecoder, SeveralChunksInARowAddNothingOfTheirOwn)
{
    // "D:\\>" plus the program's UTF-8 answer, delivered in three reads. The
    // concatenation must equal what the console printed: no lost prompt, no
    // replacement glyph at the seams, no break invented.
    ConsoleStreamDecoder decoder;
    QString text = decoder.decode(QByteArray("D:\\>"));
    text += decoder.decode(QByteArray("\xE4\xB8"));
    text += decoder.decode(QByteArray("\xAD\r\n"));
    text += decoder.flush();
    EXPECT_EQ(text, QStringLiteral("D:\\>") + QString::fromUtf8("\xE4\xB8\xAD")
                     + QStringLiteral("\n"));
}

TEST(ConsoleStreamDecoder, FlushDeliversACharacterThatNeverCompleted)
{
    // The shell was killed between the two halves of a character: the held
    // byte is still text, and dropping it would be a silent edit of what the
    // program printed.
    ConsoleStreamDecoder decoder;
    EXPECT_TRUE(decoder.decode(QByteArray("\xE4\xB8")).isEmpty());
    EXPECT_FALSE(decoder.flush().isEmpty());
    EXPECT_TRUE(decoder.flush().isEmpty());
}

TEST(ConsoleStreamDecoder, BytesThatCannotStartACharacterArePassedThrough)
{
    // 0xFF is not a UTF-8 lead and not a GBK one either. Holding it back
    // would stall the panel on garbage; emitting it turns it into the
    // replacement glyph, which is the honest rendering.
    ConsoleStreamDecoder decoder;
    const QString text = decoder.decode(QByteArray("ok\xFF"));
    EXPECT_TRUE(text.startsWith(QStringLiteral("ok")));
    EXPECT_EQ(text.size(), 3);
    EXPECT_TRUE(decoder.flush().isEmpty());
}
