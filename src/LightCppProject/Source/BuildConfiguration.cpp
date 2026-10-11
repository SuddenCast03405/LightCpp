#include "LightCpp/Project/BuildConfiguration.hpp"

namespace LightCpp::Project {

QString toString(BuildConfiguration config)
{
    switch (config) {
    case BuildConfiguration::Debug: return QStringLiteral("Debug");
    case BuildConfiguration::Release: return QStringLiteral("Release");
    }
    return QStringLiteral("Debug");
}

std::optional<BuildConfiguration> buildConfigurationFromName(QStringView name)
{
    if (name.compare(QStringLiteral("Debug"), Qt::CaseInsensitive) == 0) {
        return BuildConfiguration::Debug;
    }
    if (name.compare(QStringLiteral("Release"), Qt::CaseInsensitive) == 0) {
        return BuildConfiguration::Release;
    }
    return std::nullopt;
}

QStringList flagsFor(BuildConfiguration config, CompilerFamily family)
{
    const bool release = (config == BuildConfiguration::Release);
    if (family == CompilerFamily::Msvc) {
        // /Od is cl's default anyway, but saying it keeps the two
        // configurations visibly symmetric — and makes the debug build
        // immune to a user-supplied optimisation flag. /Zi produces the
        // PDB cdb needs: without it "debug" on an MSVC kit starts a
        // debugger that can hit no breakpoints and show no source.
        return release ? QStringList{QStringLiteral("/O2"), QStringLiteral("/DNDEBUG")}
                       : QStringList{QStringLiteral("/Od"), QStringLiteral("/Zi")};
    }
    return release ? QStringList{QStringLiteral("-O2"), QStringLiteral("-DNDEBUG")}
                   : QStringList{QStringLiteral("-g")};
}

}  // namespace LightCpp::Project
