#pragma once

#include <QPlainTextEdit>
#include <QPointer>
#include <QSet>
#include <QTextCharFormat>
#include <QTextCursor>

#include <string>
#include <vector>

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

    /// Apply the editor font from the settings: the widget font, the tab
    /// width (four monospace cells of the new font) and the gutter metrics.
    void setEditorFont(const QFont& font);

    /// Switch the C++ highlighter on or off for this document. Used for
    /// files above the configured large-file threshold, where tokenising
    /// every keystroke costs more than the colouring is worth.
    void setSyntaxHighlightEnabled(bool enabled);
    [[nodiscard]] bool isSyntaxHighlightEnabled() const noexcept
    {
        return syntaxHighlightEnabled_;
    }

    /// What a line too long for the viewport does: true wraps it at the
    /// editor edge, false keeps one row per line and scrolls sideways.
    void setLongLinesWrap(bool wrap);
    [[nodiscard]] bool longLinesWrap() const noexcept
    {
        return longLinesWrap_;
    }

    /// files.showWhitespaces: render spaces as middle dots and tabs as
    /// arrows. Applied to the document's default text option, so it costs
    /// nothing while off.
    void setShowWhitespaces(bool show);
    /// files.showIndentGuides: a muted vertical line at each indent stop of
    /// every line that starts with whitespace.
    void setShowIndentGuides(bool show);

    void paintEvent(QPaintEvent* event) override;

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

    // --- formatting -------------------------------------------------------
    /// Replace the buffer with `text` — the whole file, as a formatter would
    /// write it — as a single undo step, keeping the caret on the same line
    /// and column. Does nothing when the text is already identical, so a
    /// format that changes nothing leaves the buffer clean and the undo stack
    /// untouched.
    void applyFormattedText(const QString& text);

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

    // --- debugging --------------------------------------------------------
    /// The set of 0-based lines carrying breakpoints in this buffer; painted
    /// as red dots in the gutter. Replaces the previous set wholesale.
    void setBreakpointLines(const QSet<int>& lines);
    [[nodiscard]] const QSet<int>& breakpointLines() const noexcept
    {
        return breakpointLines_;
    }

    // --- bookmarks --------------------------------------------------------
    /// Per-buffer bookmarks (0-based lines, like the breakpoints). Bookmarks
    /// belong to the editor tab and are not persisted — they mark "come back
    /// here while I work on this file", not project state.
    void toggleBookmark();
    void nextBookmark();
    void prevBookmark();
    [[nodiscard]] const QSet<int>& bookmarkLines() const noexcept
    {
        return bookmarkLines_;
    }

public slots:
    /// Show the completion popup with `items` at the editor cursor's
    /// global position. Hides the popup if the list is empty.
    void presentPopup(const QList<LightCpp::App::LspCompletionItem>& items);
    /// Hide the completion popup if it is visible.
    void dismissPopup();
    /// Show the signature-help tooltip (rich text) anchored at the caret.
    /// An empty string hides it.
    void showSignatureHelp(const QString& html);

signals:
    /// Emitted when the editor wants completion at the current cursor.
    /// `line` / `column` are 0-based; `triggerChar` is the character
    /// that caused the request (".", ":", ">") or empty for Ctrl+Space.
    void completionRequested(int line, int column, const QString& triggerChar);

    /// Emitted after `(` or `,` inside a call: the IDE asks the language
    /// server for signature help and feeds the answer back through
    /// showSignatureHelp().
    void signatureHelpRequested(int line, int column);

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
    /// Paste. Overridden so a paste cancels any pending as-you-type
    /// completion and asks for a syntax check straight away instead of
    /// waiting for the next keystroke to trigger one.
    void insertFromMimeData(const QMimeData* source) override;
    /// Intercepts QEvent::ToolTip to show the diagnostic message for the
    /// text under the mouse (extra-selection tooltips don't work natively).
    bool event(QEvent* event) override;
    /// Watches the editor's top-level window for Move/Resize while the
    /// completion popup is visible, so the popup follows the window instead
    /// of being left behind at its stale screen position.
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect& rect, int dy);
    void highlightCurrentLine();
    /// Fold regions are recomputed on a short idle timer instead of inside
    /// the textChanged handler: the analysis is a whole-document pass, so
    /// running it per keystroke — and especially once per pasted block —
    /// is what made a paste visibly stutter.
    void rebuildFoldRegionsOnIdle();

private:
    /// Reset the tab width to four monospace cells of the current font.
    void applyTabWidthForCurrentFont();
    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent* event);
    /// Rebuild the extra selections (current-line highlight + diagnostic
    /// underlines). Called whenever either input changes.
    void updateExtraSelections();
    /// Send a completion request at the current cursor position.
    void requestCompletionAtCursor(const QString& triggerChar);
    /// Re-filter the visible popup by the identifier before the cursor.
    void refreshPopupFilter();
    /// Move the visible popup back under the caret (window moved/resized).
    void repositionPopup();
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
    /// Smart Backspace: when only whitespace sits between the line start and
    /// the caret, one Backspace removes a whole indent unit (back to the
    /// previous tab stop — a 4-space step for space indentation, one tab for
    /// tab indentation). Returns true when handled; anything else (text
    /// before the caret, a selection, column 0) falls through to the
    /// default single-character delete.
    bool smartBackspace();
    /// Auto-indent on Enter: the new line inherits the current line's
    /// indentation, one level deeper when the text before the caret ends
    /// with `{`. When the auto-inserted `}` sits right after the caret
    /// (`{|}` + Enter), the pair expands into three lines with the closer
    /// back at the `{` line's indentation. Returns true when handled.
    bool autoIndentOnReturn();
    /// Type `}` at the start of an indented line: the line dedents one
    /// level first, so the closer lines up with its `{`. Returns true when
    /// handled; a `}` typed mid-line or on an unindented line is left to
    /// the default handling.
    bool autoDedentOnCloseBrace(QChar typed);
    /// The closing character that pairs with `opener`, or a null QChar when
    /// `opener` is not an auto-paired character.
    [[nodiscard]] static QChar closingBracketFor(QChar opener);
    [[nodiscard]] static bool isClosingBracket(QChar ch);
    // --- built-in snippets ------------------------------------------------
    /// Tab on a trigger word ("for", "main", ...) replaces the word with the
    /// snippet body; a single `|` in the body marks the final caret spot and
    /// every body line inherits the trigger line's indentation. Returns true
    /// when a snippet expanded.
    bool expandSnippetAtCursor();
    /// The built-in trigger word -> body table (with `|` caret markers).
    [[nodiscard]] static const QHash<QString, QString>& snippetTable();
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

    /// A collapsed region. `headerCursor` is parked on the header line and
    /// rides every edit (QTextCursor positions adjust as the document
    /// changes), so its block number keeps pointing at that same construct
    /// even when lines are added or removed above — which is what the
    /// previous "Nth foldable range" ordinal could not do: one insertion
    /// above shifted every ordinal and silently dropped the fold.
    /// `regionIndex` is re-derived on each rebuild; `headerText` is the
    /// safety net: if the line the cursor lands on now opens different
    /// text, the fold described something that is gone and is dropped
    /// instead of jumping to a look-alike.
    struct FoldAnchor {
        int regionIndex = 0;
        QString headerText;
        QTextCursor headerCursor;
    };

    LineNumberArea* lineNumberArea_;
    CppSyntaxHighlighter* highlighter_;
    bool syntaxHighlightEnabled_ = true;
    bool longLinesWrap_ = false;
    bool showWhitespaces_ = false;
    bool showIndentGuides_ = false;
    LightCpp::App::CompletionPopup* popup_;
    /// The top-level window currently watched for Move/Resize events that
    /// require repositioning the popup (installed by presentPopup()).
    QPointer<QObject> popupWatchWindow_;
    /// Fires ~200ms after the last identifier keystroke to request
    /// "as-you-type" completion (no trigger char needed).
    QTimer* completionIdleTimer_ = nullptr;
    /// Coalesces the fold analysis: restarted on every text change and run
    /// once the burst is over, so a paste costs ONE whole-document pass
    /// instead of one per inserted change notification.
    QTimer* foldRebuildTimer_ = nullptr;
    QString filePath_;
    QString lintPath_;
    QString encodingLabel_;
    QList<LightCpp::App::LspDiagnostic> diagnostics_;
    QSet<int> breakpointLines_;  // 0-based lines with breakpoints (gutter dots)
    QSet<int> bookmarkLines_;    // 0-based lines with bookmarks

    QList<LightCpp::Core::Fold::FoldRegion> foldRegions_;
    QList<FoldAnchor> folded_;
    /// The hidden line numbers currently applied to the document. Compared
    /// against the wanted set so a relayout only happens when it must.
    QSet<int> hiddenLines_;

    /// Cached plain text for the bracket/symbol extra selections, keyed by
    /// the document revision. Cursor moves used to copy the whole document
    /// and re-encode it to UTF-8 on EVERY keystroke/arrow press; with the
    /// cache the copies happen once per edit, and pure cursor movement costs
    /// nothing. `selectionCharToByte_` maps QString indices to UTF-8 byte
    /// offsets (size+1 entries), so the byte positions the Core scanners
    /// return can be turned back into document positions — on non-ASCII
    /// text the two number spaces are NOT the same, and using bytes as
    /// positions drew the highlights in the wrong columns.
    int selectionTextRevision_ = -1;
    QString selectionText_;
    std::string selectionTextBytes_;
    std::vector<int> selectionCharToByte_;

    friend class LineNumberArea;
};
