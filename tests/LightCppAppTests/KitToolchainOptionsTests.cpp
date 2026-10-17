// LightCppApp tests - what the Kit page's Toolchain drop-down offers (R2-32).
//
// The bug: a non-editable QComboBox refuses setCurrentText() for a value it
// does not offer. currentIndex stays -1, currentText() answers "", and the kit
// form committed that "" back into kits.json — a project kit inheriting a
// global toolchain lost the reference, and with it every compiler.

#include "LightCpp/App/KitToolchainOptions.hpp"

#include <gtest/gtest.h>

#include <QStringList>

namespace {

using LightCpp::App::KitToolchainOption;
using LightCpp::App::KitToolchainOptions;
using LightCpp::App::kitToolchainOptionsFor;

/// The stored values, in drop-down order.
QStringList valuesOf(const KitToolchainOptions& options)
{
    QStringList out;
    for (const KitToolchainOption& item : options.items) {
        out.append(item.value);
    }
    return out;
}

/// The text the user reads for the selected row.
QString selectedText(const KitToolchainOptions& options)
{
    EXPECT_GE(options.current, 0);
    EXPECT_LT(options.current, options.items.size());
    if (options.current < 0 || options.current >= options.items.size()) {
        return QString();
    }
    return options.items[options.current].text;
}

}  // namespace

TEST(KitToolchainOptions, AKnownIdIsOfferedAndSelected)
{
    const KitToolchainOptions options =
        kitToolchainOptionsFor({QStringLiteral("gcc"), QStringLiteral("llvm")},
                               QStringLiteral("llvm"));
    ASSERT_EQ(options.items.size(), 3);  // two toolchains + "inherits nothing"
    EXPECT_EQ(valuesOf(options), QStringList({QStringLiteral("gcc"),
                                              QStringLiteral("llvm"),
                                              QString()}));
    EXPECT_EQ(options.current, 1);
    EXPECT_EQ(selectedText(options), QStringLiteral("llvm"));
}

TEST(KitToolchainOptions, AnEmptyIdSelectsTheNoneRow)
{
    // A kit that does not inherit names no toolchain. The box used to be
    // blank at index -1, which looks exactly like "the id I picked is gone".
    const KitToolchainOptions options =
        kitToolchainOptionsFor({QStringLiteral("gcc")}, QString());
    const int noneIndex = options.items.size() - 1;
    EXPECT_EQ(options.current, noneIndex);
    EXPECT_TRUE(options.items[noneIndex].value.isEmpty());
    EXPECT_FALSE(options.items[noneIndex].missing);
}

TEST(KitToolchainOptions, AnUnknownIdIsKeptAndMarkedMissing)
{
    // This is the bug itself: the id is not in the list, so the box showed
    // nothing and the commit wrote the blank back. It now gets its own row.
    const KitToolchainOptions options =
        kitToolchainOptionsFor({QStringLiteral("gcc")}, QStringLiteral("clang"));
    EXPECT_EQ(valuesOf(options), QStringList({QStringLiteral("gcc"),
                                              QStringLiteral("clang"),
                                              QString()}));
    EXPECT_EQ(options.current, 1);
    EXPECT_TRUE(options.items[1].missing);
    // The user sees the id plus a word saying it is not defined; the kit keeps
    // the id itself, so saving an untouched form is a no-op.
    EXPECT_EQ(selectedText(options), QStringLiteral("clang (missing)"));
    EXPECT_EQ(options.items[1].value, QStringLiteral("clang"));
    EXPECT_FALSE(options.items[0].missing);
}

TEST(KitToolchainOptions, AnUnknownIdSurvivesAnEmptySettingsLayer)
{
    // The project layer usually defines no toolchain of its own. That made
    // the old list empty, setCurrentText("") and the id vanished on Apply.
    const KitToolchainOptions options = kitToolchainOptionsFor({}, QStringLiteral("mingw"));
    ASSERT_EQ(options.items.size(), 2);
    EXPECT_EQ(options.items[0].value, QStringLiteral("mingw"));
    EXPECT_TRUE(options.items[0].missing);
    EXPECT_EQ(options.current, 0);
}

TEST(KitToolchainOptions, OnlyTheNoneRowWhenThereIsNothingAtAll)
{
    const KitToolchainOptions options = kitToolchainOptionsFor({}, QString());
    ASSERT_EQ(options.items.size(), 1);
    EXPECT_TRUE(options.items[0].value.isEmpty());
    EXPECT_EQ(options.current, 0);
}

TEST(KitToolchainOptions, ADuplicateIdIsOfferedOnce)
{
    // Two rows with the same id are two ways to answer the same question,
    // and the second one silently wins on a re-select.
    const KitToolchainOptions options =
        kitToolchainOptionsFor({QStringLiteral("gcc"), QStringLiteral("gcc")},
                               QStringLiteral("gcc"));
    EXPECT_EQ(valuesOf(options), QStringList({QStringLiteral("gcc"), QString()}));
    EXPECT_EQ(options.current, 0);
}

TEST(KitToolchainOptions, IdsWithoutNamesAreIgnored)
{
    // A toolchain with no id cannot be referenced, so it must not become a
    // blank row the user can select.
    const KitToolchainOptions options =
        kitToolchainOptionsFor({QString(), QStringLiteral("  "), QStringLiteral("gcc")},
                               QString());
    EXPECT_EQ(valuesOf(options), QStringList({QStringLiteral("gcc"), QString()}));
}

TEST(KitToolchainOptions, TheNoneRowIsAlwaysLastAndSomethingIsAlwaysSelected)
{
    const QList<QStringList> idLists = {
        {}, {QStringLiteral("a")}, {QStringLiteral("a"), QStringLiteral("b")}};
    const QList<QString> storedIds = {QString(), QStringLiteral("a"),
                                      QStringLiteral("b"), QStringLiteral("gone")};
    for (const QStringList& ids : idLists) {
        for (const QString& stored : storedIds) {
            const KitToolchainOptions options = kitToolchainOptionsFor(ids, stored);
            ASSERT_FALSE(options.items.isEmpty());
            EXPECT_TRUE(options.items.last().value.isEmpty());
            EXPECT_GE(options.current, 0);
            EXPECT_LT(options.current, options.items.size());
            // Whatever is selected carries the stored id: the commit can
            // always write back what the kit already said.
            EXPECT_EQ(options.items[options.current].value, stored.trimmed())
                << ids.join(QLatin1Char(',')).toStdString() << " / "
                << stored.toStdString();
        }
    }
}
