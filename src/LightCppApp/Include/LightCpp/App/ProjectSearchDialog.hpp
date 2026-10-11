#pragma once

#include <QDialog>
#include <QList>
#include <QPair>
#include <QRegularExpression>
#include <QString>
#include <QThread>
#include <atomic>
#include <optional>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// Global namespace like the other App-layer classes (MainWindow,
// CodeEditor, ...) — this header is consumed by MainWindow.cpp, whose
// member functions live at global scope and resolve unqualified names
// there. The App-library classes (Core/Settings/...) keep their namespaces.

/// Background scanner for ProjectSearchDialog: walks a directory tree and
/// reports every line that matches the pattern. Runs in its own thread so
/// the GUI never blocks on file I/O (the project rule for file reads).
class ProjectSearchThread : public QThread {
    Q_OBJECT
public:
    ProjectSearchThread(const QString& root, const QString& pattern,
                        bool caseSensitive, bool wholeWord, bool useRegex,
                        QObject* parent = nullptr);

    /// Ask the scan to stop at the next file. Checked between files, so it
    /// takes effect within milliseconds even on huge trees.
    void abort() { aborted_.store(true); }

    void run() override;

signals:
    /// The matches found in one file. `line` is 1-based; `text` is the
    /// trimmed line content (cut to a sane length).
    void fileHits(const QString& filePath,
                  const QList<QPair<int, QString>>& hits);
    /// The walk ended. `aborted` distinguishes a user restart from a
    /// completed scan (hit cap or tree done).
    void scanFinished(int filesScanned, int totalHits, bool abortedByUser);

private:
    /// True when `fileName` is a text/source file worth scanning.
    [[nodiscard]] static bool isSearchableFile(const QString& fileName);
    /// Read the file as text; nullopt when it is binary or too large.
    [[nodiscard]] std::optional<QString> readTextFile(const QString& path) const;

    QString root_;
    QRegularExpression regex_;
    std::atomic<bool> aborted_{false};
};

/// "Search in Project" — a modeless dialog that scans the Explorer root
/// (or a folder the user picks) in a background thread and lists the hits
/// grouped by file. Double-clicking (or Enter on) a hit asks the main
/// window to open the file at that line via resultActivated().
class ProjectSearchDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProjectSearchDialog(const QString& root, QWidget* parent = nullptr);
    ~ProjectSearchDialog() override;

signals:
    /// The user picked a hit. `line` is 1-based.
    void resultActivated(const QString& filePath, int line);

private slots:
    void startSearch();
    void onFileHits(const QString& filePath,
                    const QList<QPair<int, QString>>& hits);
    void onScanFinished(int filesScanned, int totalHits, bool abortedByUser);
    void onResultActivated(QTreeWidgetItem* item, int column);

private:
    void updateStatus();

    QString root_;
    QLineEdit* pattern_ = nullptr;
    QCheckBox* caseSensitive_ = nullptr;
    QCheckBox* wholeWord_ = nullptr;
    QCheckBox* useRegex_ = nullptr;
    QPushButton* searchButton_ = nullptr;
    QTreeWidget* results_ = nullptr;
    QLabel* status_ = nullptr;

    ProjectSearchThread* thread_ = nullptr;
    int filesScanned_ = 0;
    int totalHits_ = 0;
};
