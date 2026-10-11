// CompletionPopup — implementation.

#include "LightCpp/App/CompletionPopup.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QScreen>
#include <QScrollBar>
#include <QWindow>

namespace LightCpp::App {

namespace {

/// A one-letter glyph per LSP CompletionItemKind, drawn at the start of
/// each row so the user can tell functions from variables/types/keywords
/// at a glance. The LSP kinds used here are the common clangd ones.
QString kindGlyph(int kind)
{
    switch (kind) {
    case 2:  return QStringLiteral("M");   // Method
    case 3:  return QStringLiteral("f");   // Function
    case 4:  return QStringLiteral("C");   // Constructor
    case 5:  return QStringLiteral("F");   // Field
    case 6:  return QStringLiteral("v");   // Variable
    case 7:  return QStringLiteral("T");   // Class
    case 8:  return QStringLiteral("I");   // Interface
    case 9:  return QStringLiteral("M");   // Module
    case 10: return QStringLiteral("P");   // Property
    case 12: return QStringLiteral("v");   // Value
    case 13: return QStringLiteral("E");   // Enum
    case 14: return QStringLiteral("k");   // Keyword
    case 15: return QStringLiteral("S");   // Snippet
    case 17: return QStringLiteral("F");   // File
    case 18: return QStringLiteral("M");   // Reference
    case 19: return QStringLiteral("F");   // Folder
    case 20: return QStringLiteral("E");   // EnumMember
    case 21: return QStringLiteral("C");   // Constant
    case 22: return QStringLiteral("s");   // Struct
    case 23: return QStringLiteral("E");   // Event
    case 24: return QStringLiteral("O");   // Operator
    case 25: return QStringLiteral("T");   // TypeParameter
    default: return QStringLiteral(" ");
    }
}

}  // namespace

CompletionPopup::CompletionPopup(QWidget* parent)
    : QListWidget(parent)
{
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::Tool);
    setAttribute(Qt::WA_ShowWithoutActivating, false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setUniformItemSizes(true);
    setEditTriggers(NoEditTriggers);
    setSelectionMode(SingleSelection);
    setFocus();
    connect(this, &QListWidget::itemActivated,
            this, [this](QListWidgetItem* it) {
        if (it) {
            const int idx = row(it);
            if (idx >= 0 && idx < items_.size()) {
                emit itemAccepted(items_[idx]);
            }
            dismiss();
        }
    });
    connect(this, &QListWidget::itemClicked,
            this, [this](QListWidgetItem* it) {
        if (it) {
            const int idx = row(it);
            if (idx >= 0 && idx < items_.size()) {
                emit itemAccepted(items_[idx]);
            }
            dismiss();
        }
    });
}

void CompletionPopup::present(const QList<LspCompletionItem>& items,
                              const QPoint& globalPos)
{
    items_ = items;
    clear();
    for (const LspCompletionItem& it : items) {
        const QString text = QStringLiteral("%1  %2")
                                 .arg(kindGlyph(it.kind), it.label);
        auto* row = new QListWidgetItem(text, this);
        if (!it.detail.isEmpty()) {
            row->setToolTip(it.detail + (it.documentation.isEmpty()
                                          ? QString() : QStringLiteral("\n\n") + it.documentation));
        } else if (!it.documentation.isEmpty()) {
            row->setToolTip(it.documentation);
        }
    }
    if (items_.isEmpty()) {
        hide();
        return;
    }

    setCurrentRow(0);

    // Size: width = max item text width + scrollbar margin; height = up to
    // 12 rows, capped by the actual item count.
    const QFontMetrics fm = fontMetrics();
    int maxText = 0;
    for (int i = 0; i < count(); ++i) {
        maxText = qMax(maxText, fm.horizontalAdvance(item(i)->text()));
    }
    const int width = qMax(220, maxText + 40);
    const int rowH = qMax(20, fm.height() + 6);
    const int visibleRows = qMin(count(), 12);
    const int height = rowH * visibleRows + 6;

    // Reposition if the popup would run off-screen.
    QPoint pos = globalPos;
    QScreen* screen = nullptr;
    if (auto* w = windowHandle()) screen = w->screen();
    if (!screen) screen = QApplication::primaryScreen();
    if (screen) {
        const QRect g = screen->availableGeometry();
        if (pos.x() + width > g.right()) pos.setX(qMax(g.left(), g.right() - width));
        if (pos.y() + height > g.bottom()) {
            // Prefer flipping above the caret; when there is no room above
            // either, dock to the bottom edge. Clamping to g.top() (the old
            // behaviour) would tear the popup away from the cursor.
            const int flipped = pos.y() - height - 20;
            pos.setY(flipped >= g.top()
                         ? flipped
                         : qMax(g.top(), g.bottom() - height + 1));
        }
    }

    setFixedSize(width, height);
    move(pos);
    show();
    raise();
    activateWindow();
    setFocus();
}

void CompletionPopup::dismiss()
{
    items_.clear();
    clear();
    hide();
    emit dismissed();
}

void CompletionPopup::filterByPrefix(const QString& prefix)
{
    for (int i = 0; i < count(); ++i) {
        QListWidgetItem* it = item(i);
        if (i >= items_.size()) break;
        const bool match = items_[i].label.startsWith(prefix, Qt::CaseInsensitive);
        it->setHidden(!match);
    }
    // present() sized the popup to the unfiltered item count, so a list
    // filtered down from 12 rows to 4 would keep a large empty area. Recompute
    // the geometry from the visible rows (the height can only shrink, so the
    // popup never grows off-screen here; the width may shrink with the texts).
    int visible = 0;
    int maxText = 0;
    const QFontMetrics fm = fontMetrics();
    for (int i = 0; i < count(); ++i) {
        QListWidgetItem* it = item(i);
        if (it->isHidden()) {
            continue;
        }
        ++visible;
        maxText = qMax(maxText, fm.horizontalAdvance(it->text()));
    }
    if (visible > 0) {
        const int rowH = qMax(20, fm.height() + 6);
        setFixedSize(qMax(220, maxText + 40), rowH * qMin(visible, 12) + 6);
    }
    // Select the first visible item.
    for (int i = 0; i < count(); ++i) {
        if (!item(i)->isHidden()) {
            setCurrentRow(i);
            return;
        }
    }
    // No matches — hide.
    dismiss();
}

const LspCompletionItem* CompletionPopup::selectedItem() const noexcept
{
    const int row = currentRow();
    if (row < 0 || row >= items_.size()) return nullptr;
    return &items_[row];
}

void CompletionPopup::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter: {
        if (const LspCompletionItem* it = selectedItem()) {
            emit itemAccepted(*it);
            dismiss();
        }
        return;
    }
    case Qt::Key_Escape:
        dismiss();
        return;
    case Qt::Key_Up:
    case Qt::Key_Down:
        // Skip hidden rows; fall through to the default list navigation.
        QListWidget::keyPressEvent(event);
        return;
    default:
        break;
    }
    // Any other key (a letter, a space, a backspace...): forward to the
    // editor by re-emitting it as a shortcut-less event so the editor
    // processes it. The simplest portable way is to accept the event and
    // tell the parent (editor) to handle it via QApplication::sendEvent.
    // We do that by NOT consuming the event here and re-sending it to the
    // focus proxy's parent (the CodeEditor).
    if (auto* target = parentWidget()) {
        QApplication::sendEvent(target, event);
    } else {
        QListWidget::keyPressEvent(event);
    }
}

void CompletionPopup::hideEvent(QHideEvent* event)
{
    QListWidget::hideEvent(event);
    emit dismissed();
}

}  // namespace LightCpp::App
