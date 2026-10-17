// LightCppApp - what the Kit page's "Toolchain" drop-down offers.

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace LightCpp::App {

/// One row of the drop-down: what the user reads, and what the kit stores.
struct KitToolchainOption {
    QString text;
    QString value;
    /// The id is stored but the settings do not define it.
    bool missing = false;
};

/// The drop-down's contents, plus the row to select.
struct KitToolchainOptions {
    QList<KitToolchainOption> items;
    int current = 0;
};

/// Build the Kit page's "Toolchain" drop-down.
///
/// A non-editable QComboBox silently refuses setCurrentText() for a value it
/// does not offer: currentIndex stays -1 and currentText() answers "". The
/// kit form then committed that "" and wrote `toolchainId: null` into
/// kits.json, which is how a project kit lost the global toolchain it
/// inherited (R2-32) — after which the kit had no compiler at all.
///
/// So the stored id is always offered. When the settings do not define it, it
/// gets its own row marked missing: the page shows what is stored, the commit
/// writes it back unchanged, and the word "missing" says it needs attention.
/// The list always ends with the "inherits nothing" row, because a blank box
/// is indistinguishable from "the toolchain I picked is gone" — and because a
/// kit that names no toolchain needs a row to select.
///
/// @param visibleIds the ids the merged (base + overlay) settings define
/// @param storedId   the kit's own toolchainId, possibly empty or unknown
[[nodiscard]] KitToolchainOptions kitToolchainOptionsFor(const QStringList& visibleIds,
                                                         const QString& storedId);

}  // namespace LightCpp::App
