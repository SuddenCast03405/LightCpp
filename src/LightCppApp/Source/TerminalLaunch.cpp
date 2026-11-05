#include "LightCpp/App/TerminalLaunch.hpp"

namespace LightCpp::App::TerminalLaunch {

QString programCommandLine(const QString& exePath, bool powerShellStyle,
                           const QStringList& arguments)
{
    QStringList words;
    words.reserve(arguments.size() + 1);
    if (powerShellStyle) {
        // Inside single quotes PowerShell takes the text literally, so the
        // only escape needed is the quote itself, doubled. The call operator
        // is what makes a quoted path run instead of merely evaluate to a
        // string.
        const auto quote = [](QString text) {
            text.replace(QLatin1Char('\''), QStringLiteral("''"));
            return QStringLiteral("'%1'").arg(text);
        };
        words.append(QStringLiteral("& ") + quote(exePath));
        for (const QString& argument : arguments) {
            words.append(quote(argument));
        }
        return words.join(QLatin1Char(' '));
    }
#ifdef Q_OS_WIN
    // cmd.exe: double quotes group each word. A double quote cannot appear in
    // a Windows file name, but run arguments may carry one — cmd has no
    // escape for it, so the best that can be done is doubling ("" stays one
    // word and most C runtimes fold it back to a single quote).
    const auto quote = [](QString text) {
        text.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QStringLiteral("\"%1\"").arg(text);
    };
#else
    // POSIX shells use single quotes; embedded apostrophes are represented
    // by closing the quote, escaping the apostrophe, then reopening it.
    const auto quote = [](QString text) {
        text.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
        return QLatin1Char('\'') + text + QLatin1Char('\'');
    };
#endif
    words.append(quote(exePath));
    for (const QString& argument : arguments) {
        words.append(quote(argument));
    }
    return words.join(QLatin1Char(' '));
}

}  // namespace LightCpp::App::TerminalLaunch
