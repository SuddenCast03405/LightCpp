#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

/// LightCpp::Settings — the IDE's configuration model.
///
/// Configuration lives in JSON documents next to the executable
/// (<exe dir>/config/*.json), in the spirit of VSCode's settings but keyed
/// by an explicit "type" field instead of a dotted key namespace:
///
///   "toolchains"   — registries of discovered compilers/debuggers/tools
///   "defaults"     — which kit / configuration / tools to use by default
///   "kit"          — one build environment (compilers, flags, styles, ...)
///   "preferences"  — fonts, encodings, shortcuts, performance limits
///
/// A working directory may carry its own config/*.json on top of the
/// executable's; documents with the same type+id are merged field by field,
/// with the later layer winning (arrays replace wholesale).
namespace LightCpp::Settings {

// ---------------------------------------------------------------------------
// toolchain
// ---------------------------------------------------------------------------

/// One named toolchain: the tool paths a kit can inherit wholesale. Kits
/// either name their own tools or set inheritToolchain and reference one of
/// these by id.
struct Toolchain {
    QString id;
    QString name;

    QString cCompiler;
    QString cppCompiler;
    QString debugger;      // gdb / lldb / ...
    QString clangd;
    QString clangFormat;
    /// "cmake" | "ninja" | "gnuMake" | "msbuild" -> one path ("" = unset).
    QHash<QString, QString> buildTools;

    /// A toolchain is usable when it is identified and names both compilers.
    [[nodiscard]] bool isValid() const noexcept
    {
        return !id.isEmpty() && !cCompiler.isEmpty() && !cppCompiler.isEmpty();
    }
};

// ---------------------------------------------------------------------------
// defaults
// ---------------------------------------------------------------------------

struct GlobalDefaults {
    QString defaultKitId;
    QString defaultBuildConfiguration;  // e.g. "Debug" / "Release"
    QString gitPath;
    QString clangdPath;
    QString clangFormatPath;
};

// ---------------------------------------------------------------------------
// kit
// ---------------------------------------------------------------------------

/// One entry of a kit's buildConfigurations[]: named flag sets shown in the
/// toolbar's configuration drop-down.
struct KitBuildConfiguration {
    QString name;
    bool inheritFromKit = true;  ///< false: the kit's own flags are replaced
    QStringList compileFlags;
    QStringList linkFlags;
    QString cppStandard;         ///< empty = use the kit's standard
    /// Extra build-tool arguments, used only when the project does not
    /// define its own ("cmake" | "ninja" | "gnuMake" | "msbuild").
    QHash<QString, QStringList> buildToolArgs;
};

struct Kit {
    QString id;
    QString name;
    bool enabled = true;
    bool isDefault = false;

    /// When true, the empty tool fields below are filled from the toolchain
    /// named by toolchainId (see SettingsStore::kits()). A kit that inherits
    /// still keeps its own fields as per-field overrides: anything the kit
    /// names explicitly wins over the toolchain.
    bool inheritToolchain = false;
    QString toolchainId;

    // Necessary (unless inherited from a toolchain)
    QString cCompiler;
    QString cppCompiler;

    // Optional
    QString debugger;
    QString clangd;
    QString clangFormat;
    /// "cmake" | "ninja" | "gnuMake" | "msbuild" -> one path ("" = unset).
    QHash<QString, QString> buildTools;
    QString cppStandard;         ///< e.g. "gnu++20"; empty = compiler default
    QStringList compileFlags;    ///< kit-global compile flags
    QStringList linkFlags;       ///< kit-global link flags
    QString gitPath;             ///< kit-specific git, empty = default
    QStringList gitArgs;
    QString clangFormatStyle;    ///< "file" or a preset (LLVM, Microsoft, ...)
    QString clangdFallbackStyle;
    QList<KitBuildConfiguration> buildConfigurations;

    /// A kit is usable when it is identified and names both compilers.
    [[nodiscard]] bool isValid() const noexcept
    {
        return !id.isEmpty() && !cCompiler.isEmpty() && !cppCompiler.isEmpty();
    }
};

// ---------------------------------------------------------------------------
// preferences
// ---------------------------------------------------------------------------

struct FontSpec {
    QString family;
    double size = 0;  ///< points; 0 = unset (keep the built-in default)
    [[nodiscard]] bool isValid() const noexcept
    {
        return !family.isEmpty() && size > 0;
    }
};

/// File-size limits (in MB) above which expensive per-file features switch
/// themselves off. 0 = no limit.
struct LargeFileLimits {
    int syntaxHighlightDisabledAboveMB = 0;
    int indexDisabledAboveMB = 0;
    int parseDisabledAboveMB = 0;
};

struct ClangdService {
    bool enabledByDefault = true;
    bool completion = true;
    bool diagnostics = true;
    /// Gates the index-dependent navigation (go-to-definition). clangd's own
    /// background index stays off regardless — it writes caches into the
    /// user's source tree, which this IDE does not do.
    bool indexing = true;
    QString fallbackStyle;  ///< "" or "file" = clangd's own default
};

/// Which engine answers code-completion requests, and how stubbornly the IDE
/// retries when it cannot.
struct CompletionService {
    /// "clangd" (default) | "libclang" | "ccls".
    QString backend = QStringLiteral("clangd");
    /// How many times a failed completion request (empty answer, server that
    /// would not start or died) is retried before giving up. 0 = no retry.
    int retryCount = 2;
};

struct ClangFormatService {
    bool enabled = true;
    bool formatOnSave = false;
    /// "file" = keep the built-in fallback; otherwise a preset name used
    /// when the file has no .clang-format of its own.
    QString style;
};

struct Preferences {
    FontSpec toolbarFont;
    FontSpec editorFont;
    FontSpec outputFont;

    QString defaultEncoding;         ///< e.g. "UTF-8"; canonical Text name
    QStringList detectionPriority;   ///< modelled; detection order is fixed
    QString lineEndingDefault;       ///< "auto" | "LF" | "CRLF"

    /// When true, start-up checks the default kit's compilers and offers to
    /// open the settings when one cannot be found.
    bool checkCompilersAtStartup = true;

    QString shortcutClangd;      ///< Syntax Check toggle; empty = none
    QString shortcutClangFormat; ///< Format Document; empty = built-ins

    LargeFileLimits largeFile;
    ClangdService clangd;
    ClangFormatService clangFormat;
    /// Code-completion engine selection + failure retry policy.
    CompletionService completion;

    /// Global formatting style: "file" = keep the built-in fallback,
    /// otherwise the --fallback-style passed to clang-format.
    QString globalStyle;
    /// Reserved: content for a generated .clang-format (not consumed yet).
    QString defaultStyleContent;

    /// run.output — how the built program is launched. "program" (default)
    /// captures its output in the IDE's Program Output channel; anything
    /// else the App layer does not recognise is treated as "program".
    /// "consolePauser" runs it via ConsolePauser.exe in an external console
    /// window; "cmd" / "powershell" run it inside an external cmd.exe /
    /// PowerShell window that stays open after the program exits.
    QString programOutput = QStringLiteral("program");
};

// ---------------------------------------------------------------------------
// Effective (resolved) values
// ---------------------------------------------------------------------------

/// The flags one named build configuration contributes on top of a kit —
/// what the compiler command line is actually built from.
struct EffectiveBuildConfig {
    QString name;
    QStringList compileFlags;
    QStringList linkFlags;
    QString cppStandard;
    QHash<QString, QStringList> buildToolArgs;
};

}  // namespace LightCpp::Settings
