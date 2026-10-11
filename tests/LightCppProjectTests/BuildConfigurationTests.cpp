// BuildConfigurationTests — the Debug/Release flag tables.

#include "LightCpp/Project/BuildConfiguration.hpp"

#include <gtest/gtest.h>

namespace {

using LightCpp::Project::buildConfigurationFromName;
using LightCpp::Project::BuildConfiguration;
using LightCpp::Project::CompilerFamily;
using LightCpp::Project::flagsFor;
using LightCpp::Project::toString;

TEST(BuildConfiguration, CanonicalNamesRoundTrip)
{
    EXPECT_EQ(toString(BuildConfiguration::Debug), QStringLiteral("Debug"));
    EXPECT_EQ(toString(BuildConfiguration::Release), QStringLiteral("Release"));

    // The same string is handed to CMake as CMAKE_BUILD_TYPE, so it has to be
    // spelled the way CMake spells it.
    ASSERT_TRUE(buildConfigurationFromName(toString(BuildConfiguration::Release)).has_value());
    EXPECT_EQ(*buildConfigurationFromName(toString(BuildConfiguration::Release)),
              BuildConfiguration::Release);
}

TEST(BuildConfiguration, ParsingIsCaseInsensitiveAndRejectsJunk)
{
    EXPECT_EQ(*buildConfigurationFromName(QStringLiteral("debug")),
              BuildConfiguration::Debug);
    EXPECT_EQ(*buildConfigurationFromName(QStringLiteral("RELEASE")),
              BuildConfiguration::Release);

    // An empty or unknown value must not silently become Debug: the caller
    // gets to decide what its default is.
    EXPECT_FALSE(buildConfigurationFromName(QStringLiteral("")).has_value());
    EXPECT_FALSE(buildConfigurationFromName(QStringLiteral("RelWithDebInfo")).has_value());
    EXPECT_FALSE(buildConfigurationFromName(QStringLiteral("Debug ")).has_value());
}

TEST(BuildConfiguration, GnuDebugOnlyAddsDebugInfo)
{
    EXPECT_EQ(flagsFor(BuildConfiguration::Debug, CompilerFamily::Gnu),
              QStringList({QStringLiteral("-g")}));
}

TEST(BuildConfiguration, GnuReleaseOptimisesAndDefinesNdebug)
{
    EXPECT_EQ(flagsFor(BuildConfiguration::Release, CompilerFamily::Gnu),
              QStringList({QStringLiteral("-O2"), QStringLiteral("-DNDEBUG")}));
}

TEST(BuildConfiguration, MsvcDebugTurnsOptimisationOffAndEmitsPdb)
{
    const QStringList flags = flagsFor(BuildConfiguration::Debug, CompilerFamily::Msvc);
    EXPECT_EQ(flags, QStringList({QStringLiteral("/Od"), QStringLiteral("/Zi")}));

    // Deliberate: the IDE offers cdb debugging on MSVC kits, and cdb can
    // only hit breakpoints with a PDB — a Debug build without /Zi starts a
    // debugger that shows nothing. (Historically /Zi was pinned OFF here;
    // the audit flipped the contract, and /Z7 alone does not help because
    // the linker still writes the .pdb.)
    EXPECT_TRUE(flags.join(QLatin1Char(' ')).contains(QStringLiteral("/Zi")));
    EXPECT_FALSE(flags.join(QLatin1Char(' ')).contains(QStringLiteral("/Z7")));
}

TEST(BuildConfiguration, MsvcReleaseOptimisesAndDefinesNdebug)
{
    EXPECT_EQ(flagsFor(BuildConfiguration::Release, CompilerFamily::Msvc),
              QStringList({QStringLiteral("/O2"), QStringLiteral("/DNDEBUG")}));
}

TEST(BuildConfiguration, NdebugIsNeverDefinedInDebug)
{
    for (const CompilerFamily family : {CompilerFamily::Gnu, CompilerFamily::Msvc}) {
        const QString all = flagsFor(BuildConfiguration::Debug, family).join(QLatin1Char(' '));
        EXPECT_FALSE(all.contains(QStringLiteral("NDEBUG"))) << "family " << int(family);
        EXPECT_FALSE(all.contains(QStringLiteral("O2"))) << "family " << int(family);
    }
}

}  // namespace
