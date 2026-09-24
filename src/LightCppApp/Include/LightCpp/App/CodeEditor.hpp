#pragma once

#include <QPlainTextEdit>
#include <QTextCharFormat>
#include <QTextCursor>

#include "LightCpp/App/LanguageServerClient.hpp"

class CppSyntaxHighlighter;
class LineNumberArea;
class QTimer;
namespace LightCpp::App { class CompletionPopup; }

/// QPlainTextEdit subclass with a left-side line-number gutter and
/// basic C++ syntax highlighting.  Drop-in replacement for QScintilla
/// in the "plain text" phase of the project.
///
/// Also renders language-server diagnostics: wavy underlines under the
/// offending ranges (red for errors, amber for warnings) plus a small
/// marker in the line-number gutter.
///
/// Provides minimal code-completion UX: typing ".", ":", ">" or pressing
/// Ctrl+Space asks the language server for completions via the
/// completionRequested() signal; MainWindow forwards the request to
/// clangd and feeds results back through presentPopup().
class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEditor(QWidget* parent = nullptr);
    ~CodeEditor() override;

    void setFilePath(const QString& path) { filePath_ = path; }
    [[nodiscard]] const QString& filePath() const noexcept { return filePath_; }

    /// Path used to identify this buffer to the language server.
    /// Unsaved ("Untitled") buffers get a virtual path so clangd can still
    /// check them — clangd keys everything off file URIs. Empty lintPath
    /// falls back to filePath().
    void setLintPath(const QString& path) { lintPath_ = path; }
    [[nodiscard]] QString lintPath() const noexcept
    {
        return lintPath_.isEmpty() ? filePath_ : lintPath_;
    }

    void setEncodingLabel(const QString& label) { encodingLabel_ = label; }
    [[nodiscard]] const QString& encodingLabel() const noexcept { return encodingLabel_; }

    /// Load raw UTF-8 text into the editor and reset the undo stack.
    void loadUtf8(const QByteArray& utf8);

    /// Return the current content as UTF-8.
    [[nodiscard]] QByteArray toUtf8() const;

    /// Mark the document as dirty / clean.
    void setModified(bool modified);
    [[nodiscard]] bool isModified() const;

    /// Replace the current set of language-server diagnostics and
    /// re-render decorations. An empty list clears all markers.
    /// Emits diagnosticsChanged() so the host can refresh its problem list.
    void setDiagnostics(const QList<LightCpp::App::LspDiagnostic>& diagnostics);
    [[nodiscard]] const QList<LightCpp::App::LspDiagnostic>& diagnostics() const noexcept
    {
        return diagnostics_;
    }
    /// The stored diagnostics with the same position normalisation that the
    /// underlines use, so a problem list reports the line the user sees.
    [[nodiscard]] QList<LightCpp::App::LspDiagnostic> normalizedDiagnostics() const;

    /// Move the caret to (line, column) — both 0-based, as reported by the
    /// language server — scroll it into view and take focus.
    void gotoLocation(int line, int column);

public slots:
    /// Show the completion popup with `items` at the editor cursor's
    /// global position. Hides the popup if the list is empty.
    void presentPopup(const QList<LightCpp::App::LspCompletionItem>& items);
    /// Hide the completion popup if it is visible.
    void dismissPopup();

signals:
    /// Emitted when the editor wants completion at the current cursor.
    /// `line` / `column` are 0-based; `triggerChar` is the character
    /// that caused the request (".", ":", ">") or empty for Ctrl+Space.
    void completionRequested(int line, int column, const QString& triggerChar);

    /// The diagnostic set of this buffer changed. `path` is lintPath(),
    /// and the diagnostics are already position-normalised.
    void diagnosticsChanged(const QString& path,
                            const QList<LightCpp::App::LspDiagnostic>& diagnostics);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    /// Intercepts QEvent::ToolTip to show the diagnostic message for the
    /// text under the mouse (extra-selection tooltips don't work natively).
    bool event(QEvent* event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect& rect, int dy);
    void highlightCurrentLine();

private:
    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent* event);
    /// Rebuild the extra selections (current-line highlight + diagnostic
    /// underlines). Called whenever either input changes.
    void updateExtraSelections();
    /// Send a completion request at the current cursor position.
    void requestCompletionAtCursor(const QString& triggerChar);
    /// Re-filter the visible popup by the identifier before the cursor.
    void refreshPopupFilter();
    /// Replace the identifier (or partial identifier) before the cursor
    /// with `item.insertText` and position the cursor at the end.
    void applyCompletionItem(const LightCpp::App::LspCompletionItem& item);
    /// Adjusts diagnostics for rendering: clangd reports some errors (e.g.
    /// "#include" without a filename) as zero-width ranges at column 0 of
    /// the EMPTY line AFTER the mistake. Visually those belong to the end
    /// of the previous line — this moves the anchor there.
    [[nodiscard]] LightCpp::App::LspDiagnostic
    normalizedDiagnostic(const LightCpp::App::LspDiagnostic& d) const;

    // --- bracket auto-pairing ------------------------------------------
    /// Handle a typed bracket/quote key. Inserts the closing half, wraps a
    /// selection, or steps over a closing bracket that is already there.
    /// Returns true when the keystroke was consumed.
    bool autoCloseBracket(QChar typed);
    /// Delete both halves of an empty pair — `(|)` + Backspace removes the
    /// pair instead of leaving a stray `)`. Returns true when handled.
    bool backspaceEmptyPair();
    /// The closing character that pairs with `opener`, or a null QChar when
    /// `opener` is not an auto-paired character.
    [[nodiscard]] static QChar closingBracketFor(QChar opener);
    [[nodiscard]] static bool isClosingBracket(QChar ch);
    [[nodiscard]] static bool isQuote(QChar ch);
    /// Character immediately before / after `cursor`, or a null QChar when
    /// the cursor sits at the very start / end of the document.
    [[nodiscard]] static QChar characterBefore(const QTextCursor& cursor);
    [[nodiscard]] static QChar characterAfter(const QTextCursor& cursor);

    LineNumberArea* lineNumberArea_;
    CppSyntaxHighlighter* highlighter_;
    LightCpp::App::CompletionPopup* popup_;
    /// Fires ~200ms after the last identifier keystroke to request
    /// "as-you-type" completion (no trigger char needed).
    QTimer* completionIdleTimer_ = nullptr;
    QString filePath_;
    QString lintPath_;
    QString encodingLabel_;
    QList<LightCpp::App::LspDiagnostic> diagnostics_;

    friend class LineNumberArea;
};
