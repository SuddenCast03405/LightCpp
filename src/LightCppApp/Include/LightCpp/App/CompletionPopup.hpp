// CompletionPopup — a floating QListWidget that shows language-server
// completion items at the editor cursor and lets the user navigate with
// Up/Down/Enter/Esc or pick with the mouse.

#pragma once

#include <QListWidget>

#include "LightCpp/App/LanguageServerClient.hpp"

namespace LightCpp::App {

class CompletionPopup : public QListWidget {
    Q_OBJECT
public:
    explicit CompletionPopup(QWidget* parent = nullptr);

    /// Replace the displayed items and re-position at the given global
    /// position. The popup auto-sizes to its content (capped at a sensible
    /// max) and grabs keyboard focus for navigation.
    void present(const QList<LspCompletionItem>& items, const QPoint& globalPos);

    /// Hide and discard items. Safe to call when the popup is already hidden.
    void dismiss();

    /// Apply front-end filtering by `prefix`: hide items whose label does
    /// not start with the prefix (case-insensitive). The first surviving
    /// item is selected.
    void filterByPrefix(const QString& prefix);

    /// The currently-selected item, or nullptr if the popup is hidden or
    /// has no items.
    [[nodiscard]] const LspCompletionItem* selectedItem() const noexcept;

signals:
    /// Emitted when the user accepts an item (Enter/Return or click).
    void itemAccepted(const LightCpp::App::LspCompletionItem& item);
    /// Emitted when the user cancels (Esc, or click outside).
    void dismissed();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QList<LspCompletionItem> items_;
};

}  // namespace LightCpp::App
