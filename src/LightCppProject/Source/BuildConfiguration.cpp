#include "LightCpp/Project/BuildConfiguration.hpp"

namespace LightCpp::Project {

QString toString(BuildConfiguration config)
{
    return config == BuildConfiguration::Release
               ? QStringLiteral("Release")
               : QStringLiteral("Debug");
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
        // immune to a user-supplied optimisation flag.
        return release ? QStringList{QStringLiteral("/O2"), QStringLiteral("/DNDEBUG")}
                       : QStringList{QStringLiteral("/Od")};
    }
    return release ? QStringList{QStringLiteral("-O2"), QStringLiteral("-DNDEBUG")}
                   : QStringList{QStringLiteral("-g")};
}

}  // namespace LightCpp::Project
