#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QStringList>

/// Minimal C/C++ syntax highlighter using QSyntaxHighlighter.
/// Handles keywords, types, strings, comments, preprocessor, numbers.
class CppSyntaxHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit CppSyntaxHighlighter(QTextDocument* parent = nullptr);
    ~CppSyntaxHighlighter() override;

protected:
    void highlightBlock(const QString& text) override;

private:
    /// A set of whole words (the keywords, the built-in types) matched by ONE
    /// alternation regex, with the format per word looked up in a hash. Two
    /// of these replace the ~110 single-word regexes this highlighter used to
    /// run over every block, which is what made big pastes stutter.
    struct WordClass {
        QRegularExpression pattern;
        QHash<QString, QTextCharFormat> formats;
    };

    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QList<Rule> rules_;
    QList<WordClass> wordClasses_;
    /// String and char literals, applied AFTER rules_ and wordClasses_: a
    /// later setFormat() wins, so the literal green must land last or every
    /// keyword/number/identifier inside a string ("const char* s = "size_t
    /// bytes";") gets repainted in the word colour.
    QList<Rule> stringRules_;

    /// TODO / FIXME / XXX / HACK markers, applied AFTER the comment rules
    /// so they stay visible inside comments (the comments would otherwise
    /// paint right over them — rules_ runs before the comment passes).
    QRegularExpression todoRule_;
    QTextCharFormat todoFormat_;

    QTextCharFormat keywordFormat_;
    QTextCharFormat typeFormat_;
    QTextCharFormat stringFormat_;
    QTextCharFormat commentFormat_;
    QTextCharFormat preprocFormat_;
    QTextCharFormat numberFormat_;
    QTextCharFormat funcFormat_;
};
