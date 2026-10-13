#pragma once

#include <QByteArray>
#include <QString>

namespace LightCpp::App {

/// Decodes console output (compiler, build tool, or a running program) into
/// display text.
///
/// Console encodings on Windows are tool-dependent: MSVC's cl.exe and the
/// programs it builds speak the local ANSI code page (GBK on a zh-CN
/// system), while MSYS2/MinGW tools and the programs they build speak
/// UTF-8. Neither decoding alone works for both, so each block is probed:
/// a byte sequence that is well-formed UTF-8 is decoded as UTF-8, anything
/// else falls back to the local code page. (A strict UTF-8 check is what
/// makes the fallback reliable — GBK text is almost never valid UTF-8,
/// while UTF-8 text is never valid GBK.)
///
/// A chunk can also split a multi-byte character in half, so only complete
/// lines are consumed and the tail is kept for the next call.
class ConsoleDecoder {
public:
    /// Decode one output chunk. Returns the text of the complete lines the
    /// chunk holds (empty when no full line has arrived yet); call flush()
    /// when the process ends to pick up a trailing line without a newline.
    QString decode(const QByteArray& chunk);
    /// Decode whatever is left in the buffer (end of process output).
    QString flush();

private:
    QByteArray pending_;
    /// True when the previous decode() ended on a bare '\r' and emitted its
    /// line break. The next chunk may then open with the '\n' of that same
    /// "\r\n" pair, split across the two calls; that '\n' is dropped so the
    /// pair stays one break instead of becoming a blank line.
    bool pendingCarriageReturn_ = false;
};

}  // namespace LightCpp::App
