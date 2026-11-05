// MarkupRender — implementation. See the header for the QTextBrowser limits.

#include "LightCpp/Workbench/MarkupRender.hpp"

#include <QRegularExpression>
#include <QStringList>

namespace LightCpp::Workbench {
namespace {

QString escapeHtml(const QString& text)
{
    QString out;
    out.reserve(text.size() + 16);
    for (const QChar ch : text) {
        switch (ch.unicode()) {
        case '&':  out += QStringLiteral("&amp;"); break;
        case '<':  out += QStringLiteral("&lt;"); break;
        case '>':  out += QStringLiteral("&gt;"); break;
        case '"':  out += QStringLiteral("&quot;"); break;
        default:   out.append(ch); break;
        }
    }
    return out;
}

bool isBlank(const QString& line)
{
    return line.trimmed().isEmpty();
}

/// ``` and ~~~ fences, with an optional language.
struct Fence {
    bool open = false;
    QChar marker;
    int length = 0;
    QString language;
};

bool fenceStart(const QString& line, Fence& fence)
{
    const QString trimmed = line.trimmed();
    if (trimmed.size() < 3) {
        return false;
    }
    const QChar marker = trimmed.at(0);
    if (marker != QLatin1Char('`') && marker != QLatin1Char('~')) {
        return false;
    }
    int count = 0;
    while (count < trimmed.size() && trimmed.at(count) == marker) {
        ++count;
    }
    if (count < 3) {
        return false;
    }
    fence.open = true;
    fence.marker = marker;
    fence.length = count;
    fence.language = trimmed.mid(count).trimmed();
    return true;
}

bool fenceEnd(const QString& line, const Fence& fence)
{
    const QString trimmed = line.trimmed();
    int count = 0;
    while (count < trimmed.size() && trimmed.at(count) == fence.marker) {
        ++count;
    }
    return count >= fence.length && trimmed.mid(count).trimmed().isEmpty();
}

/// Inline Markdown: `code`, **bold**, *italic*, ~~strike~~, [text](url),
/// ![alt](src), <autolink>. The input is already escaped, so the patterns
/// operate on escaped text and restore the intended tags.
QString renderInline(const QString& escaped)
{
    QString text = escaped;

    // Code spans first: their contents must not be interpreted further, so
    // they are swapped for placeholders and restored at the end.
    QStringList codes;
    static const QRegularExpression codeSpan(QStringLiteral("`([^`]+)`"));
    QString withPlaceholders;
    int last = 0;
    auto it = codeSpan.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        withPlaceholders += text.mid(last, match.capturedStart() - last);
        withPlaceholders += QStringLiteral("\x01%1\x02").arg(codes.size());
        codes << QStringLiteral("<code>%1</code>").arg(match.captured(1));
        last = match.capturedEnd();
    }
    withPlaceholders += text.mid(last);
    text = withPlaceholders;

    static const QRegularExpression image(
        QStringLiteral("!\\[([^\\]]*)\\]\\(([^)\\s]+)(?:\\s+\"([^\"]*)\")?\\)"));
    text.replace(image, QStringLiteral("<img src=\"\\2\" alt=\"\\1\">"));

    static const QRegularExpression link(
        QStringLiteral("\\[([^\\]]+)\\]\\(([^)\\s]+)(?:\\s+\"([^\"]*)\")?\\)"));
    text.replace(link, QStringLiteral("<a href=\"\\2\">\\1</a>"));

    static const QRegularExpression autolink(
        QStringLiteral("&lt;(https?://[^&\\s]+)&gt;"));
    text.replace(autolink, QStringLiteral("<a href=\"\\1\">\\1</a>"));

    static const QRegularExpression bold(
        QStringLiteral("\\*\\*([^*]+)\\*\\*"));
    text.replace(bold, QStringLiteral("<b>\\1</b>"));
    static const QRegularExpression boldUnderscore(
        QStringLiteral("__([^_]+)__"));
    text.replace(boldUnderscore, QStringLiteral("<b>\\1</b>"));
    static const QRegularExpression strike(
        QStringLiteral("~~([^~]+)~~"));
    text.replace(strike, QStringLiteral("<s>\\1</s>"));
    static const QRegularExpression italic(
        QStringLiteral("(?<!\\*)\\*([^*\\s][^*]*)\\*(?!\\*)"));
    text.replace(italic, QStringLiteral("<i>\\1</i>"));

    for (int i = 0; i < codes.size(); ++i) {
        text.replace(QStringLiteral("\x01%1\x02").arg(i), codes.at(i));
    }
    return text;
}

int headingLevel(const QString& line, QString& body)
{
    int level = 0;
    while (level < line.size() && line.at(level) == QLatin1Char('#')) {
        ++level;
    }
    if (level == 0 || level > 6) {
        return 0;
    }
    if (line.size() > level && !line.at(level).isSpace()) {
        return 0;
    }
    body = line.mid(level).trimmed();
    return level;
}

bool isHorizontalRule(const QString& line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.size() < 3) {
        return false;
    }
    const QChar first = trimmed.at(0);
    if (first != QLatin1Char('-') && first != QLatin1Char('*') && first != QLatin1Char('_')) {
        return false;
    }
    int count = 0;
    for (const QChar ch : trimmed) {
        if (ch == first) {
            ++count;
        } else if (!ch.isSpace()) {
            return false;
        }
    }
    return count >= 3;
}

/// "| a | b |" plus its "|---|---|" separator → table rows.
bool isTableRow(const QString& line)
{
    const QString trimmed = line.trimmed();
    return trimmed.startsWith(QLatin1Char('|')) && trimmed.endsWith(QLatin1Char('|'));
}

bool isTableSeparator(const QString& line)
{
    const QString trimmed = line.trimmed();
    if (!isTableRow(trimmed)) {
        return false;
    }
    for (const QChar ch : trimmed) {
        if (ch != QLatin1Char('|') && ch != QLatin1Char('-')
            && ch != QLatin1Char(':') && !ch.isSpace()) {
            return false;
        }
    }
    return true;
}

QStringList splitRow(const QString& line)
{
    QString trimmed = line.trimmed();
    trimmed = trimmed.mid(1, trimmed.size() - 2);
    QStringList cells;
    for (const QString& cell : trimmed.split(QLatin1Char('|'))) {
        cells << cell.trimmed();
    }
    return cells;
}

struct ListItem {
    int indent = 0;
    bool ordered = false;
    int number = 1;
    QString text;
};

bool parseListItem(const QString& line, ListItem& item)
{
    int indent = 0;
    while (indent < line.size() && (line.at(indent) == QLatin1Char(' ')
                                    || line.at(indent) == QLatin1Char('\t'))) {
        indent += (line.at(indent) == QLatin1Char('\t') ? 4 : 1);
    }
    const QString rest = line.mid(indent);
    if (rest.size() > 1
        && (rest.at(0) == QLatin1Char('-') || rest.at(0) == QLatin1Char('*')
            || rest.at(0) == QLatin1Char('+'))
        && rest.at(1).isSpace()) {
        item.indent = indent;
        item.ordered = false;
        item.text = rest.mid(2).trimmed();
        return true;
    }
    static const QRegularExpression ordered(
        QStringLiteral("^(\\d{1,9})[.)]\\s+(.*)$"));
    if (const auto match = ordered.match(rest); match.hasMatch()) {
        item.indent = indent;
        item.ordered = true;
        item.number = match.captured(1).toInt();
        item.text = match.captured(2).trimmed();
        return true;
    }
    return false;
}

}  // namespace

QString renderMarkdown(const QString& markdown)
{
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    QString html;
    Fence fence;
    bool inParagraph = false;
    bool inCode = false;
    QStringList codeLines;
    QString codeLanguage;

    const auto closeParagraph = [&html, &inParagraph] {
        if (inParagraph) {
            html += QStringLiteral("</p>\n");
            inParagraph = false;
        }
    };

    // Open list state: a stack of (indent, ordered).
    QList<QPair<int, bool>> listStack;

    const auto closeLists = [&html, &listStack] {
        while (!listStack.isEmpty()) {
            html += listStack.last().second ? QStringLiteral("</ol>\n")
                                            : QStringLiteral("</ul>\n");
            listStack.removeLast();
        }
    };

    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString& raw = lines.at(i);
        const QString line = raw;

        if (inCode) {
            if (fenceEnd(line, fence)) {
                QString body = codeLines.join(QLatin1Char('\n'));
                html += QStringLiteral("<pre><code");
                if (!codeLanguage.isEmpty()) {
                    html += QStringLiteral(" class=\"language-%1\"").arg(escapeHtml(codeLanguage));
                }
                html += QStringLiteral(">%1</code></pre>\n").arg(escapeHtml(body));
                codeLines.clear();
                inCode = false;
                fence = Fence{};
            } else {
                codeLines << line;
            }
            continue;
        }

        if (fenceStart(line, fence)) {
            closeParagraph();
            closeLists();
            inCode = true;
            codeLanguage = fence.language;
            codeLines.clear();
            continue;
        }

        if (isBlank(line)) {
            closeParagraph();
            // A blank line does not close a list: Markdown keeps it open, and
            // QTextBrowser renders the tighter list either way.
            continue;
        }

        // Setext headings: "Title" followed by === or ---.
        if (i + 1 < lines.size() && !isBlank(line)) {
            const QString underline = lines.at(i + 1).trimmed();
            if (!underline.isEmpty()
                && (underline.count(QLatin1Char('=')) == underline.size()
                    || underline.count(QLatin1Char('-')) == underline.size())
                && underline.size() >= 3
                && !isHorizontalRule(line)) {
                closeParagraph();
                closeLists();
                const int level = underline.at(0) == QLatin1Char('=') ? 1 : 2;
                html += QStringLiteral("<h%1>%2</h%1>\n")
                            .arg(level)
                            .arg(renderInline(escapeHtml(line.trimmed())));
                ++i;
                continue;
            }
        }

        QString headingBody;
        if (const int level = headingLevel(line, headingBody); level > 0) {
            closeParagraph();
            closeLists();
            html += QStringLiteral("<h%1>%2</h%1>\n")
                        .arg(level)
                        .arg(renderInline(escapeHtml(headingBody)));
            continue;
        }

        if (isHorizontalRule(line)) {
            closeParagraph();
            closeLists();
            html += QStringLiteral("<hr>\n");
            continue;
        }

        if (isTableRow(line) && i + 1 < lines.size() && isTableSeparator(lines.at(i + 1))) {
            closeParagraph();
            closeLists();
            const QStringList headers = splitRow(line);
            html += QStringLiteral("<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\">\n<tr>");
            for (const QString& cell : headers) {
                html += QStringLiteral("<th>%1</th>").arg(renderInline(escapeHtml(cell)));
            }
            html += QStringLiteral("</tr>\n");
            qsizetype j = i + 2;
            for (; j < lines.size() && isTableRow(lines.at(j)); ++j) {
                if (isTableSeparator(lines.at(j))) {
                    continue;
                }
                html += QStringLiteral("<tr>");
                for (const QString& cell : splitRow(lines.at(j))) {
                    html += QStringLiteral("<td>%1</td>").arg(renderInline(escapeHtml(cell)));
                }
                html += QStringLiteral("</tr>\n");
            }
            html += QStringLiteral("</table>\n");
            i = j - 1;
            continue;
        }

        if (line.startsWith(QLatin1Char('>'))) {
            closeParagraph();
            closeLists();
            QString quoted = line.mid(1);
            while (quoted.startsWith(QLatin1Char(' '))) {
                quoted = quoted.mid(1);
            }
            html += QStringLiteral("<blockquote><p>%1</p></blockquote>\n")
                        .arg(renderInline(escapeHtml(quoted)));
            continue;
        }

        ListItem item;
        if (parseListItem(line, item)) {
            closeParagraph();
            // Pop lists that this item is not inside.
            while (!listStack.isEmpty() && item.indent < listStack.last().first) {
                html += listStack.last().second ? QStringLiteral("</ol>\n")
                                                : QStringLiteral("</ul>\n");
                listStack.removeLast();
            }
            if (!listStack.isEmpty() && item.indent == listStack.last().first
                && listStack.last().second == item.ordered) {
                // Same list, another item.
            } else {
                html += item.ordered ? QStringLiteral("<ol>\n") : QStringLiteral("<ul>\n");
                listStack.append({item.indent, item.ordered});
            }
            html += QStringLiteral("<li>%1</li>\n").arg(renderInline(escapeHtml(item.text)));
            continue;
        }

        // Continuation of a list item: append to the last <li>.
        if (!listStack.isEmpty() && line.startsWith(QLatin1Char(' '))
            && !inParagraph) {
            html.chop(6);  // drop "</li>\n"
            html += QStringLiteral(" %1</li>\n").arg(renderInline(escapeHtml(line.trimmed())));
            continue;
        }

        closeLists();
        if (!inParagraph) {
            html += QStringLiteral("<p>");
            inParagraph = true;
        } else {
            html += QStringLiteral("<br>");
        }
        html += renderInline(escapeHtml(line));
    }

    if (inCode) {
        html += QStringLiteral("<pre><code>%1</code></pre>\n")
                    .arg(escapeHtml(codeLines.join(QLatin1Char('\n'))));
    }
    closeParagraph();
    closeLists();
    return html;
}

QString renderRestructuredText(const QString& rst)
{
    const QStringList lines = rst.split(QLatin1Char('\n'));
    QString html;
    bool inParagraph = false;
    bool inLiteral = false;
    bool inCode = false;
    QString codeLanguage;
    QStringList codeLines;

    const auto closeParagraph = [&html, &inParagraph] {
        if (inParagraph) {
            html += QStringLiteral("</p>\n");
            inParagraph = false;
        }
    };
    const auto flushCode = [&] {
        if (!codeLines.isEmpty() || inCode) {
            html += QStringLiteral("<pre><code");
            if (!codeLanguage.isEmpty()) {
                html += QStringLiteral(" class=\"language-%1\"").arg(escapeHtml(codeLanguage));
            }
            html += QStringLiteral(">%1</code></pre>\n")
                        .arg(escapeHtml(codeLines.join(QLatin1Char('\n'))));
            codeLines.clear();
        }
        inCode = false;
        codeLanguage.clear();
    };

    // RST inline: ``literal``, **bold**, *italic*, `link <url>`_.
    const auto renderRstInline = [](const QString& escaped) {
        QString text = escaped;
        static const QRegularExpression literal(QStringLiteral("``([^`]+)``"));
        text.replace(literal, QStringLiteral("<code>\\1</code>"));
        static const QRegularExpression bold(QStringLiteral("\\*\\*([^*]+)\\*\\*"));
        text.replace(bold, QStringLiteral("<b>\\1</b>"));
        static const QRegularExpression italic(QStringLiteral("\\*([^*]+)\\*"));
        text.replace(italic, QStringLiteral("<i>\\1</i>"));
        static const QRegularExpression link(
            QStringLiteral("`([^`<]+)\\s*&lt;([^&]+)&gt;`_"));
        text.replace(link, QStringLiteral("<a href=\"\\2\">\\1</a>"));
        return text;
    };

    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString& line = lines.at(i);

        if (inCode) {
            if (isBlank(line)) {
                // A blank line inside a code-block only ends it when the next
                // line is not indented; keep it simple and treat it as the end.
                flushCode();
                continue;
            }
            if (line.startsWith(QLatin1Char(' ')) || line.startsWith(QLatin1Char('\t'))) {
                codeLines << line.trimmed();
                continue;
            }
            flushCode();
            // fall through to normal handling of this line
        }

        if (isBlank(line)) {
            closeParagraph();
            inLiteral = inLiteral && false;
            continue;
        }

        // Directive: ".. code-block:: cpp" / ".. code:: cpp"
        if (line.trimmed().startsWith(QLatin1String(".. code"))) {
            closeParagraph();
            const QString trimmed = line.trimmed();
            const qsizetype colons = trimmed.lastIndexOf(QLatin1String("::"));
            codeLanguage = colons >= 0 ? trimmed.mid(colons + 2).trimmed() : QString();
            inCode = true;
            codeLines.clear();
            continue;
        }
        // Any other directive (".. note::", ".. image::") is kept as a note.
        if (line.trimmed().startsWith(QLatin1String(".. "))) {
            closeParagraph();
            const QString trimmed = line.trimmed();
            const qsizetype colons = trimmed.indexOf(QLatin1String("::"));
            if (colons > 3) {
                const QString kind = trimmed.mid(3, colons - 3).trimmed();
                html += QStringLiteral("<p><b>%1:</b> ")
                            .arg(escapeHtml(kind));
                // The directive body is the indented block that follows.
                QStringList body;
                qsizetype j = i + 1;
                for (; j < lines.size(); ++j) {
                    if (lines.at(j).trimmed().isEmpty()) {
                        break;
                    }
                    if (!lines.at(j).startsWith(QLatin1Char(' '))) {
                        break;
                    }
                    body << lines.at(j).trimmed();
                }
                html += renderRstInline(escapeHtml(body.join(QLatin1Char(' '))))
                        + QStringLiteral("</p>\n");
                i = j;
                continue;
            }
            continue;
        }

        // Section title: a line followed by an underline of punctuation.
        if (i + 1 < lines.size() && !isBlank(lines.at(i + 1))) {
            const QString underline = lines.at(i + 1).trimmed();
            const QChar mark = underline.isEmpty() ? QChar() : underline.at(0);
            // RST allows any non-alphanumeric character as an underline;
            // QChar::isPunct() excludes '=' and '-', which are symbols, so
            // the test is "not a letter, digit or space".
            const bool punctuation = !mark.isLetterOrNumber() && !mark.isSpace();
            bool allSame = !underline.isEmpty();
            for (const QChar ch : underline) {
                allSame = allSame && ch == mark;
            }
            if (punctuation && allSame && underline.size() >= line.trimmed().size()
                && underline.size() >= 3) {
                closeParagraph();
                // The character used decides the level in RST; this renderer
                // maps the first six characters it sees to h2..h6 in order.
                static QString seen;
                if (!seen.contains(mark)) {
                    seen.append(mark);
                }
                const int level = std::min(6, 2 + static_cast<int>(seen.indexOf(mark)));
                html += QStringLiteral("<h%1>%2</h%1>\n")
                            .arg(level)
                            .arg(renderRstInline(escapeHtml(line.trimmed())));
                ++i;
                continue;
            }
        }

        // Bullet list.
        static const QRegularExpression bullet(
            QStringLiteral("^([-*+])\\s+(.*)$"));
        if (const auto match = bullet.match(line.trimmed()); match.hasMatch()) {
            closeParagraph();
            html += QStringLiteral("<ul><li>%1</li></ul>\n")
                        .arg(renderRstInline(escapeHtml(match.captured(2))));
            continue;
        }
        // Enumerated list: "1. text" / "#. text" / "a) text".
        static const QRegularExpression enumerated(
            QStringLiteral("^(\\d+|[#]|[a-zA-Z])[.)]\\s+(.*)$"));
        if (const auto match = enumerated.match(line.trimmed()); match.hasMatch()) {
            closeParagraph();
            html += QStringLiteral("<ol><li>%1</li></ol>\n")
                        .arg(renderRstInline(escapeHtml(match.captured(2))));
            continue;
        }

        // Field list: ":Author: name".
        static const QRegularExpression field(
            QStringLiteral("^:([^:]+):\\s*(.*)$"));
        if (const auto match = field.match(line.trimmed()); match.hasMatch()) {
            closeParagraph();
            html += QStringLiteral("<p><b>%1</b> %2</p>\n")
                        .arg(escapeHtml(match.captured(1)),
                             renderRstInline(escapeHtml(match.captured(2))));
            continue;
        }

        // A paragraph line ending in "::" introduces a literal block.
        if (line.trimmed().endsWith(QLatin1String("::"))) {
            closeParagraph();
            QString text = line.trimmed();
            text.chop(2);
            if (!text.isEmpty()) {
                html += QStringLiteral("<p>%1</p>\n").arg(renderRstInline(escapeHtml(text)));
            }
            inLiteral = true;
            // The following indented block is the literal text.
            QStringList block;
            qsizetype j = i + 1;
            for (; j < lines.size(); ++j) {
                if (!lines.at(j).startsWith(QLatin1Char(' ')) && !lines.at(j).trimmed().isEmpty()) {
                    break;
                }
                block << lines.at(j).trimmed();
            }
            while (!block.isEmpty() && block.last().isEmpty()) {
                block.removeLast();
            }
            html += QStringLiteral("<pre>%1</pre>\n").arg(escapeHtml(block.join(QLatin1Char('\n'))));
            i = j - 1;
            inLiteral = false;
            continue;
        }

        if (!inParagraph) {
            html += QStringLiteral("<p>");
            inParagraph = true;
        } else {
            html += QLatin1Char(' ');
        }
        html += renderRstInline(escapeHtml(line.trimmed()));
    }
    flushCode();
    closeParagraph();
    return html;
}

QString renderForFile(const QString& path, const QString& text)
{
    const QString lower = path.toLower();
    if (lower.endsWith(QLatin1String(".md")) || lower.endsWith(QLatin1String(".markdown"))
        || lower.endsWith(QLatin1String(".mdown"))) {
        return renderMarkdown(text);
    }
    if (lower.endsWith(QLatin1String(".rst")) || lower.endsWith(QLatin1String(".rest"))) {
        return renderRestructuredText(text);
    }
    return QStringLiteral("<pre>%1</pre>").arg(escapeHtml(text));
}

QString previewStyleSheet(bool darkBackground)
{
    const QString background = darkBackground ? QStringLiteral("#1e1e1e")
                                              : QStringLiteral("#ffffff");
    const QString foreground = darkBackground ? QStringLiteral("#d4d4d4")
                                              : QStringLiteral("#1f1f1f");
    const QString codeBackground = darkBackground ? QStringLiteral("#2d2d2d")
                                                  : QStringLiteral("#f4f4f4");
    const QString border = darkBackground ? QStringLiteral("#3c3c3c")
                                          : QStringLiteral("#d8d8d8");
    const QString link = darkBackground ? QStringLiteral("#4da3ff")
                                        : QStringLiteral("#0b5cad");
    return QStringLiteral(
               "body { background: %1; color: %2; font-size: 11pt; }"
               "h1, h2, h3, h4 { color: %2; margin-top: 14px; margin-bottom: 6px; }"
               "h1 { font-size: 18pt; } h2 { font-size: 15pt; } h3 { font-size: 13pt; }"
               "p { margin: 6px 0; line-height: 140%; }"
               "pre { background: %3; border: 1px solid %4; padding: 8px; }"
               "code { background: %3; font-family: monospace; }"
               "blockquote { border-left: 3px solid %4; margin: 8px 0; padding-left: 10px; }"
               "table { border-collapse: collapse; }"
               "th, td { border: 1px solid %4; padding: 4px 8px; }"
               "a { color: %5; }"
               "hr { border: none; border-top: 1px solid %4; }")
        .arg(background, foreground, codeBackground, border, link);
}

// ---------------------------------------------------------------------------
// Doxygen
// ---------------------------------------------------------------------------

bool looksLikeDoxygen(const QString& text)
{
    const QString trimmed = text.trimmed();
    return trimmed.startsWith(QLatin1String("///"))
           || trimmed.startsWith(QLatin1String("/**"))
           || trimmed.startsWith(QLatin1String("//!"))
           || trimmed.contains(QLatin1String("\\brief"))
           || trimmed.contains(QLatin1String("@brief"))
           || trimmed.contains(QLatin1String("\\param"))
           || trimmed.contains(QLatin1String("@param"));
}

namespace {

/// Strip the comment decoration: leading "///" / " * " / "//!" and the
/// closing "*/".
QString cleanCommentLine(QString line)
{
    line = line.trimmed();
    if (line.startsWith(QLatin1String("///")) || line.startsWith(QLatin1String("//!"))) {
        line = line.mid(3);
    } else if (line.startsWith(QLatin1String("/**")) || line.startsWith(QLatin1String("/*!"))) {
        line = line.mid(3);
    } else if (line.startsWith(QLatin1Char('*'))) {
        line = line.mid(1);
    }
    if (line.endsWith(QLatin1String("*/"))) {
        line.chop(2);
    }
    return line.trimmed();
}

}  // namespace

DoxygenComment parseDoxygenAbove(const QStringList& lines, int lineNumber)
{
    DoxygenComment comment;
    if (lines.isEmpty()) {
        return comment;
    }
    int index = lineNumber < 0 ? static_cast<int>(lines.size()) - 1
                               : std::min<int>(lineNumber - 1, static_cast<int>(lines.size()) - 1);
    // Walk up over blank lines, then collect the comment block.
    while (index >= 0 && lines.at(index).trimmed().isEmpty()) {
        --index;
    }
    QStringList block;
    bool closed = false;
    for (; index >= 0; --index) {
        const QString trimmed = lines.at(index).trimmed();
        const bool isComment = trimmed.startsWith(QLatin1String("///"))
                               || trimmed.startsWith(QLatin1String("//!"))
                               || trimmed.startsWith(QLatin1String("*"))
                               || trimmed.startsWith(QLatin1String("/*"))
                               || trimmed.startsWith(QLatin1String("/**"));
        if (!isComment) {
            break;
        }
        block.prepend(trimmed);
        if (trimmed.startsWith(QLatin1String("/*"))) {
            closed = true;
            break;
        }
    }
    if (block.isEmpty()) {
        return comment;
    }
    if (!closed && !block.first().startsWith(QLatin1String("///"))
        && !block.first().startsWith(QLatin1String("//!"))) {
        return comment;  // a stray "*" line, not a comment block
    }

    QString pendingText;
    const auto flushText = [&comment, &pendingText] {
        if (pendingText.trimmed().isEmpty()) {
            pendingText.clear();
            return;
        }
        if (comment.brief.isEmpty()) {
            comment.brief = pendingText.trimmed();
        } else if (comment.details.isEmpty()) {
            comment.details = pendingText.trimmed();
        } else {
            comment.details += QStringLiteral("\n") + pendingText.trimmed();
        }
        pendingText.clear();
    };

    for (const QString& raw : block) {
        QString line = cleanCommentLine(raw);
        if (line.isEmpty()) {
            flushText();
            continue;
        }
        if (!line.startsWith(QLatin1Char('\\')) && !line.startsWith(QLatin1Char('@'))) {
            pendingText += (pendingText.isEmpty() ? QString() : QStringLiteral(" ")) + line;
            continue;
        }
        flushText();
        const QChar marker = line.at(0);
        line = line.mid(1);
        QString command;
        int i = 0;
        while (i < line.size() && line.at(i).isLetter()) {
            ++i;
        }
        command = line.left(i).toLower();
        QString body = line.mid(i).trimmed();
        Q_UNUSED(marker);
        if (command == QLatin1String("brief") || command == QLatin1String("short")) {
            comment.brief = body;
        } else if (command == QLatin1String("details") || command == QLatin1String("long")) {
            comment.details = body;
        } else if (command == QLatin1String("param") || command == QLatin1String("arg")) {
            // "\param in name  description" / "\param name description"
            const QStringList parts = body.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (parts.isEmpty()) {
                continue;
            }
            int offset = 0;
            if (parts.size() > 1 && (parts.at(0) == QLatin1String("in")
                                     || parts.at(0) == QLatin1String("out")
                                     || parts.at(0) == QLatin1String("inout"))) {
                offset = 1;
            }
            const QString name = parts.value(offset);
            const QString description = body.mid(body.indexOf(name) + name.size()).trimmed();
            comment.params.append({name, description});
        } else if (command == QLatin1String("return") || command == QLatin1String("returns")
                   || command == QLatin1String("retval")) {
            comment.returns = body;
        } else if (command == QLatin1String("note")) {
            comment.notes << body;
        } else if (command == QLatin1String("warning") || command == QLatin1String("attention")) {
            comment.warnings << body;
        } else if (command == QLatin1String("see") || command == QLatin1String("sa")) {
            comment.seeAlso << body;
        } else if (command == QLatin1String("deprecated")) {
            comment.deprecated = body;
        } else if (command == QLatin1String("author")) {
            comment.authors << body;
        } else {
            // Unknown commands are kept in the details rather than dropped:
            // losing a \todo would be worse than showing it as prose.
            pendingText += (pendingText.isEmpty() ? QString() : QStringLiteral(" ")) + body;
        }
    }
    flushText();
    comment.valid = true;
    return comment;
}

QString doxygenToHtml(const DoxygenComment& comment)
{
    if (!comment.valid) {
        return {};
    }
    QString html;
    if (!comment.brief.isEmpty()) {
        html += QStringLiteral("<p><b>%1</b></p>").arg(escapeHtml(comment.brief));
    }
    if (!comment.details.isEmpty()) {
        html += QStringLiteral("<p>%1</p>").arg(escapeHtml(comment.details));
    }
    if (!comment.params.isEmpty()) {
        html += QStringLiteral("<p><b>Parameters</b></p><ul>");
        for (const auto& param : comment.params) {
            html += QStringLiteral("<li><code>%1</code> — %2</li>")
                        .arg(escapeHtml(param.first), escapeHtml(param.second));
        }
        html += QStringLiteral("</ul>");
    }
    if (!comment.returns.isEmpty()) {
        html += QStringLiteral("<p><b>Returns</b> %1</p>").arg(escapeHtml(comment.returns));
    }
    for (const QString& note : comment.notes) {
        html += QStringLiteral("<p><b>Note</b> %1</p>").arg(escapeHtml(note));
    }
    for (const QString& warning : comment.warnings) {
        html += QStringLiteral("<p><b>Warning</b> %1</p>").arg(escapeHtml(warning));
    }
    if (!comment.deprecated.isEmpty()) {
        html += QStringLiteral("<p><b>Deprecated</b> %1</p>").arg(escapeHtml(comment.deprecated));
    }
    if (!comment.seeAlso.isEmpty()) {
        html += QStringLiteral("<p><b>See also</b> %1</p>")
                    .arg(escapeHtml(comment.seeAlso.join(QStringLiteral(", "))));
    }
    return html;
}

}  // namespace LightCpp::Workbench
