// PluginPaths — where the IDE looks for plugins, decided in exactly one
// place. The plugins directory used to be resolved inside loadPlugins()
// and kept in a local variable, while the plugin panel and the
// enable/disable/uninstall actions rebuilt `<exeDir>/plugins` by hand: run
// the raw build-tree executable and the panel listed nothing, the toggles
// edited paths that did not exist, and the reload banner never appeared
// (R2-17). Every consumer now asks here.

#pragma once

#include <QString>

namespace LightCpp::Plugin {

/// How many ancestor levels resolvePluginsDirectory() walks up by default.
inline constexpr int kPluginsDirSearchLevels = 4;

/// The plugins directory a staged build uses: `<executableDir>/plugins`.
/// This is also what resolvePluginsDirectory() falls back to, so a caller
/// can tell "we are on the build-tree directory" by comparing the two.
[[nodiscard]] QString defaultPluginsDirectory(const QString& executableDir);

/// Resolve the plugins directory to use for an executable living in
/// `executableDir`.
///
/// `<executableDir>/plugins` wins whenever it exists. Otherwise the
/// build-tree layout is assumed: the raw exe sits deep inside the build
/// directory (.../src/LightCppApp) while the plugin dlls land in
/// `<build>/plugins` — only the staged dist/ copy gets a plugins/
/// directory next to the executable. Each of the next `levelsUp` ancestors
/// is then tried, and the first whose `plugins` subdirectory actually
/// holds `*Plugin*.dll` files wins. Demanding the dlls keeps an unrelated
/// ancestor "plugins" folder (an IDE's, a browser's) from hijacking the
/// search.
///
/// Returns defaultPluginsDirectory(executableDir) when nothing matched —
/// including when that directory does not exist at all, because only the
/// caller knows which log channel to report that on.
[[nodiscard]] QString resolvePluginsDirectory(
    const QString& executableDir,
    int levelsUp = kPluginsDirSearchLevels);

}  // namespace LightCpp::Plugin
