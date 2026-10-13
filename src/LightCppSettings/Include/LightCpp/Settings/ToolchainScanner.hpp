// ToolchainScanner — recursive, read-only discovery of toolchain folders.
//
// Given a root directory it walks the tree (bounded depth, never entering
// uninteresting folder names such as include/ or lib/) and returns one
// Toolchain per directory that contains a compiler executable. Pure logic:
// Qt Core only, no widgets, so the walk rules are unit-testable.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <atomic>

#include "LightCpp/Settings/SettingsTypes.hpp"

namespace LightCpp::Settings {

struct ToolchainScanOptions {
    /// How many directory levels below the chosen root are examined (the
    /// root itself is always examined, whatever the value). Default 5.
    int maxDepth = 5;

    /// Folder names the walk never enters (matched without case; the root
    /// itself is never excluded). Defaults keep the walk out of the heavy,
    /// compiler-free subtrees of a typical SDK layout.
    QStringList excludedDirectories = {
        QStringLiteral("include"),
        QStringLiteral("lib"),
        QStringLiteral("share"),
        QStringLiteral("libexec"),
        QStringLiteral("var"),
    };

    /// Optional cancellation flag. When it is set to true the walk stops at
    /// the next directory and returns what it has found so far — which is
    /// what lets a caller run a deep scan on a worker thread and still tear
    /// itself down promptly (the scan of a whole drive can take minutes).
    /// Owned by the caller and read from the scanning thread.
    std::atomic<bool>* abort = nullptr;
};

/// True when the caller asked for the scan to stop.
[[nodiscard]] inline bool scanAborted(const ToolchainScanOptions& options)
{
    return options.abort != nullptr && options.abort->load();
}

/// Walk `root` and collect one toolchain per directory that hosts a
/// compiler (gcc/g++, clang/clang++, cl). Each toolchain is named after
/// its directory and filled with every known tool executable sitting in
/// the same folder (debugger, clangd, cmake, ...). The walk does not
/// descend below a directory that already hosts a toolchain. Ids stay
/// empty — the caller assigns unique ones. Returns the toolchains in
/// walk order; a non-existent root yields an empty list.
QList<Toolchain> scanForToolchains(const QString& root,
                                   const ToolchainScanOptions& options = {});

}  // namespace LightCpp::Settings
