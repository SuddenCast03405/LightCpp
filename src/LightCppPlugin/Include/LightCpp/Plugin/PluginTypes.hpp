// PluginTypes — value types shared between the IDE and its plugins.
//
// They deliberately live in the plugin framework (not in LightCppProject or
// LightCppApp) so a plugin DLL never has to link more than LightCppPlugin and
// Qt Core to talk to the IDE.

#pragma once

#include <QString>
#include <QStringList>

namespace LightCpp::Plugin {

/// One external command in a project build — the plugin-side twin of the
/// MainWindow's ProjectStep. A build is a short queue of these, run one after
/// another, because an early step failing makes the later ones meaningless.
struct BuildStep {
    QString program;
    QStringList arguments;
    QString workingDirectory;
    QString label;  // echoed into the Build Log before the step runs
};

/// What a project backend reports after opening a project descriptor.
///
/// The full project model stays inside the plugin (it parsed the file, it
/// knows what the fields mean); the IDE only sees this summary plus the build
/// steps the backend produces on request. That keeps the IDE free of any
/// concrete project format.
struct ProjectSummary {
    QString filePath;         // the descriptor itself, absolute
    QString projectDir;       // the project's root directory
    QString name;             // display name, for the window title
    QString targetLabel;      // what it builds, e.g. "Console Application"
    QString outputPath;       // the binary the build will produce
    int unitCount = 0;        // units listed in the file
    int buildUnitCount = 0;   // units that actually take part in the build
    bool producesExecutable = true;  // false for static/dynamic libraries
    QStringList runArguments; // arguments passed when running the result
};

/// Everything a backend needs to turn the open project into build steps.
///
/// The IDE resolves tool paths and the selected configuration; the backend
/// decides how they are used. Configuration is passed as the canonical name
/// ("Debug" / "Release") so the framework does not depend on the project
/// library's enum.
struct BuildRequest {
    QString configuration;     // canonical name, e.g. "Debug"
    QString compiler;          // the kit's C++ compiler (GNU family projects)
    QString make;              // the kit's make, for custom-makefile projects
    QStringList extraMakeArgs; // kit build-tool args appended to the make step
};

}  // namespace LightCpp::Plugin
