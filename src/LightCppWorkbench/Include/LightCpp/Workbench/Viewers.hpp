// Viewers — the two read-only viewers lightcpp.1.md §九 asks for that are
// nothing but a decode of bytes or lines: the hex viewer and the log viewer.
//
// Both are "show me what is in this file" and both have one trap:
//
//   * A hex dump has to keep its offsets *absolute*. A viewer that formats a
//     window starting at 0 and then scrolls by re-reading from a new offset
//     shows the right bytes under the wrong address, which is worse than not
//     showing them.
//   * A log line's level is not always at the start of the line. Java, Python
//     and syslog all put a timestamp first; a classifier that only looks at
//     position 0 marks every line "unknown" and the filter does nothing.
//
// Exports, filtering and the search command live here so the dialogs stay
// small and the behaviour is testable.

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench::Viewer {

// ---------------------------------------------------------------------------
// Hex
// ---------------------------------------------------------------------------

/// One row of a hex dump.
struct HexRow {
    quint64 offset = 0;
    QByteArray bytes;      ///< up to bytesPerRow bytes
    QString ascii;         ///< the printable rendering, '.' for the rest
    bool highlighted = false;  ///< the row contains the current search hit
};

/// Format `data` as rows of `bytesPerRow` bytes, with offsets starting at
/// `baseOffset`. `highlightFrom`/`highlightLength` mark the row(s) a search hit
/// falls in; pass `highlightLength == 0` for no highlight.
[[nodiscard]] QList<HexRow> hexRows(const QByteArray& data,
                                    quint64 baseOffset = 0,
                                    int bytesPerRow = 16,
                                    quint64 highlightFrom = 0,
                                    int highlightLength = 0);

/// The offset of `needle` in `haystack` at or after `from`, or -1.
[[nodiscard]] qint64 findBytes(const QByteArray& haystack,
                               const QByteArray& needle,
                               qint64 from = 0);

/// Parse a search query: "DE AD BE EF", "deadbeef", "\xDE\xAD", or a quoted
/// text ("hello"). Returns an empty array when the query is not valid hex.
[[nodiscard]] QByteArray parseHexQuery(const QString& query, bool* isText = nullptr);

/// `00000010` — an offset padded to the width the file needs, so the column
/// never jumps when the dump scrolls past a power of two.
[[nodiscard]] QString formatOffset(quint64 offset, quint64 fileSize);

/// A whole dump as text (what "Save as…" writes).
[[nodiscard]] QString hexDumpText(const QByteArray& data, quint64 baseOffset = 0,
                                  int bytesPerRow = 16);

/// 1-based line number of the row that contains `offset`.
[[nodiscard]] int rowOfOffset(quint64 offset, quint64 baseOffset, int bytesPerRow);

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

enum class Level {
    Unknown = 0,
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical,
};

[[nodiscard]] QString levelName(Level level);
/// "TRACE", "DEBUG", … — the tag as a log usually prints it.
[[nodiscard]] QString levelTag(Level level);

struct LogLine {
    int index = 0;        ///< 0-based line number in the original text
    Level level = Level::Unknown;
    QString timestamp;    ///< the leading timestamp, when one was recognised
    QString text;         ///< the whole line, unmodified
};

/// Classify one line. Recognises, in order: syslog (`Jan  1 12:00:00 host
/// prog[1]: …`), ISO-8601 (`2024-01-01T12:00:00Z`), bracketed levels
/// (`[ERROR]`, `[warn]`, `(debug)`), `LEVEL:` prefixes, and Python's
/// `logging` format (`2024-01-01 12:00:00,123 - name - ERROR - msg`).
[[nodiscard]] Level classifyLine(const QString& line, QString* timestamp = nullptr);

/// Parse a whole log. Consecutive lines that continue a multi-line entry (a
/// stack trace, an indented continuation) inherit the previous line's level:
/// a stack trace is part of the error above it, not a thousand "unknown"
/// lines that a level filter hides.
[[nodiscard]] QList<LogLine> parseLog(const QString& text);

/// The filter state of the log viewer.
struct LogFilter {
    Level minimumLevel = Level::Unknown;  ///< Unknown = show everything
    QString substring;
    bool useRegex = false;
    bool caseSensitive = false;
    bool onlyErrors = false;              ///< Error and Critical only
};

[[nodiscard]] QList<LogLine> filterLog(const QList<LogLine>& lines,
                                       const LogFilter& filter);

/// How many lines each level contributed — the header of the log viewer.
struct LevelCounts {
    int trace = 0;
    int debug = 0;
    int info = 0;
    int warning = 0;
    int error = 0;
    int critical = 0;
    int unknown = 0;

    [[nodiscard]] int total() const
    {
        return trace + debug + info + warning + error + critical + unknown;
    }
    [[nodiscard]] int problems() const { return warning + error + critical; }
};

[[nodiscard]] LevelCounts countLevels(const QList<LogLine>& lines);

/// One-line summary: "1 234 lines — 3 errors, 12 warnings".
[[nodiscard]] QString describeCounts(const LevelCounts& counts);

/// The text of the lines that a filter kept, for "Save filtered log…".
[[nodiscard]] QString joinLines(const QList<LogLine>& lines);

// ---------------------------------------------------------------------------
// Binary sniffing (image / resource preview, §九 "图像 / 资源预览")
// ---------------------------------------------------------------------------

/// What a file's first bytes say it is. The image preview only opens formats
/// Qt can actually decode; naming the real type first turns "cannot display"
/// into "this is a JPEG with a .bin extension".
enum class BinaryKind {
    Unknown = 0,
    Png,
    Jpeg,
    Gif,
    Bmp,
    WebP,
    Svg,
    Ico,
    Pdf,
    Zip,
    Elf,
    Pe,
    MachO,
    Text,
};

[[nodiscard]] BinaryKind sniffBytes(const QByteArray& head);
[[nodiscard]] QString binaryKindName(BinaryKind kind);
[[nodiscard]] bool isImageKind(BinaryKind kind);

}  // namespace LightCpp::Workbench::Viewer
