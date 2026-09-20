#pragma once

#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QRegularExpression>

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
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };
    QList<Rule> rules_;

    QTextCharFormat keywordFormat_;
    QTextCharFormat typeFormat_;
    QTextCharFormat stringFormat_;
    QTextCharFormat commentFormat_;
    QTextCharFormat preprocFormat_;
    QTextCharFormat numberFormat_;
    QTextCharFormat funcFormat_;
};
