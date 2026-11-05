// LightCppApp - what the Kit page's "Toolchain" drop-down offers.

#include "LightCpp/App/KitToolchainOptions.hpp"

namespace LightCpp::App {

namespace {

QString noneText()
{
    // Not translated on purpose: this module is the documentable rule, and a
    // translated marker would make the test depend on the UI language. The
    // dialog can re-label the row later without touching the decision.
    return QStringLiteral("(none)");
}

QString missingText(const QString& id)
{
    return id + QStringLiteral(" (missing)");
}

}  // namespace

KitToolchainOptions kitToolchainOptionsFor(const QStringList& visibleIds,
                                           const QString& storedId)
{
    KitToolchainOptions out;
    const QString stored = storedId.trimmed();

    for (const QString& raw : visibleIds) {
        const QString id = raw.trimmed();
        if (id.isEmpty()) {
            continue;  // a toolchain without an id cannot be referenced
        }
        bool seen = false;
        for (const KitToolchainOption& existing : out.items) {
            if (existing.value == id) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;  // one row per id: two rows offer the same answer twice
        }
        out.items.append(KitToolchainOption{id, id, false});
    }

    int current = -1;
    if (!stored.isEmpty()) {
        for (int i = 0; i < out.items.size(); ++i) {
            if (out.items[i].value == stored) {
                current = i;
                break;
            }
        }
        if (current < 0) {
            // Keep what the kit says even though nothing defines it: dropping
            // it here is what wrote the empty id back to disk.
            out.items.append(KitToolchainOption{missingText(stored), stored, true});
            current = out.items.size() - 1;
        }
    }

    out.items.append(KitToolchainOption{noneText(), QString(), false});
    if (current < 0) {
        current = out.items.size() - 1;  // stored id empty: "inherits nothing"
    }
    out.current = current;
    return out;
}

}  // namespace LightCpp::App
