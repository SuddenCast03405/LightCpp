// CppSyntaxHighlighter — basic C/C++ syntax highlighting.

#include "LightCpp/App/CppSyntaxHighlighter.hpp"

#include <algorithm>

#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QTextDocument>

#include "LightCpp/App/CppTokenScan.hpp"

namespace CppTokenScan = LightCpp::App::CppTokenScan;

namespace {

/// QSyntaxHighlighter carries one integer between blocks. It means "this block
/// ends inside a /* ... */ comment".
constexpr int kBlockCommentState = 1;

/// Build "\\b(a|b|c)\\b" with the alternatives longest-first, so a prefix
/// ("int" vs "int8_t") is never shadowed by a shorter alternative that would
/// only match because the trailing \\b had not been tried yet.
///
/// The keyword and type lists used to be one QRegularExpression each (~110 in
/// total), all run over every block. That is what made pasting a large block
/// — or typing in a long file — visibly stutter: the cost grew with the
/// NUMBER OF WORDS rather than with the text. One combined pattern per class
/// turns that into two passes, and the captured word selects the colour.
QString alternationOf(QStringList words)
{
    std::sort(words.begin(), words.end(),
              [](const QString& a, const QString& b) {
                  if (a.size() != b.size()) {
                      return a.size() > b.size();
                  }
                  return a < b;
              });
    QStringList escaped;
    escaped.reserve(words.size());
    for (const QString& word : words) {
        escaped.append(QRegularExpression::escape(word));
    }
    return QStringLiteral("\\b(?:%1)\\b").arg(escaped.join(QLatin1Char('|')));
}

}  // namespace

CppSyntaxHighlighter::CppSyntaxHighlighter(QTextDocument* parent)
    : QSyntaxHighlighter(parent)
{
    // --- Formats (dark theme) ---
    keywordFormat_.setForeground(QColor(0xCF, 0x6A, 0x6A));  // warm red
    keywordFormat_.setFontWeight(QFont::Bold);

    typeFormat_.setForeground(QColor(0x56, 0x9C, 0xD6));     // blue
    typeFormat_.setFontWeight(QFont::Bold);

    stringFormat_.setForeground(QColor(0x6A, 0x8B, 0x5B));    // green

    commentFormat_.setForeground(QColor(0x80, 0x80, 0x80));   // grey
    commentFormat_.setFontItalic(true);

    preprocFormat_.setForeground(QColor(0xC6, 0x86, 0x00));   // gold
    preprocFormat_.setFontWeight(QFont::Bold);

    numberFormat_.setForeground(QColor(0x8F, 0x5C, 0xD6));    // purple

    funcFormat_.setForeground(QColor(0xDC, 0xD8, 0x4A));      // yellow

    // --- Rules ---
    // Order is significant: a later setFormat() wins on an overlapping range,
    // so a preprocessor line is coloured first and the word classes paint over
    // it, exactly as when each word had a pattern of its own.
    Rule r;

    // Preprocessor (must come before keywords to match #include etc.)
    r.pattern = QRegularExpression(QStringLiteral(R"(^\s*#\s*\w+)"));
    r.format = preprocFormat_;
    rules_.append(r);

    // Keywords — one pass for the whole class.
    const QStringList keywords = {
        QStringLiteral("alignas"), QStringLiteral("alignof"), QStringLiteral("auto"),
        QStringLiteral("break"), QStringLiteral("case"), QStringLiteral("catch"),
        QStringLiteral("class"), QStringLiteral("concept"), QStringLiteral("const"),
        QStringLiteral("constexpr"), QStringLiteral("continue"), QStringLiteral("co_await"),
        QStringLiteral("co_return"), QStringLiteral("co_yield"), QStringLiteral("decltype"),
        QStringLiteral("default"), QStringLiteral("delete"), QStringLiteral("do"),
        QStringLiteral("else"), QStringLiteral("enum"), QStringLiteral("explicit"),
        QStringLiteral("export"), QStringLiteral("extern"), QStringLiteral("for"),
        QStringLiteral("friend"), QStringLiteral("goto"), QStringLiteral("if"),
        QStringLiteral("inline"), QStringLiteral("mutable"), QStringLiteral("namespace"),
        QStringLiteral("new"), QStringLiteral("noexcept"), QStringLiteral("operator"),
        QStringLiteral("private"), QStringLiteral("protected"), QStringLiteral("public"),
        QStringLiteral("register"), QStringLiteral("requires"), QStringLiteral("return"),
        QStringLiteral("sizeof"), QStringLiteral("static"), QStringLiteral("static_assert"),
        QStringLiteral("struct"), QStringLiteral("switch"), QStringLiteral("template"),
        QStringLiteral("this"), QStringLiteral("throw"), QStringLiteral("try"),
        QStringLiteral("typedef"), QStringLiteral("typename"), QStringLiteral("using"),
        QStringLiteral("virtual"), QStringLiteral("while"),
        // C keywords
        QStringLiteral("restrict"),
        // Literals share the keyword colour and the keyword pass.
        QStringLiteral("true"), QStringLiteral("false"), QStringLiteral("nullptr"),
    };

    // Built-in types
    const QStringList types = {
        QStringLiteral("bool"), QStringLiteral("char"), QStringLiteral("char8_t"),
        QStringLiteral("char16_t"), QStringLiteral("char32_t"), QStringLiteral("double"),
        QStringLiteral("float"), QStringLiteral("int"), QStringLiteral("long"),
        QStringLiteral("short"), QStringLiteral("signed"), QStringLiteral("unsigned"),
        QStringLiteral("void"), QStringLiteral("wchar_t"),
        QStringLiteral("size_t"), QStringLiteral("ptrdiff_t"), QStringLiteral("int8_t"),
        QStringLiteral("int16_t"), QStringLiteral("int32_t"), QStringLiteral("int64_t"),
        QStringLiteral("uint8_t"), QStringLiteral("uint16_t"), QStringLiteral("uint32_t"),
        QStringLiteral("uint64_t"), QStringLiteral("nullptr_t"), QStringLiteral("uint"),
        QStringLiteral("int8"), QStringLiteral("int16"), QStringLiteral("int32"),
        QStringLiteral("int64"), QStringLiteral("QString"), QStringLiteral("QByteArray"),
        QStringLiteral("QList"), QStringLiteral("QVector"), QStringLiteral("QHash"),
        QStringLiteral("QObject"), QStringLiteral("QWidget"), QStringLiteral("QMainWindow"),
    };

    wordClasses_.append(
        WordClass{QRegularExpression(alternationOf(keywords)), {}});
    for (const QString& word : keywords) {
        wordClasses_.last().formats.insert(word, keywordFormat_);
    }
    wordClasses_.append(
        WordClass{QRegularExpression(alternationOf(types)), {}});
    for (const QString& word : types) {
        wordClasses_.last().formats.insert(word, typeFormat_);
    }

    // Numbers (hex, binary, float, int).
    // The alternation must sit INSIDE the \b(...)\b group: an extra ")"
    // after the exponent group used to leave the whole pattern invalid
    // ("unmatched closing parenthesis"), so QSyntaxHighlighter silently
    // skipped it and no number was ever highlighted.
    r.pattern = QRegularExpression(
        QStringLiteral(R"(\b(0[xX][0-9a-fA-F']+|0[bB][01']+|[0-9']*\.[0-9']*([eE][+-]?[0-9']+)?|([0-9']+[eE][+-]?[0-9']+)|[0-9']+[uUlL]{0,3})\b)"));
    r.format = numberFormat_;
    rules_.append(r);

    // Function calls: identifier followed by (
    r.pattern = QRegularExpression(QStringLiteral(R"(\b[A-Za-z_][A-Za-z0-9_]*(?=\s*\())"));
    r.format = funcFormat_;
    rules_.append(r);

    // String and char literals are no longer rules: deciding where a literal
    // ends (and whether the `/*` or `//` further along the line is really
    // code) needs one scanner that sees the whole line in order. See
    // CppTokenScan.hpp — the scan also tells the passes above which ranges to
    // leave alone, which is what stops a keyword inside a literal from
    // leaving its bold weight behind under the green.

    // TODO / FIXME / XXX / HACK markers. Applied separately at the END of
    // highlightBlock (after the comment and literal spans, which would
    // otherwise paint right over them) so they stay orange inside comments.
    // \b on both sides keeps it a marker token, not part of a word.
    todoRule_ = QRegularExpression(
        QStringLiteral(R"(\b(TODO|FIXME|XXX|HACK)\b)"));
    todoFormat_.setForeground(QColor(0xC8, 0x64, 0x00));  // dark orange
    QFont todoFont;
    todoFont.setBold(true);
    todoFormat_.setFont(todoFont);
}

CppSyntaxHighlighter::~CppSyntaxHighlighter() = default;

void CppSyntaxHighlighter::highlightBlock(const QString& text)
{
    // One scan answers "what is a comment, what is a literal" for the whole
    // block; every pass below asks it before painting, so no pass can paint
    // into a literal and leave its font weight or colour behind. Three
    // independent passes over the same question is what made `s = "/*";`
    // open a comment that never closed and made the `//` in `"http://x"`
    // depend on whether some other regex happened to match the literal.
    const CppTokenScan::Line tokens =
        CppTokenScan::scan(text, previousBlockState() == kBlockCommentState);
    setCurrentBlockState(tokens.endsInsideBlockComment ? kBlockCommentState : 0);

    const auto paintsOverText = [&tokens](const QRegularExpressionMatch& m) {
        return CppTokenScan::overlapsAny(tokens.spans, m.capturedStart(),
                                         m.capturedLength());
    };

    // Apply single-line rules.
    for (const auto& rule : rules_) {
        auto it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            if (paintsOverText(m)) {
                continue;
            }
            setFormat(m.capturedStart(), m.capturedLength(), rule.format);
        }
    }

    // Word classes (keywords, literals, built-in types): one regex pass per
    // class, then a hash lookup per match to pick the colour.
    for (const WordClass& wc : wordClasses_) {
        auto it = wc.pattern.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            if (paintsOverText(m)) {
                continue;
            }
            const auto format = wc.formats.constFind(m.captured());
            if (format != wc.formats.constEnd()) {
                setFormat(m.capturedStart(), m.capturedLength(), *format);
            }
        }
    }

    // Comments and literals: one setFormat per span, nothing underneath it.
    for (const CppTokenScan::Span& span : tokens.spans) {
        setFormat(span.start, span.length,
                  span.kind == CppTokenScan::Kind::Literal ? stringFormat_
                                                           : commentFormat_);
    }

    // TODO/FIXME markers last, so they punch through the comment colours.
    auto todoIt = todoRule_.globalMatch(text);
    while (todoIt.hasNext()) {
        const QRegularExpressionMatch m = todoIt.next();
        setFormat(m.capturedStart(), m.capturedLength(), todoFormat_);
    }
}