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
#ifdef Q_OS_WIN
    // cmd.exe: double quotes group the whole path into one word. A double
    // quote cannot appear in a Windows file name.
    return QStringLiteral("\"%1\"").arg(exePath);
#else
    // POSIX shells use single quotes; embedded apostrophes are represented
    // by closing the quote, escaping the apostrophe, then reopening it.
    QString quoted = exePath;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'") + quoted + QLatin1Char('\'');
#endif
}

}  // namespace LightCpp::App::TerminalLaunch
