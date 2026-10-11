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

}  // namespace

QString ConsoleDecoder::decode(const QByteArray& chunk)
{
    pending_ += chunk;
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

    // Probe line by line, not block by block: one pipe chunk can mix the
    // program's own UTF-8 output with the GBK echo of what was typed into
    // it, and a whole-block probe would then garble the UTF-8 half. Line
    // boundaries are ASCII, so splitting is always safe. A "\r\n" pair is
    // one terminator; a bare '\r' also flushes its line.
    QString result;
    int start = 0;
    const int size = complete.size();
    for (int i = 0; i < size; ++i) {
        const char c = complete[i];
        if (c != '\n' && c != '\r') {
            continue;
        }
        result += decodeConsoleBlock(complete.mid(start, i - start));
        if (c == '\r') {
            // Fold "\r\n" into one break; a lone '\r' becomes a break too
            // (the panel cannot overwrite a line the way a console does,
            // and a visible update beats a silently swallowed one).
            if (i + 1 < size && complete[i + 1] == '\n') {
                ++i;
            }
        }
        result += QLatin1Char('\n');
        start = i + 1;
    }
    return result;
}

QString ConsoleDecoder::flush()
{
    if (pending_.isEmpty()) {
        return {};
    }
    const QByteArray rest = std::move(pending_);
    pending_.clear();
    return decodeConsoleBlock(rest);
}

}  // namespace LightCpp::App
