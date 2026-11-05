#pragma once

#include <QDialog>
#include <QList>
#include <QString>

#include "LightCpp/App/LanguageServerClient.hpp"

class QLabel;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

// Global namespace like the other App-layer classes (MainWindow,
// CodeEditor, ...) — see the note in ProjectSearchDialog.hpp.

/// Modeless list of LSP locations (references / declaration / implementation
/// results). Rows appear immediately; each row's source text is read from
/// disk afterwards in small batches driven by a zero-interval timer, so a
/// reference list spanning hundreds of files cannot stall the GUI thread.
/// Activating a row asks the main window to open the file at that line via
/// locationActivated(). Capped at kMaxRows entries.
class LspLocationsDialog : public QDialog {
    Q_OBJECT
public:
    LspLocationsDialog(const QString& title,
                       const QList<LightCpp::App::LspLocation>& locations,
                       QWidget* parent = nullptr);

signals:
    /// The user picked a location. `line` is 1-based.
    void locationActivated(const QString& filePath, int line);

private slots:
    void onLocationActivated(QTreeWidgetItem* item, int column);
    /// Reads preview text for the next few files. Re-armed until done.
    void fillPreviewBatch();

private:
    /// The (1-based) text of `line` in `filePath`, reading each file once.
    [[nodiscard]] QString lineText(const QString& filePath, int line1based);

    /// A row still waiting for its preview text.
    struct PendingRow {
        QTreeWidgetItem* item = nullptr;
        QString filePath;
        int line1based = 0;
    };

    QTreeWidget* results_ = nullptr;
    QLabel* status_ = nullptr;
    QTimer* fillTimer_ = nullptr;
    QList<PendingRow> pending_;
    int pendingIndex_ = 0;
    QHash<QString, QStringList> fileLines_;
};
