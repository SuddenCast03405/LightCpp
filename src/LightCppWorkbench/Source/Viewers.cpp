// Viewers — implementation. See the header for the two traps.

#include "LightCpp/Workbench/Viewers.hpp"

#include <QRegularExpression>

namespace LightCpp::Workbench::Viewer {
namespace {

QString asciiOf(const QByteArray& bytes)
{
    QString text;
    text.reserve(bytes.size());
    for (const char raw : bytes) {
        const auto byte = static_cast<unsigned char>(raw);
        text.append(byte >= 0x20 && byte < 0x7f ? QLatin1Char(static_cast<char>(byte))
                                                : QLatin1Char('.'));
    }
    return text;
}

bool isHexDigits(const QString& text)
{
    if (text.isEmpty()) {
        return false;
    }
    for (const QChar ch : text) {
        if (!((ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
              || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'))
              || (ch >= QLatin1Char('A') && ch <= QLatin1Char('F')))) {
            return false;
        }
    }
    return true;
}

/// Order matters: the most specific tag first, because "ERROR" contains "ERR"
/// and "WARNING" contains "WARN". The check is anchored on a word boundary so
/// "errors: 3" in an ordinary message is not a level tag.
Level levelFromWord(const QString& word)
{
    const QString upper = word.toUpper();
    if (upper == QLatin1String("TRACE") || upper == QLatin1String("TRC")
        || upper == QLatin1String("VERBOSE")) {
        return Level::Trace;
    }
    if (upper == QLatin1String("DEBUG") || upper == QLatin1String("DBG")) {
        return Level::Debug;
    }
    if (upper == QLatin1String("INFO") || upper == QLatin1String("INF")
        || upper == QLatin1String("NOTICE")) {
        return Level::Info;
    }
    if (upper == QLatin1String("WARN") || upper == QLatin1String("WARNING")
        || upper == QLatin1String("WRN")) {
        return Level::Warning;
    }
    if (upper == QLatin1String("ERROR") || upper == QLatin1String("ERR")
        || upper == QLatin1String("SEVERE")) {
        return Level::Error;
    }
    if (upper == QLatin1String("FATAL") || upper == QLatin1String("CRITICAL")
        || upper == QLatin1String("CRIT") || upper == QLatin1String("PANIC")
        || upper == QLatin1String("ALERT") || upper == QLatin1String("EMERG")) {
        return Level::Critical;
    }
    return Level::Unknown;
}

/// Try to read a leading timestamp and return the rest of the line.
struct Split {
    QString timestamp;
    QString rest;
};

Split splitTimestamp(const QString& line)
{
    static const QRegularExpression iso(
        QStringLiteral("^(\\d{4}-\\d{2}-\\d{2}[T ]\\d{2}:\\d{2}:\\d{2}(?:[.,]\\d{1,9})?"
                       "(?:Z|[+-]\\d{2}:?\\d{2})?)\\s+(.*)$"));
    static const QRegularExpression syslog(
        QStringLiteral("^([A-Z][a-z]{2}\\s+\\d{1,2}\\s+\\d{2}:\\d{2}:\\d{2})\\s+(.*)$"));
    static const QRegularExpression timeOnly(
        QStringLiteral("^(\\d{2}:\\d{2}:\\d{2}(?:[.,]\\d{1,9})?)\\s+(.*)$"));

    for (const auto* pattern : {&iso, &syslog, &timeOnly}) {
        const auto match = pattern->match(line);
        if (match.hasMatch()) {
            Split split;
            split.timestamp = match.captured(1);
            split.rest = match.captured(2);
            return split;
        }
    }
    return Split{QString(), line};
}

}  // namespace

// ---------------------------------------------------------------------------
// Hex
// ---------------------------------------------------------------------------

QList<HexRow> hexRows(const QByteArray& data, quint64 baseOffset, int bytesPerRow,
                      quint64 highlightFrom, int highlightLength)
{
    QList<HexRow> rows;
    if (bytesPerRow <= 0) {
        return rows;
    }
    const quint64 end = baseOffset + static_cast<quint64>(data.size());
    const quint64 highlightEnd = highlightFrom + static_cast<quint64>(qMax(0, highlightLength));
    for (qsizetype offset = 0; offset < data.size(); offset += bytesPerRow) {
        HexRow row;
        row.offset = baseOffset + static_cast<quint64>(offset);
        row.bytes = data.mid(offset, bytesPerRow);
        row.ascii = asciiOf(row.bytes);
        if (highlightLength > 0 && highlightFrom < end
            && highlightFrom < row.offset + static_cast<quint64>(row.bytes.size())
            && highlightEnd > row.offset) {
            row.highlighted = true;
        }
        rows.append(row);
    }
    return rows;
}

qint64 findBytes(const QByteArray& haystack, const QByteArray& needle, qint64 from)
{
    if (needle.isEmpty() || haystack.isEmpty()) {
        return -1;
    }
    if (from < 0) {
        from = 0;
    }
    if (from >= haystack.size()) {
        return -1;
    }
    return haystack.indexOf(needle, static_cast<qsizetype>(from));
}

QByteArray parseHexQuery(const QString& query, bool* isText)
{
    if (isText != nullptr) {
        *isText = false;
    }
    const QString trimmed = query.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    // A quoted query is text, whatever it looks like.
    if (trimmed.size() >= 2 && trimmed.startsWith(QLatin1Char('"'))
        && trimmed.endsWith(QLatin1Char('"'))) {
        if (isText != nullptr) {
            *isText = true;
        }
        return trimmed.mid(1, trimmed.size() - 2).toUtf8();
    }
    // "\xDE\xAD" — the C-escape form.
    if (trimmed.startsWith(QLatin1String("\\x"))) {
        QByteArray bytes;
        QString compact = trimmed;
        compact.remove(QLatin1String("\\x"));
        compact.remove(QLatin1Char(' '));
        compact.remove(QLatin1Char('\t'));
        if (!isHexDigits(compact) || compact.size() % 2 != 0) {
            return {};
        }
        return QByteArray::fromHex(compact.toLatin1());
    }
    // "DE AD BE EF", "DEADBEEF", "de-ad-be-ef".
    QString compact = trimmed;
    compact.remove(QLatin1Char(' '));
    compact.remove(QLatin1Char('-'));
    compact.remove(QLatin1Char(':'));
    if (isHexDigits(compact) && compact.size() % 2 == 0) {
        return QByteArray::fromHex(compact.toLatin1());
    }
    // Anything else is a text search — the useful default for a grep box.
    if (isText != nullptr) {
        *isText = true;
    }
    return trimmed.toUtf8();
}

QString formatOffset(quint64 offset, quint64 fileSize)
{
    // The column is as wide as the largest offset the file can have: 4 hex
    // digits up to 64 KiB, more above. A fixed 8 is fine for the sizes a hex
    // viewer is used on and never wraps.
    Q_UNUSED(offset);
    int width = 8;
    if (fileSize <= 0xFFFFull) {
        width = 4;
    } else if (fileSize <= 0xFFFFFFull) {
        width = 6;
    } else if (fileSize <= 0xFFFFFFFFull) {
        width = 8;
    } else {
        width = 16;
    }
    return QStringLiteral("%1").arg(offset, width, 16, QLatin1Char('0')).toUpper();
}

QString hexDumpText(const QByteArray& data, quint64 baseOffset, int bytesPerRow)
{
    QStringList out;
    const QList<HexRow> rows = hexRows(data, baseOffset, bytesPerRow);
    for (const HexRow& row : rows) {
        QString hex;
        for (int i = 0; i < bytesPerRow; ++i) {
            if (i < row.bytes.size()) {
                hex += QStringLiteral("%1 ").arg(
                    static_cast<quint8>(row.bytes.at(i)), 2, 16, QLatin1Char('0')).toUpper();
            } else {
                hex += QStringLiteral("   ");
            }
        }
        out.append(QStringLiteral("%1  %2 %3")
                       .arg(formatOffset(row.offset, baseOffset + data.size()), hex, row.ascii));
    }
    return out.join(QLatin1Char('\n'));
}

int rowOfOffset(quint64 offset, quint64 baseOffset, int bytesPerRow)
{
    if (bytesPerRow <= 0 || offset < baseOffset) {
        return 0;
    }
    return static_cast<int>((offset - baseOffset) / static_cast<quint64>(bytesPerRow)) + 1;
}

// ---------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------

QString levelName(Level level)
{
    switch (level) {
    case Level::Trace:    return QStringLiteral("trace");
    case Level::Debug:    return QStringLiteral("debug");
    case Level::Info:     return QStringLiteral("info");
    case Level::Warning:  return QStringLiteral("warning");
    case Level::Error:    return QStringLiteral("error");
    case Level::Critical: return QStringLiteral("critical");
    case Level::Unknown:  break;
    }
    return QStringLiteral("unknown");
}

QString levelTag(Level level)
{
    switch (level) {
    case Level::Trace:    return QStringLiteral("TRACE");
    case Level::Debug:    return QStringLiteral("DEBUG");
    case Level::Info:     return QStringLiteral("INFO");
    case Level::Warning:  return QStringLiteral("WARN");
    case Level::Error:    return QStringLiteral("ERROR");
    case Level::Critical: return QStringLiteral("FATAL");
    case Level::Unknown:  break;
    }
    return QStringLiteral("LOG");
}

Level classifyLine(const QString& line, QString* timestamp)
{
    if (timestamp != nullptr) {
        timestamp->clear();
    }
    const Split split = splitTimestamp(line);
    if (timestamp != nullptr) {
        *timestamp = split.timestamp;
    }
    const QString rest = split.rest;

    // [ERROR] / <error> / (warn) / [   INFO] — a bracketed tag anywhere in the
    // first 64 characters. Bounded because a message body may contain the
    // word later on.
    static const QRegularExpression bracketed(
        QStringLiteral("[\\[\\(<]\\s*([A-Za-z]{3,8})\\s*[\\]\\)>]"));
    const qsizetype window = qMin<qsizetype>(rest.size(), 64);
    auto match = bracketed.match(rest.left(window));
    if (match.hasMatch()) {
        const Level level = levelFromWord(match.captured(1));
        if (level != Level::Unknown) {
            return level;
        }
    }

    // "ERROR:" / "ERROR " at the start of what remains.
    static const QRegularExpression prefixed(
        QStringLiteral("^([A-Za-z]{3,8})\\b\\s*:?\\s"));
    match = prefixed.match(rest);
    if (match.hasMatch()) {
        const Level level = levelFromWord(match.captured(1));
        if (level != Level::Unknown) {
            return level;
        }
    }

    // Last resort: a separators-delimited tag, as Python's logging emits
    // ("2024-01-01 12:00:00,123 - name - ERROR - msg").
    static const QRegularExpression dashSeparated(
        QStringLiteral("(?:^|\\s-\\s)([A-Za-z]{3,8})(?:\\s-\\s|$)"));
    match = dashSeparated.match(rest.left(qMin<qsizetype>(rest.size(), 160)));
    if (match.hasMatch()) {
        const Level level = levelFromWord(match.captured(1));
        if (level != Level::Unknown) {
            return level;
        }
    }
    return Level::Unknown;
}

QList<LogLine> parseLog(const QString& text)
{
    QList<LogLine> lines;
    const QStringList raw = text.split(QLatin1Char('\n'));
    lines.reserve(raw.size());
    Level previous = Level::Unknown;
    bool previousIsContinuable = false;
    for (int i = 0; i < raw.size(); ++i) {
        const QString& line = raw.at(i);
        if (i == raw.size() - 1 && line.isEmpty()) {
            break;  // the trailing newline is not a line
        }
        LogLine entry;
        entry.index = i;
        entry.text = line;
        const Level level = classifyLine(line, &entry.timestamp);
        const bool indented = !line.isEmpty()
                              && (line.startsWith(QLatin1Char(' '))
                                  || line.startsWith(QLatin1Char('\t')));
        if (level == Level::Unknown && indented && previousIsContinuable) {
            // A stack frame under an error is part of that error.
            entry.level = previous;
        } else {
            entry.level = level;
        }
        previous = entry.level;
        // Only an entry with a level can own continuation lines; a run of
        // indented lines with nothing above them stays "unknown".
        previousIsContinuable = entry.level != Level::Unknown;
        lines.append(entry);
    }
    return lines;
}

QList<LogLine> filterLog(const QList<LogLine>& lines, const LogFilter& filter)
{
    QList<LogLine> out;
    QRegularExpression regex;
    bool useRegex = filter.useRegex;
    if (useRegex) {
        regex.setPattern(filter.substring);
        regex.setPatternOptions(filter.caseSensitive ? QRegularExpression::NoPatternOption
                                                     : QRegularExpression::CaseInsensitiveOption);
        if (!regex.isValid()) {
            // A half-typed regex must not empty the view; fall back to a
            // substring search so the user sees their text while typing.
            useRegex = false;
        }
    }
    for (const LogLine& line : lines) {
        if (filter.onlyErrors) {
            if (line.level != Level::Error && line.level != Level::Critical) {
                continue;
            }
        } else if (filter.minimumLevel != Level::Unknown
                   && static_cast<int>(line.level) < static_cast<int>(filter.minimumLevel)) {
            continue;
        }
        if (!filter.substring.isEmpty()) {
            if (useRegex) {
                if (!regex.match(line.text).hasMatch()) {
                    continue;
                }
            } else {
                const Qt::CaseSensitivity sensitivity =
                    filter.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
                if (!line.text.contains(filter.substring, sensitivity)) {
                    continue;
                }
            }
        }
        out.append(line);
    }
    return out;
}

LevelCounts countLevels(const QList<LogLine>& lines)
{
    LevelCounts counts;
    for (const LogLine& line : lines) {
        switch (line.level) {
        case Level::Trace:    ++counts.trace; break;
        case Level::Debug:    ++counts.debug; break;
        case Level::Info:     ++counts.info; break;
        case Level::Warning:  ++counts.warning; break;
        case Level::Error:    ++counts.error; break;
        case Level::Critical: ++counts.critical; break;
        case Level::Unknown:  ++counts.unknown; break;
        }
    }
    return counts;
}

QString describeCounts(const LevelCounts& counts)
{
    QStringList parts;
    parts.append(QStringLiteral("%1 lines").arg(counts.total()));
    if (counts.error + counts.critical > 0) {
        parts.append(QStringLiteral("%1 error(s)").arg(counts.error + counts.critical));
    }
    if (counts.warning > 0) {
        parts.append(QStringLiteral("%1 warning(s)").arg(counts.warning));
    }
    return parts.join(QStringLiteral(" — "));
}

QString joinLines(const QList<LogLine>& lines)
{
    QStringList text;
    text.reserve(lines.size());
    for (const LogLine& line : lines) {
        text.append(line.text);
    }
    return text.join(QLatin1Char('\n'));
}

// ---------------------------------------------------------------------------
// Sniffing
// ---------------------------------------------------------------------------

BinaryKind sniffBytes(const QByteArray& head)
{
    const auto starts = [&head](const char* magic, int length) {
        return head.size() >= length && head.startsWith(QByteArray(magic, length));
    };
    if (starts("\x89PNG\r\n\x1a\n", 8)) {
        return BinaryKind::Png;
    }
    if (head.size() >= 3 && static_cast<quint8>(head.at(0)) == 0xFF
        && static_cast<quint8>(head.at(1)) == 0xD8) {
        return BinaryKind::Jpeg;
    }
    if (starts("GIF87a", 6) || starts("GIF89a", 6)) {
        return BinaryKind::Gif;
    }
    if (starts("BM", 2) && head.size() > 14) {
        return BinaryKind::Bmp;
    }
    if (starts("RIFF", 4) && head.mid(8, 4) == QByteArray("WEBP", 4)) {
        return BinaryKind::WebP;
    }
    if (starts("<?xml", 5) || starts("<svg", 4)) {
        // Only an SVG if the root element really is <svg>.
        if (head.contains("<svg") || head.contains("<SVG")) {
            return BinaryKind::Svg;
        }
    }
    if (head.size() >= 4 && static_cast<quint8>(head.at(0)) == 0
        && static_cast<quint8>(head.at(1)) == 0
        && static_cast<quint8>(head.at(2)) == 1
        && static_cast<quint8>(head.at(3)) == 0) {
        return BinaryKind::Ico;
    }
    if (starts("%PDF", 4)) {
        return BinaryKind::Pdf;
    }
    if (starts("PK\x03\x04", 4)) {
        return BinaryKind::Zip;
    }
    if (starts("\x7f" "ELF", 4)) {
        return BinaryKind::Elf;
    }
    if (starts("MZ", 2)) {
        return BinaryKind::Pe;
    }
    if (head.size() >= 4) {
        const quint32 magic = (static_cast<quint32>(static_cast<quint8>(head.at(0))) << 24)
                              | (static_cast<quint32>(static_cast<quint8>(head.at(1))) << 16)
                              | (static_cast<quint32>(static_cast<quint8>(head.at(2))) << 8)
                              | static_cast<quint32>(static_cast<quint8>(head.at(3)));
        if (magic == 0xFEEDFACEu || magic == 0xFEEDFACFu || magic == 0xCEFAEDFEu
            || magic == 0xCFFAEDFEu || magic == 0xCAFEBABEu) {
            return BinaryKind::MachO;
        }
    }
    // Text: no NUL in the sample and mostly printable.
    if (!head.isEmpty()) {
        int printable = 0;
        for (const char raw : head) {
            const auto byte = static_cast<unsigned char>(raw);
            if (byte == 0) {
                return BinaryKind::Unknown;
            }
            if (byte >= 0x20 || byte == '\n' || byte == '\r' || byte == '\t') {
                ++printable;
            }
        }
        if (printable * 100 >= head.size() * 95) {
            return BinaryKind::Text;
        }
    }
    return BinaryKind::Unknown;
}

QString binaryKindName(BinaryKind kind)
{
    switch (kind) {
    case BinaryKind::Png:    return QStringLiteral("PNG image");
    case BinaryKind::Jpeg:   return QStringLiteral("JPEG image");
    case BinaryKind::Gif:    return QStringLiteral("GIF image");
    case BinaryKind::Bmp:    return QStringLiteral("BMP image");
    case BinaryKind::WebP:   return QStringLiteral("WebP image");
    case BinaryKind::Svg:    return QStringLiteral("SVG image");
    case BinaryKind::Ico:    return QStringLiteral("Windows icon");
    case BinaryKind::Pdf:    return QStringLiteral("PDF document");
    case BinaryKind::Zip:    return QStringLiteral("ZIP archive");
    case BinaryKind::Elf:    return QStringLiteral("ELF executable");
    case BinaryKind::Pe:     return QStringLiteral("Windows executable");
    case BinaryKind::MachO:  return QStringLiteral("Mach-O executable");
    case BinaryKind::Text:   return QStringLiteral("text");
    case BinaryKind::Unknown: break;
    }
    return QStringLiteral("unknown binary");
}

bool isImageKind(BinaryKind kind)
{
    switch (kind) {
    case BinaryKind::Png:
    case BinaryKind::Jpeg:
    case BinaryKind::Gif:
    case BinaryKind::Bmp:
    case BinaryKind::WebP:
    case BinaryKind::Svg:
    case BinaryKind::Ico:
        return true;
    default:
        break;
    }
    return false;
}

}  // namespace LightCpp::Workbench::Viewer
