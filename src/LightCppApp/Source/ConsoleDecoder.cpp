// ConsoleDecoder — console-output decoding with UTF-8 / local code page probe.

#include "LightCpp/App/ConsoleDecoder.hpp"

#include <utility>

namespace LightCpp::App {
namespace {

/// True when `data` is entirely well-formed UTF-8. Hand-rolled (strict,
/// overlong/surrogate-aware) rather than Qt's lenient conversion so the
/// GBK fallback only fires for text that genuinely is not UTF-8.
bool isWellFormedUtf8(const QByteArray& data)
{
    const auto* p = reinterpret_cast<const unsigned char*>(data.constData());
    const auto* end = p + data.size();
    while (p < end) {
        const unsigned char lead = *p++;
        if (lead < 0x80) {
            continue;  // ASCII, including '\n' and '\r'
        }
        int continuation = 0;
        unsigned int codePoint = 0;
        if ((lead & 0xE0) == 0xC0) {
            continuation = 1;
            codePoint = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            continuation = 2;
            codePoint = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            continuation = 3;
            codePoint = lead & 0x07;
        } else {
            return false;
        }
        if (end - p < continuation) {
            return false;
        }
        for (int i = 0; i < continuation; ++i) {
            const unsigned char byte = *p++;
            if ((byte & 0xC0) != 0x80) {
                return false;
            }
            codePoint = (codePoint << 6) | (byte & 0x3F);
        }
        // Overlong encodings, UTF-16 surrogates and out-of-range values are
        // not well-formed; rejecting them keeps GBK pairs that happen to
        // resemble a 3-byte UTF-8 sequence from being misdecoded.
        if ((continuation == 1 && codePoint < 0x80)
            || (continuation == 2 && codePoint < 0x800)
            || (continuation == 3 && codePoint < 0x10000)
            || (codePoint >= 0xD800 && codePoint <= 0xDFFF)
            || codePoint > 0x10FFFF) {
            return false;
        }
    }
    return true;
}

QString decodeConsoleBlock(const QByteArray& block)
{
    if (isWellFormedUtf8(block)) {
        return QString::fromUtf8(block);
    }
    return QString::fromLocal8Bit(block);
}

/// Split `data` on '\r\n' / '\r' / '\n', decoding each piece and rejoining
/// with '\n'. Shared: both decoders probe line by line for the same reason
/// (one chunk can mix the program's UTF-8 output with the local-encoded echo
/// of what was typed into it, and line boundaries are always ASCII).
QString decodeLines(const QByteArray& data, bool emitUnterminatedTail)
{
    QString result;
    int start = 0;
    const int size = data.size();
    for (int i = 0; i < size; ++i) {
        const char c = data[i];
        if (c != '\n' && c != '\r') {
            continue;
        }
        result += decodeConsoleBlock(data.mid(start, i - start));
        if (c == '\r') {
            // Fold "\r\n" into one break; a lone '\r' becomes a break too
            // (the panel cannot overwrite a line the way a console does,
            // and a visible update beats a silently swallowed one).
            if (i + 1 < size && data[i + 1] == '\n') {
                ++i;
            }
        }
        result += QLatin1Char('\n');
        start = i + 1;
    }
    if (start < size && emitUnterminatedTail) {
        // No terminator: a shell prompt, or a program that has not printed
        // its newline yet. Only the stream decoder emits it now — that is
        // the entire difference between the two.
        result += decodeConsoleBlock(data.mid(start));
    }
    return result;
}

/// How many bytes at the end of `data` may be the front of a character whose
/// remaining bytes have not arrived yet (0..3).
///
/// Walked **forwards**, not read back from the end: a GBK trail byte can look
/// exactly like a UTF-8 lead (0xD0 in D6 D0 is 110 10000), so asking "what is
/// the last byte?" holds back a byte of a pair that is already complete and
/// then decodes its first half on its own — the replacement glyph this
/// decoder exists to prevent. Scanning forwards keeps a running position, so
/// the pair is recognised as a pair and only a genuinely unfinished sequence
/// at the very end is held.
///
/// The rule per position: ASCII advances one; a UTF-8 lead takes its
/// continuations (if the buffer ends inside them, that is the hold); anything
/// else that a local double-byte code page could introduce (0x81..0xFE) takes
/// one more byte, or is held when it is the last byte. 0x80 and 0xFF start a
/// character in neither encoding, so they pass through and turn into the
/// honest replacement glyph instead of stalling the panel.
int incompleteTailLength(const QByteArray& data)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.constData());
    const int size = data.size();
    int i = 0;
    while (i < size) {
        const unsigned char byte = bytes[i];
        if (byte < 0x80) {
            ++i;
            continue;
        }
        int need = 0;
        if ((byte & 0xE0) == 0xC0) {
            need = 2;
        } else if ((byte & 0xF0) == 0xE0) {
            need = 3;
        } else if ((byte & 0xF8) == 0xF0) {
            need = 4;
        }
        if (need != 0) {
            int have = 1;
            while (have < need && i + have < size && (bytes[i + have] & 0xC0) == 0x80) {
                ++have;
            }
            if (have == need) {
                i += need;
                continue;
            }
            if (i + have == size) {
                return size - i;  // the chunk ended inside a UTF-8 sequence
            }
            // A non-continuation interrupted it: this is not UTF-8 here, so
            // fall through to the double-byte reading below.
        }
        if (byte >= 0x81 && byte <= 0xFE) {
            if (i + 1 < size) {
                i += 2;
                continue;
            }
            return 1;  // a lone lead byte may be the front of a pair
        }
        ++i;
    }
    return 0;
}

}  // namespace

QString ConsoleDecoder::decode(const QByteArray& chunk)
{
    pending_ += chunk;
    // A "\r\n" pair split across two calls: the previous call ended on the
    // '\r', emitted its break and left nothing (a bare '\r' is a terminator
    // on its own, see below). If this chunk then opens with that pair's '\n',
    // counting it again would add a blank line the program never printed.
    if (pendingCarriageReturn_) {
        pendingCarriageReturn_ = false;
        if (pending_.startsWith('\n')) {
            pending_.remove(0, 1);
        }
    }
    // Consume only up to the last terminator: a multi-byte character split
    // across two chunks would otherwise be misjudged at the seam.
    // '\r' counts as a terminator too — progress bars and prompts carry
    // their updates on bare '\r', and waiting for a '\n' that never comes
    // made interactive programs look hung.
    const int lastNewline = qMax(pending_.lastIndexOf('\n'),
                                 pending_.lastIndexOf('\r'));
    if (lastNewline < 0) {
        return {};
    }
    const QByteArray complete = pending_.left(lastNewline + 1);
    pending_.remove(0, lastNewline + 1);

    QString result = decodeLines(complete, /*emitUnterminatedTail=*/false);
    // A trailing bare '\r' inside this call emitted its break, but if its '\n'
    // arrives with the next chunk we must not count that '\n' again.
    pendingCarriageReturn_ = !complete.isEmpty() && complete.back() == '\r';
    return result;
}

QString ConsoleDecoder::flush()
{
    pendingCarriageReturn_ = false;
    if (pending_.isEmpty()) {
        return {};
    }
    const QByteArray rest = std::move(pending_);
    pending_.clear();
    return decodeConsoleBlock(rest);
}

QString ConsoleStreamDecoder::decode(const QByteArray& chunk)
{
    pending_ += chunk;
    // Same "\r\n"-across-chunks rule as the line decoder above.
    if (pendingCarriageReturn_) {
        pendingCarriageReturn_ = false;
        if (pending_.startsWith('\n')) {
            pending_.remove(0, 1);
        }
    }
    // Everything except a possible half character at the very end goes out
    // now: a prompt without a newline is the whole reason this decoder
    // exists, and holding a whole line back is what it must not do.
    const int hold = incompleteTailLength(pending_);
    const QByteArray usable = pending_.left(pending_.size() - hold);
    pending_ = pending_.right(hold);
    if (usable.isEmpty()) {
        return {};
    }
    pendingCarriageReturn_ = usable.back() == '\r';
    return decodeLines(usable, /*emitUnterminatedTail=*/true);
}

QString ConsoleStreamDecoder::flush()
{
    pendingCarriageReturn_ = false;
    if (pending_.isEmpty()) {
        return {};
    }
    const QByteArray rest = std::move(pending_);
    pending_.clear();
    return decodeConsoleBlock(rest);
}

}  // namespace LightCpp::App
