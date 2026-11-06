// DebugSymbols — implementation. See the header for why each command exists.

#include "LightCpp/Workbench/DebugSymbols.hpp"

namespace LightCpp::Workbench::Debug {

QString backendName(Backend backend)
{
    switch (backend) {
    case Backend::Gdb:  return QStringLiteral("gdb");
    case Backend::Lldb: return QStringLiteral("lldb");
    case Backend::Cdb:  return QStringLiteral("cdb");
    case Backend::Unknown: break;
    }
    return QStringLiteral("unknown");
}

Backend backendFromName(const QString& name)
{
    const QString lower = name.trimmed().toLower();
    if (lower == QLatin1String("gdb") || lower == QLatin1String("gdb-mi")) {
        return Backend::Gdb;
    }
    if (lower == QLatin1String("lldb")) {
        return Backend::Lldb;
    }
    if (lower == QLatin1String("cdb") || lower == QLatin1String("windbg")
        || lower == QLatin1String("msvc")) {
        return Backend::Cdb;
    }
    return Backend::Unknown;
}

QString quoteArgument(const QString& argument)
{
    if (argument.isEmpty()) {
        return QStringLiteral("\"\"");
    }
    const bool needsQuotes = argument.contains(QLatin1Char(' '))
                             || argument.contains(QLatin1Char('\t'))
                             || argument.contains(QLatin1Char('"'));
    if (!needsQuotes) {
        return argument;
    }
    QString escaped = argument;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString substitutePathCommand(const PathMapping& mapping)
{
    return QStringLiteral("set substitute-path %1 %2")
        .arg(quoteArgument(mapping.from), quoteArgument(mapping.to));
}

QStringList prettyPrintCommands(Backend backend, bool enabled)
{
    switch (backend) {
    case Backend::Gdb:
        // MI, not console: `-enable-pretty-printing` is a GDB/MI command and
        // is answered with ^done, whereas `enable-pretty-printing` (console)
        // is not understood by the MI interpreter at all.
        return {QStringLiteral("-enable-pretty-printing")};
    case Backend::Lldb:
        // lldb's synthetic children (the vector/summary providers) are on by
        // default; the setting that turns them off is per-target.
        return {QStringLiteral("settings set target.enable-synthetic-value %1")
                    .arg(enabled ? QStringLiteral("true") : QStringLiteral("false"))};
    case Backend::Cdb:
    case Backend::Unknown:
        break;
    }
    return {};
}

QStringList enumDisplayCommands(Backend backend, bool enabled)
{
    switch (backend) {
    case Backend::Gdb:
        // gdb 13+ has `set print enum`; older ones print the enumerator name
        // whenever the type is known, so the second command is the one that
        // matters on those and is harmless on the new ones.
        return {
            QStringLiteral("set print enum %1")
                .arg(enabled ? QStringLiteral("on") : QStringLiteral("off")),
            QStringLiteral("set print pretty %1")
                .arg(enabled ? QStringLiteral("on") : QStringLiteral("off")),
        };
    case Backend::Lldb:
        return {QStringLiteral("settings set target.enable-synthetic-value %1")
                    .arg(enabled ? QStringLiteral("true") : QStringLiteral("false"))};
    case Backend::Cdb:
        // cdb prints enumerators by name through natvis; there is no switch
        // that suppresses it, so nothing is sent rather than something wrong.
        break;
    case Backend::Unknown:
        break;
    }
    return {};
}

QStringList dynamicTypeCommands(Backend backend, bool enabled)
{
    switch (backend) {
    case Backend::Gdb:
        return {QStringLiteral("set print object %1")
                    .arg(enabled ? QStringLiteral("on") : QStringLiteral("off"))};
    case Backend::Lldb:
        return {QStringLiteral("settings set target.prefer-dynamic-value %1")
                    .arg(enabled ? QStringLiteral("run-target")
                                 : QStringLiteral("no-dynamic-values"))};
    case Backend::Cdb:
    case Backend::Unknown:
        break;
    }
    return {};
}

QStringList sourceMappingCommands(Backend backend,
                                  const QList<PathMapping>& mappings)
{
    QStringList commands;
    int count = 0;
    for (const PathMapping& mapping : mappings) {
        if (!mapping.isValid()) {
            continue;
        }
        if (++count > SymbolSettings::kMaxMappings) {
            break;
        }
        switch (backend) {
        case Backend::Gdb:
            commands.append(substitutePathCommand(mapping));
            break;
        case Backend::Lldb:
            // One rule per command: lldb's source-map is a single ordered list
            // and a second call replaces the first, so it must be given as one
            // pair per invocation (lldb accepts repeated FROM TO pairs, but
            // only in one command — keep one pair per call for clarity).
            commands.append(QStringLiteral("settings set target.source-map %1 %2")
                                .arg(quoteArgument(mapping.from),
                                     quoteArgument(mapping.to)));
            break;
        case Backend::Cdb:
            // cdb's .srcpath takes a single search path, not a mapping; the
            // destination is what matters, and the debugger finds the file
            // under it by name.
            commands.append(QStringLiteral(".srcpath+ %1")
                                .arg(quoteArgument(mapping.to)));
            break;
        case Backend::Unknown:
            break;
        }
    }
    return commands;
}

QStringList symbolPathCommands(Backend backend, const QStringList& paths)
{
    QStringList valid;
    for (const QString& path : paths) {
        if (!path.trimmed().isEmpty()) {
            valid.append(path);
        }
        if (valid.size() >= SymbolSettings::kMaxPaths) {
            break;
        }
    }
    if (valid.isEmpty()) {
        return {};
    }
    switch (backend) {
    case Backend::Gdb: {
        // gdb separates its search path with the platform's own separator:
        // ':' everywhere except Windows, where ';' is required because a
        // drive letter already contains a colon.
#ifdef Q_OS_WIN
        const QString separator = QStringLiteral(";");
#else
        const QString separator = QStringLiteral(":");
#endif
        return {
            QStringLiteral("set debug-file-directory %1")
                .arg(quoteArgument(valid.join(separator))),
            QStringLiteral("set solib-search-path %1")
                .arg(quoteArgument(valid.join(separator))),
        };
    }
    case Backend::Lldb:
        return {
            QStringLiteral("settings set target.debug-file-search-paths %1")
                .arg(quoteArgument(valid.join(QLatin1Char(' ')))),
            QStringLiteral("settings set target.exec-search-paths %1")
                .arg(quoteArgument(valid.join(QLatin1Char(' ')))),
        };
    case Backend::Cdb:
        return {QStringLiteral(".sympath+ %1")
                    .arg(quoteArgument(valid.join(QLatin1Char(';'))))};
    case Backend::Unknown:
        break;
    }
    return {};
}

QStringList natvisCommands(Backend backend, const QStringList& files)
{
    if (backend != Backend::Cdb) {
        // natvis is a Microsoft debugger format. Sending `.nvload` to gdb is
        // not "unsupported, ignored" — it is a syntax error in the console
        // the user is reading, so it is not sent at all.
        return {};
    }
    QStringList commands;
    for (const QString& file : files) {
        if (!file.trimmed().isEmpty()) {
            commands.append(QStringLiteral(".nvload %1").arg(quoteArgument(file)));
        }
    }
    return commands;
}

QStringList loadSymbolCommands(Backend backend, const QStringList& files)
{
    QStringList commands;
    for (const QString& file : files) {
        const QString trimmed = file.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        switch (backend) {
        case Backend::Gdb:
            commands.append(QStringLiteral("add-symbol-file %1")
                                .arg(quoteArgument(trimmed)));
            break;
        case Backend::Lldb:
            commands.append(QStringLiteral("target symbols add %1")
                                .arg(quoteArgument(trimmed)));
            break;
        case Backend::Cdb:
            commands.append(QStringLiteral(".reload /f %1")
                                .arg(quoteArgument(trimmed)));
            break;
        case Backend::Unknown:
            break;
        }
    }
    return commands;
}

QStringList startupCommands(Backend backend, const SymbolSettings& settings)
{
    QStringList commands;
    commands.append(sourceMappingCommands(backend, settings.sourceMappings));
    commands.append(symbolPathCommands(backend, settings.symbolSearchPaths));
    commands.append(natvisCommands(backend, settings.natvisFiles));
    commands.append(loadSymbolCommands(backend, settings.symbolFiles));
    if (settings.prettyPrinting) {
        commands.append(prettyPrintCommands(backend, true));
    }
    if (settings.showEnumerators) {
        commands.append(enumDisplayCommands(backend, true));
    }
    if (settings.dynamicType) {
        commands.append(dynamicTypeCommands(backend, true));
    }
    return commands;
}

QString describe(const SymbolSettings& settings)
{
    QStringList parts;
    if (settings.prettyPrinting) {
        parts.append(QStringLiteral("pretty-printing"));
    }
    if (settings.showEnumerators) {
        parts.append(QStringLiteral("enumerators by name"));
    }
    if (settings.dynamicType) {
        parts.append(QStringLiteral("dynamic types"));
    }
    if (!settings.sourceMappings.isEmpty()) {
        parts.append(QStringLiteral("%1 source mapping(s)")
                         .arg(settings.sourceMappings.size()));
    }
    if (!settings.symbolSearchPaths.isEmpty()) {
        parts.append(QStringLiteral("%1 symbol path(s)")
                         .arg(settings.symbolSearchPaths.size()));
    }
    if (!settings.natvisFiles.isEmpty()) {
        parts.append(QStringLiteral("%1 natvis file(s)")
                         .arg(settings.natvisFiles.size()));
    }
    if (parts.isEmpty()) {
        return QStringLiteral("symbol layer off — variables print in raw form");
    }
    return parts.join(QStringLiteral(", "));
}

}  // namespace LightCpp::Workbench::Debug
