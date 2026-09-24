#pragma once

#include <QPlainTextEdit>
#include <QSet>
#include <QTextCharFormat>
#include <QTextCursor>

#include "LightCpp/App/LanguageServerClient.hpp"
#include "LightCpp/Core/FoldModel.hpp"

class CppSyntaxHighlighter;
class LineNumberArea;
class QPainter;
class QPaintEvent;
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
///
/// Provides go-to-definition: Ctrl+Click on an identifier (or F12 with the
/// caret on one) emits definitionRequested(); MainWindow asks clangd and
/// jumps through gotoLocation().
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

    // --- code folding -----------------------------------------------------
    /// The foldable ranges in the current text, in document order. Recomputed
    /// after every edit, so this is always consistent with what is on screen.
    [[nodiscard]] const QList<LightCpp::Core::Fold::FoldRegion>& foldRegions() const noexcept
    {
        return foldRegions_;
    }
    [[nodiscard]] bool isFolded(int regionIndex) const;
    /// Collapse or expand one region, addressed by its index in foldRegions().
    void setFolded(int regionIndex, bool folded);
    /// Toggle the region whose opening line is `line`. Returns false when that
    /// line starts no region — which is the case for most lines, so the gutter
    /// can call this on every click.
    bool toggleFoldAtLine(int line);
    /// Collapse the innermost region containing the caret.
    void foldAtCaret();
    /// Expand the innermost collapsed region containing the caret.
    void unfoldAtCaret();
    void foldAll();
    void unfoldAll();

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

    /// Go-to-definition was asked for at (line, column) — Ctrl+Click on an
    /// identifier, or F12 with the caret on one. Both are 0-based.
    void definitionRequested(int line, int column);

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
    /// the cursor sits at the very start or end of the document.
    [[nodiscard]] static QChar characterBefore(const QTextCursor& cursor);
    [[nodiscard]] static QChar characterAfter(const QTextCursor& cursor);

    // --- code folding -----------------------------------------------------
    /// Recompute foldRegions_ from the document text.
    void rebuildFoldRegions();
    /// Apply folded_ to the document: hide the lines the collapsed regions
    /// cover, show the rest. Cheap and idempotent — it does nothing when the
    /// hidden set is unchanged, which matters because this runs on every edit.
    void applyFoldState();
    /// True while `line` sits inside a collapsed region.
    [[nodiscard]] bool isLineHidden(int line) const;
    /// Expand whichever region hides `line`, if any.
    void unfoldLine(int line);
    /// Move the caret out of hidden text — otherwise it would sit somewhere
    /// invisible and the next keystroke would go to a line nobody can see.
    void keepCaretOutsideFolds();
    /// Handle a click in the gutter's fold column (`pos` in gutter
    /// coordinates), toggling the region on the clicked line.
    void handleFoldMarkerClick(const QPoint& pos);
    /// Draw the `+`/`−` box for one region in the gutter's fold strip, plus the
    /// hairline that signals more content below a collapsed range. `top` and
    /// `bottom` are the header line's vertical extent in gutter coordinates.
    void paintFoldMarker(QPainter* painter, int top, int bottom, int regionIndex);

    /// A collapsed region, remembered as "the Nth foldable range" rather than
    /// by line number. An edit above it shifts every line but leaves its
    /// ordinal untouched, which is what keeps a fold attached to its block
    /// while the text moves. `headerText` pins it to the actual construct: if
    /// the Nth range now starts with different text, the fold described
    /// something that is gone and is dropped instead of jumping to a
    /// look-alike.
    struct FoldAnchor {
        int regionIndex = 0;
        QString headerText;
    };

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

    QList<LightCpp::Core::Fold::FoldRegion> foldRegions_;
    QList<FoldAnchor> folded_;
    /// The hidden line numbers currently applied to the document. Compared
    /// against the wanted set so a relayout only happens when it must.
    QSet<int> hiddenLines_;

    friend class LineNumberArea;
};
