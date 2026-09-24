#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

class QTimer;

namespace LightCpp::App {

/// Runs clang-format over a buffer in the background.
///
/// The text is piped through stdin with `--assume-filename` naming the file it
/// belongs to. That single flag does two jobs: an unsaved buffer formats
/// correctly, and clang-format still discovers a `.clang-format` beside the
/// source. A project that ships one therefore gets its own style, which is the
/// behaviour anyone working in someone else's tree expects.
///
/// One run is in flight at a time and every request carries an id, so a slow
/// answer cannot overwrite a buffer the user has since changed.
class CodeFormatter : public QObject {
    Q_OBJECT
public:
    /// Used when the project has no `.clang-format` of its own. Microsoft
    /// braces every block on its own line.
    static constexpr const char* kFallbackStyle = "Microsoft";

    explicit CodeFormatter(QObject* parent = nullptr);
    ~CodeFormatter() override;

    /// The clang-format executable, or empty when there is none.
    [[nodiscard]] static QString toolPath();
    [[nodiscard]] static bool isAvailable();

    /// The command line for one run, with `assumeFileName` naming the buffer's
    /// file (empty when it has none). Separate from format() so the flags can
    /// be inspected without a clang-format installed. Uses the compiled-in
    /// fallback style.
    [[nodiscard]] static QStringList argumentsFor(const QString& assumeFileName);

    /// The executable this instance runs: the settings-provided path when
    /// set, else the compiled-in location.
    [[nodiscard]] QString resolvedToolPath() const;
    /// The --fallback-style this instance passes: the configured style when
    /// set (and not "file", which is what -style=file already does), else
    /// kFallbackStyle.
    [[nodiscard]] QString resolvedFallbackStyle() const;

    /// Point this instance at a specific clang-format (the settings'
    /// clangFormatPath). Empty restores the compiled-in default.
    void setToolPath(const QString& path) { toolPathOverride_ = path; }
    /// Override the fallback style (formatting.globalStyle / a kit's
    /// clangFormatStyle). "file" and empty keep the built-in default.
    void setFallbackStyle(const QString& style) { fallbackStyle_ = style; }

    /// Format `text`. The answer arrives as formatted() or failed(), carrying
    /// the returned id. A second call supersedes the first.
    int format(const QString& text, const QString& assumeFileName);

signals:
    /// `text` is the whole file as clang-format would write it.
    void formatted(int requestId, const QString& text);
    void failed(int requestId, const QString& message);

private:
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onError();

    QProcess* process_ = nullptr;
    QTimer* timeout_ = nullptr;
    /// Settings-provided overrides; empty = the compiled-in defaults.
    QString toolPathOverride_;
    QString fallbackStyle_;
    /// Id of the run currently in progress, or 0 when the process is idle.
    int inFlightId_ = 0;
    int nextId_ = 1;
};

}  // namespace LightCpp::App
