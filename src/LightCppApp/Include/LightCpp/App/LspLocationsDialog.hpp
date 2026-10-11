#pragma once

#include <QDialog>
#include <QList>
#include <QString>

#include "LightCpp/App/LanguageServerClient.hpp"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

// Global namespace like the other App-layer classes (MainWindow,
// CodeEditor, ...) — see the note in ProjectSearchDialog.hpp.

/// Modeless list of LSP locations (references / declaration / implementation
/// results). Lines are lazily read from disk once per file so each row shows
/// its source text; activating a row asks the main window to open the file
/// at that line via locationActivated(). Capped at kMaxRows entries.
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

private:
    /// The (1-based) text of `line` in `filePath`, reading each file once.
    [[nodiscard]] QString lineText(const QString& filePath, int line1based);

    QTreeWidget* results_ = nullptr;
    QLabel* status_ = nullptr;
    QHash<QString, QStringList> fileLines_;
};
