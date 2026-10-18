// The VS7 registry key: read what is in it, not what we assumed is in it.
//
// From Visual Studio 2002 to 2019 the installs are *values* of
// HKLM\SOFTWARE\Microsoft\VisualStudio\SxS\VS7, named by version:
//
//     reg query HKLM\SOFTWARE\Wow6432Node\Microsoft\VisualStudio\SxS\VS7
//     15.0    REG_SZ    C:\Program Files (x86)\Microsoft Visual Studio\2017\Community
//
// (that path has no trailing backslash on purpose: inside a `//` comment it
// would swallow the next line, and this build treats the warning as an error)
//
// The code asked QSettings for childGroups() — subkeys — so it came back with
// nothing on every machine that has one of those Visual Studios, and the only
// way an MSVC toolchain was ever found was vswhere (VS2017+).
//
// The registry cannot be faked for a test; a QSettings file can, which is why
// the reading lives in installRootsIn() and takes the settings object.

#include "LightCpp/App/MsvcToolchain.hpp"

#include <QMap>
#include <QSettings>
#include <QTemporaryDir>

#include <gtest/gtest.h>

namespace {

const char kGroup[] = "VS7";

void writeValues(const QString& path,
                 const QList<QPair<QString, QString>>& values)
{
    QSettings settings(path, QSettings::IniFormat);
    settings.beginGroup(QString::fromLatin1(kGroup));
    for (const QPair<QString, QString>& value : values) {
        settings.setValue(value.first, value.second);
    }
    settings.endGroup();
    settings.sync();
}

QList<QPair<QString, QString>> readRoots(const QString& path)
{
    QSettings settings(path, QSettings::IniFormat);
    settings.beginGroup(QString::fromLatin1(kGroup));
    const QList<QPair<QString, QString>> roots =
        LightCpp::App::Msvc::installRootsIn(settings);
    settings.endGroup();
    return roots;
}

QMap<QString, QString> asMap(const QList<QPair<QString, QString>>& roots)
{
    QMap<QString, QString> out;
    for (const QPair<QString, QString>& root : roots) {
        out.insert(root.first, root.second);
    }
    return out;
}

}  // namespace

TEST(MsvcRegistry, TheValuesOfTheKeyAreTheInstalls)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("vs7.ini"));
    writeValues(path, {{QStringLiteral("15.0"),
                        QStringLiteral("C:/Program Files (x86)/VS/2017/Community")},
                       {QStringLiteral("16.0"),
                        QStringLiteral("C:/Program Files (x86)/VS/2019/Community")}});

    const QMap<QString, QString> roots = asMap(readRoots(path));
    ASSERT_EQ(roots.size(), 2);
    EXPECT_EQ(roots.value(QStringLiteral("15.0")),
              QStringLiteral("C:/Program Files (x86)/VS/2017/Community"));
    EXPECT_EQ(roots.value(QStringLiteral("16.0")),
              QStringLiteral("C:/Program Files (x86)/VS/2019/Community"));
}

TEST(MsvcRegistry, AnEmptyValueIsNotAnInstall)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("vs7.ini"));
    writeValues(path, {{QStringLiteral("15.0"), QString()},
                       {QStringLiteral("16.0"), QStringLiteral("   ")}});

    EXPECT_TRUE(readRoots(path).isEmpty());
}

TEST(MsvcRegistry, AVersionWithSurroundingSpaceIsTrimmed)
{
    // The registry hands back the string as written, and " C:\..." would then
    // be probed as a relative path.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("vs7.ini"));
    writeValues(path, {{QStringLiteral("14.0"),
                        QStringLiteral("  C:/VS/2015/  ")}});

    const QList<QPair<QString, QString>> roots = readRoots(path);
    ASSERT_EQ(roots.size(), 1);
    EXPECT_EQ(roots.first().second, QStringLiteral("C:/VS/2015/"));
}

TEST(MsvcRegistry, ASubkeyWhoseDefaultValueIsThePathIsAlsoRead)
{
    // The shape the old code assumed. Reading it costs one call, and nothing
    // that comes out of here is trusted anyway — every candidate is probed for
    // cl.exe before it is used.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("vs7.ini"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.beginGroup(QString::fromLatin1(kGroup));
        settings.beginGroup(QStringLiteral("12.0"));
        settings.setValue(QStringLiteral("."), QStringLiteral("C:/VS/2013"));
        settings.endGroup();
        settings.endGroup();
        settings.sync();
    }

    const QMap<QString, QString> roots = asMap(readRoots(path));
    EXPECT_EQ(roots.value(QStringLiteral("12.0")), QStringLiteral("C:/VS/2013"));
}

TEST(MsvcRegistry, AKeyThatHoldsNothingYieldsNothing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("vs7.ini"));
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.beginGroup(QStringLiteral("SomethingElse"));
        settings.setValue(QStringLiteral("15.0"), QStringLiteral("C:/not/this/key"));
        settings.endGroup();
        settings.sync();
    }

    EXPECT_TRUE(readRoots(path).isEmpty());
}
