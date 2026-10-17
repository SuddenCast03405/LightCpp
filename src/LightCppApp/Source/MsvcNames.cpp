#include "LightCpp/App/MsvcNames.hpp"

#include <QDir>

namespace LightCpp::App::MsvcNames {
namespace {

constexpr int kScorePerMajor = 1000;

/// The marketing year of a release, keyed by the installer's major version.
/// Not a lookup table on purpose: the switch makes an unlisted major obvious,
/// and an unlisted major must fall back to the numbers rather than invent a
/// year (VS 2026 previews, and whatever comes next, would otherwise be named
/// after the wrong release).
QString yearForMajor(int major)
{
    switch (major) {
    case 18:
        return QStringLiteral("2026");
    case 17:
        return QStringLiteral("2022");
    case 16:
        return QStringLiteral("2019");
    case 15:
        return QStringLiteral("2017");
    case 14:
        return QStringLiteral("2015");
    case 12:
        return QStringLiteral("2013");
    case 11:
        return QStringLiteral("2012");
    case 10:
        return QStringLiteral("2010");
    case 9:
        return QStringLiteral("2008");
    case 8:
        return QStringLiteral("2005");
    case 7:
        return QStringLiteral("2002");
    default:
        break;
    }
    return {};
}

/// Trimmed, with the separators Qt builds paths with — the registry stores
/// `D:\Program Files (x86)` and the environment may carry a trailing space.
QString cleaned(const QString& raw)
{
    const QString trimmed = raw.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    return QDir::fromNativeSeparators(trimmed);
}

}  // namespace

QString productName(int score)
{
    if (score <= 0) {
        return QStringLiteral("Visual Studio");
    }
    const int major = score / kScorePerMajor;
    const int minor = score % kScorePerMajor;
    const QString year = yearForMajor(major);
    if (year.isEmpty()) {
        return QStringLiteral("Visual Studio %1.%2").arg(major).arg(minor);
    }
    if (minor == 0) {
        return QStringLiteral("Visual Studio %1").arg(year);
    }
    return QStringLiteral("Visual Studio %1 (%2.%3)")
        .arg(year)
        .arg(major)
        .arg(minor);
}

QString programFilesX86From(const QString& envValue,
                            const QString& registryValue,
                            const QString& systemDrive)
{
    if (const QString fromEnv = cleaned(envValue); !fromEnv.isEmpty()) {
        return fromEnv;
    }
    if (const QString fromRegistry = cleaned(registryValue);
        !fromRegistry.isEmpty()) {
        return fromRegistry;
    }
    QString drive = cleaned(systemDrive);
    if (drive.isEmpty()) {
        return {};
    }
    if (!drive.endsWith(u'/')) {
        drive += u'/';
    }
    return drive + QStringLiteral("Program Files (x86)");
}

}  // namespace LightCpp::App::MsvcNames
