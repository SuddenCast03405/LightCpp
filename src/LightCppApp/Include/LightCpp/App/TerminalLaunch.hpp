// TerminalLaunch — the line typed into an embedded terminal to start a program.
//
// The embedded terminal is a shell launched from a configured template
// ("cmd.exe /k", "pwsh -NoLogo"), and a run is started by *typing the
// program's path into that shell*: the path is shell input, so the shell
// parses it. An unquoted path with a space is then two words — cmd answers
// "'C:\My' is not recognized as an internal or external command" — and the
// program never starts while the IDE reports the run as begun.
//
// PowerShell-style templates are launched without the value their -Command/-c
// last token expects, so the path needs the call operator there: a bare
// quoted path is a string expression, not a command.

#pragma once

#include <QString>

namespace LightCpp::App::TerminalLaunch {

/// The line to type into the shell to start `exePath`.
///
/// `powerShellStyle` selects the spelling the shell accepts: PowerShell takes
/// `& '<path>'` (with single quotes inside doubled); every other shell here
/// (cmd.exe) takes the double-quoted path, which is one word to it and keeps
/// spaces, `&`, `^` and `|` inside the name from being read as syntax.
[[nodiscard]] QString programCommandLine(const QString& exePath, bool powerShellStyle);

}  // namespace LightCpp::App::TerminalLaunch
