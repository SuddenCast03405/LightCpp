#include "LightCpp/App/TerminalLaunch.hpp"

namespace LightCpp::App::TerminalLaunch {

QString programCommandLine(const QString& exePath, bool powerShellStyle)
{
    if (powerShellStyle) {
        // Inside single quotes PowerShell takes the text literally, so the
        // only escape needed is the quote itself, doubled. The call operator
        // is what makes a quoted path run instead of merely evaluate to a
        // string.
        QString quoted = exePath;
        quoted.replace(QLatin1Char('\''), QStringLiteral("''"));
        return QStringLiteral("& '%1'").arg(quoted);
    }
    // cmd.exe: double quotes group the whole path into one word. A double
    // quote cannot appear in a Windows file name, so there is nothing to
    // escape — but note that cmd still expands %NAME% inside the quotes, the
    // same way it did before this quoting was added.
    return QStringLiteral("\"%1\"").arg(exePath);
}

}  // namespace LightCpp::App::TerminalLaunch
