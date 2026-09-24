// CppSyntaxHighlighter — basic C/C++ syntax highlighting.

#include "LightCpp/App/CppSyntaxHighlighter.hpp"

#include <QRegularExpression>
#include <QTextDocument>

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
    Rule r;

    // Preprocessor (must come before keywords to match #include etc.)
    r.pattern = QRegularExpression(QStringLiteral(R"(^\s*#\s*\w+)"));
    r.format = preprocFormat_;
    rules_.append(r);

    // Keywords
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
    };
    for (const auto& kw : keywords) {
        r.pattern = QRegularExpression(QStringLiteral("\\b") + kw + QStringLiteral("\\b"));
        r.format = keywordFormat_;
        rules_.append(r);
    }

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
    for (const auto& ty : types) {
        r.pattern = QRegularExpression(QStringLiteral("\\b") + ty + QStringLiteral("\\b"));
        r.format = typeFormat_;
        rules_.append(r);
    }

    // Literals
    r.pattern = QRegularExpression(QStringLiteral("\\btrue\\b|\\bfalse\\b|\\bnullptr\\b"));
    r.format = keywordFormat_;
    rules_.append(r);

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

    // Double-quoted string
    r.pattern = QRegularExpression(QStringLiteral(R"("(?:[^"\\]|\\.)*")"));
    r.format = stringFormat_;
    rules_.append(r);

    // Single-quoted char
    r.pattern = QRegularExpression(QStringLiteral(R"('(?:[^'\\]|\\.)*')"));
    r.format = stringFormat_;
    rules_.append(r);
}

CppSyntaxHighlighter::~CppSyntaxHighlighter() = default;

void CppSyntaxHighlighter::highlightBlock(const QString& text)
{
    // Apply single-line rules.
    for (const auto& rule : rules_) {
        auto it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            setFormat(m.capturedStart(), m.capturedLength(), rule.format);
        }
    }

    // Multi-line comment: track state across blocks.
    setCurrentBlockState(0);

    if (previousBlockState() != 1) {
        // Not inside a block comment — look for /* ... */
        const int idx = text.indexOf(QStringLiteral("/*"));
        if (idx >= 0) {
            const int end = text.indexOf(QStringLiteral("*/"), idx + 2);
            if (end < 0) {
                // Comment runs to end of block, continues next.
                setFormat(idx, text.length() - idx, commentFormat_);
                setCurrentBlockState(1);
            } else {
                setFormat(idx, end + 2 - idx, commentFormat_);
            }
        }
    } else {
        // We are inside a continued block comment.
        const int end = text.indexOf(QStringLiteral("*/"));
        if (end < 0) {
            setFormat(0, text.length(), commentFormat_);
            setCurrentBlockState(1);
        } else {
            setFormat(0, end + 2, commentFormat_);
        }
    }

    // Single-line comment //
    for (int i = 0; i < text.length() - 1; ++i) {
        if (text[i] == u'/' && text[i + 1] == u'/') {
            // Don't override inside a string — but this is simple highlighter.
            setFormat(i, text.length() - i, commentFormat_);
            break;
        }
    }
}
