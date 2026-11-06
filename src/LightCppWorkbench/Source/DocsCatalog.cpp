// DocsCatalog — implementation.

#include "LightCpp/Workbench/DocsCatalog.hpp"

#include <QHash>
#include <QUrl>

namespace LightCpp::Workbench::Docs {
namespace {

struct Keyword {
    const char* name;
    const char* summary;
};

/// One sentence per keyword. Written to be right without being a tutorial.
const Keyword kKeywords[] = {
    {"alignas", "Specifies the alignment requirement of a type or object."},
    {"alignof", "Yields the alignment requirement of a type."},
    {"asm", "Embeds implementation-defined assembly in the translation unit."},
    {"auto", "Deduces the type of a variable from its initialiser."},
    {"bool", "The boolean type, with the values true and false."},
    {"break", "Leaves the innermost switch, loop or range-for statement."},
    {"case", "Labels a branch of a switch statement."},
    {"catch", "Handles an exception thrown by a try block."},
    {"char", "A character type large enough for the execution character set."},
    {"char8_t", "A UTF-8 code unit (C++20)."},
    {"char16_t", "A UTF-16 code unit."},
    {"char32_t", "A UTF-32 code unit."},
    {"class", "Declares a class type, whose members are private by default."},
    {"concept", "Declares a named constraint on template parameters (C++20)."},
    {"const", "Makes an object or member function non-modifiable."},
    {"consteval", "A function that must be evaluated at compile time (C++20)."},
    {"constexpr", "An expression, variable or function usable in a constant expression."},
    {"constinit", "Guarantees static initialisation of a variable (C++20)."},
    {"continue", "Jumps to the next iteration of the innermost loop."},
    {"co_await", "Suspends a coroutine until the awaited value is ready (C++20)."},
    {"co_return", "Returns a value from a coroutine (C++20)."},
    {"co_yield", "Produces a value from a coroutine and suspends it (C++20)."},
    {"decltype", "Yields the declared type of an expression."},
    {"default", "The fallback branch of a switch, or a defaulted special member."},
    {"delete", "Deletes an object, or deletes a function from overload resolution."},
    {"do", "Starts a loop whose condition is checked after the body."},
    {"double", "A double-precision floating-point type."},
    {"dynamic_cast", "A checked cast across a polymorphic class hierarchy."},
    {"else", "The alternative branch of an if statement."},
    {"enum", "Declares an enumeration type."},
    {"explicit", "Prevents a constructor or conversion operator from being used implicitly."},
    {"export", "Exports a declaration from a module (C++20)."},
    {"extern", "Gives a declaration external linkage, or names a language linkage."},
    {"false", "The false value of type bool."},
    {"float", "A single-precision floating-point type."},
    {"for", "A counted loop, or a range-for over a sequence."},
    {"friend", "Grants a function or class access to private members."},
    {"goto", "Jumps to a label in the same function."},
    {"if", "A conditional statement, optionally with an initialiser (C++17)."},
    {"inline", "Allows a function or variable to be defined in multiple translation units."},
    {"int", "The natural integer type of the target."},
    {"long", "An integer type at least as wide as int."},
    {"mutable", "Allows a member to be modified in a const member function."},
    {"namespace", "Groups names into a scope."},
    {"new", "Allocates an object with dynamic storage duration."},
    {"noexcept", "States that a function does not throw, and makes throwing terminate."},
    {"nullptr", "The null pointer literal."},
    {"operator", "Declares an overloaded operator or a conversion function."},
    {"override", "States that a virtual function overrides a base-class function."},
    {"private", "Makes members accessible only to the class and its friends."},
    {"protected", "Makes members accessible to the class, its friends and derived classes."},
    {"public", "Makes members accessible to everyone."},
    {"register", "A removed storage-class hint; an error since C++17."},
    {"reinterpret_cast", "Reinterprets the bits of one type as another."},
    {"requires", "Constrains a template, or introduces a requires-expression (C++20)."},
    {"return", "Leaves a function and yields its result."},
    {"short", "An integer type no wider than int."},
    {"signed", "An explicitly signed integer type."},
    {"sizeof", "Yields the size in bytes of a type or expression."},
    {"static", "Gives a variable static storage duration, or a member class-level scope."},
    {"static_assert", "Fails compilation when a constant expression is false."},
    {"static_cast", "A checked, non-polymorphic cast."},
    {"struct", "Declares a class type whose members are public by default."},
    {"switch", "Selects a branch by the value of an integral expression."},
    {"template", "Declares a class, function or variable template."},
    {"this", "A pointer to the object a member function was called on."},
    {"thread_local", "Gives a variable one instance per thread."},
    {"throw", "Throws an exception."},
    {"true", "The true value of type bool."},
    {"try", "Introduces a block whose exceptions are handled by catch clauses."},
    {"typedef", "Introduces a type alias with C syntax."},
    {"typeid", "Yields the runtime type information of a polymorphic expression."},
    {"typename", "Names a dependent type in a template."},
    {"union", "Declares a type whose members share storage."},
    {"unsigned", "An explicitly unsigned integer type."},
    {"using", "Introduces a type alias, or brings names into a scope."},
    {"virtual", "Enables dynamic dispatch for a member function."},
    {"void", "The type of a function that returns nothing, or of an untyped pointer."},
    {"volatile", "Marks an object whose value may change outside the program's control."},
    {"wchar_t", "A wide character type, wide enough for any supported character set."},
    {"while", "A loop whose condition is checked before the body."},
};

const QHash<QString, const char*>& keywordTable()
{
    static const QHash<QString, const char*> table = [] {
        QHash<QString, const char*> map;
        for (const Keyword& keyword : kKeywords) {
            map.insert(QString::fromLatin1(keyword.name),
                       keyword.summary);
        }
        return map;
    }();
    return table;
}

/// Standard library names whose cppreference page is at a stable URL. The key
/// is the name without "std::"; the value is the path below /w/cpp/.
const QHash<QString, const char*>& stdPages()
{
    static const QHash<QString, const char*> pages = {
        {QStringLiteral("vector"), "container/vector"},
        {QStringLiteral("array"), "container/array"},
        {QStringLiteral("deque"), "container/deque"},
        {QStringLiteral("list"), "container/list"},
        {QStringLiteral("map"), "container/map"},
        {QStringLiteral("multimap"), "container/multimap"},
        {QStringLiteral("set"), "container/set"},
        {QStringLiteral("multiset"), "container/multiset"},
        {QStringLiteral("unordered_map"), "container/unordered_map"},
        {QStringLiteral("unordered_set"), "container/unordered_set"},
        {QStringLiteral("stack"), "container/stack"},
        {QStringLiteral("queue"), "container/queue"},
        {QStringLiteral("priority_queue"), "container/priority_queue"},
        {QStringLiteral("string"), "string/basic_string"},
        {QStringLiteral("string_view"), "string/basic_string_view"},
        {QStringLiteral("span"), "container/span"},
        {QStringLiteral("optional"), "utility/optional"},
        {QStringLiteral("variant"), "utility/variant"},
        {QStringLiteral("any"), "utility/any"},
        {QStringLiteral("tuple"), "utility/tuple"},
        {QStringLiteral("pair"), "utility/pair"},
        {QStringLiteral("unique_ptr"), "memory/unique_ptr"},
        {QStringLiteral("shared_ptr"), "memory/shared_ptr"},
        {QStringLiteral("weak_ptr"), "memory/weak_ptr"},
        {QStringLiteral("make_unique"), "memory/unique_ptr/make_unique"},
        {QStringLiteral("make_shared"), "memory/shared_ptr/make_shared"},
        {QStringLiteral("function"), "utility/functional/function"},
        {QStringLiteral("move"), "utility/move"},
        {QStringLiteral("forward"), "utility/forward"},
        {QStringLiteral("sort"), "algorithm/sort"},
        {QStringLiteral("find"), "algorithm/find"},
        {QStringLiteral("transform"), "algorithm/transform"},
        {QStringLiteral("accumulate"), "algorithm/accumulate"},
        {QStringLiteral("thread"), "thread/thread"},
        {QStringLiteral("mutex"), "thread/mutex"},
        {QStringLiteral("atomic"), "atomic/atomic"},
        {QStringLiteral("future"), "thread/future"},
        {QStringLiteral("async"), "thread/async"},
        {QStringLiteral("chrono"), "chrono"},
        {QStringLiteral("filesystem"), "filesystem"},
        {QStringLiteral("regex"), "regex"},
        {QStringLiteral("iostream"), "io/iostream"},
        {QStringLiteral("ifstream"), "io/basic_ifstream"},
        {QStringLiteral("ofstream"), "io/basic_ofstream"},
        {QStringLiteral("stringstream"), "io/basic_stringstream"},
        {QStringLiteral("exception"), "error/exception"},
        {QStringLiteral("runtime_error"), "error/runtime_error"},
        {QStringLiteral("size_t"), "types/size_t"},
        {QStringLiteral("ptrdiff_t"), "types/ptrdiff_t"},
        {QStringLiteral("nullptr_t"), "types/nullptr_t"},
        {QStringLiteral("numeric_limits"), "types/numeric_limits"},
        {QStringLiteral("is_same"), "types/is_same"},
        {QStringLiteral("enable_if"), "types/enable_if"},
        {QStringLiteral("declval"), "utility/declval"},
        {QStringLiteral("initializer_list"), "utility/initializer_list"},
        {QStringLiteral("byte"), "types/byte"},
        {QStringLiteral("bit_cast"), "numeric/bit_cast"},
        {QStringLiteral("format"), "utility/format"},
        {QStringLiteral("expected"), "utility/expected"},
        {QStringLiteral("source_location"), "utility/source_location"},
    };
    return pages;
}

QString stripStd(const QString& symbol)
{
    QString name = symbol.trimmed();
    // `std::vector<int>::push_back` → `vector`; `vector::push_back` → `vector`.
    if (name.startsWith(QLatin1String("std::"))) {
        name = name.mid(5);
    }
    const qsizetype templateOpen = name.indexOf(QLatin1Char('<'));
    if (templateOpen > 0) {
        name = name.left(templateOpen);
    }
    const qsizetype scope = name.indexOf(QLatin1String("::"));
    if (scope > 0) {
        name = name.left(scope);
    }
    return name.trimmed();
}

}  // namespace

QString cppreferenceUrl(const QString& symbol)
{
    const QString name = stripStd(symbol);
    const auto page = stdPages().constFind(name);
    if (page != stdPages().constEnd()) {
        return QStringLiteral("https://en.cppreference.com/w/cpp/%1")
            .arg(QString::fromLatin1(page.value()));
    }
    // `std::` names that are not in the table, and everything else, get the
    // site's own search: an exact-but-wrong page is worse than a search.
    return QStringLiteral("https://en.cppreference.com/mwiki/index.php?search=%1")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(symbol.trimmed())));
}

bool isKnownKeyword(const QString& word)
{
    return keywordTable().contains(word.trimmed());
}

QString keywordSummary(const QString& word)
{
    const auto it = keywordTable().constFind(word.trimmed());
    return it == keywordTable().constEnd() ? QString()
                                           : QString::fromLatin1(it.value());
}

QStringList keywords()
{
    QStringList list;
    for (const Keyword& keyword : kKeywords) {
        list.append(QString::fromLatin1(keyword.name));
    }
    list.sort();
    return list;
}

DocEntry quickHelp(const QString& symbol)
{
    DocEntry entry;
    const QString trimmed = symbol.trimmed();
    if (trimmed.isEmpty()) {
        return entry;
    }
    entry.kind = QStringLiteral("keyword");
    entry.title = trimmed;
    const QString summary = keywordSummary(trimmed);
    if (!summary.isEmpty()) {
        entry.summary = summary;
        entry.url = QStringLiteral("https://en.cppreference.com/w/cpp/keyword/%1")
                        .arg(trimmed);
        return entry;
    }
    const QString name = stripStd(trimmed);
    if (stdPages().contains(name)) {
        entry.kind = QStringLiteral("library");
        entry.title = QStringLiteral("std::%1").arg(name);
        entry.summary = QStringLiteral("C++ standard library: %1.").arg(name);
        entry.url = cppreferenceUrl(trimmed);
        return entry;
    }
    entry.summary.clear();
    entry.url = cppreferenceUrl(trimmed);
    return entry;
}

QList<DocEntry> standardDocuments()
{
    // The standard drafts are the authoritative free copies. The working
    // draft is the newest, and the numbered ones are what a project pins to.
    return {
        {QStringLiteral("C++ working draft (latest)"),
         QStringLiteral("https://eel.is/c++draft/"),
         QStringLiteral("The current working draft of the C++ standard."),
         QStringLiteral("standard")},
        {QStringLiteral("C++23 (N4950)"),
         QStringLiteral("https://timsong-cpp.github.io/cppwp/n4950/"),
         QStringLiteral("Committee draft of C++23."),
         QStringLiteral("standard")},
        {QStringLiteral("C++20 (N4861)"),
         QStringLiteral("https://timsong-cpp.github.io/cppwp/n4861/"),
         QStringLiteral("Committee draft of C++20."),
         QStringLiteral("standard")},
        {QStringLiteral("C++17 (N4659)"),
         QStringLiteral("https://timsong-cpp.github.io/cppwp/n4659/"),
         QStringLiteral("Committee draft of C++17."),
         QStringLiteral("standard")},
        {QStringLiteral("C standard (C17)"),
         QStringLiteral("https://www.iso.org/standard/74528.html"),
         QStringLiteral("ISO/IEC 9899:2018 — the C17 standard."),
         QStringLiteral("standard")},
        {QStringLiteral("C++ Core Guidelines"),
         QStringLiteral("https://isocpp.github.io/CppCoreGuidelines/"),
         QStringLiteral("The community guidelines for modern C++."),
         QStringLiteral("standard")},
        {QStringLiteral("cppreference"),
         QStringLiteral("https://en.cppreference.com/w/"),
         QStringLiteral("The reference wiki for C and C++."),
         QStringLiteral("standard")},
    };
}

QList<DocEntry> libraryDocuments()
{
    return {
        {QStringLiteral("Qt 6"),
         QStringLiteral("https://doc.qt.io/qt-6/"),
         QStringLiteral("Qt 6 reference — widgets, QML, core, network."),
         QStringLiteral("library")},
        {QStringLiteral("Boost"),
         QStringLiteral("https://www.boost.org/doc/libs/"),
         QStringLiteral("Boost library documentation, per release."),
         QStringLiteral("library")},
        {QStringLiteral("{fmt}"),
         QStringLiteral("https://fmt.dev/latest/"),
         QStringLiteral("A modern formatting library; the basis of std::format."),
         QStringLiteral("library")},
        {QStringLiteral("nlohmann/json"),
         QStringLiteral("https://json.nlohmann.me/"),
         QStringLiteral("Single-header JSON for modern C++."),
         QStringLiteral("library")},
        {QStringLiteral("GoogleTest"),
         QStringLiteral("https://google.github.io/googletest/"),
         QStringLiteral("Google's C++ test framework."),
         QStringLiteral("library")},
        {QStringLiteral("CMake"),
         QStringLiteral("https://cmake.org/cmake/help/latest/"),
         QStringLiteral("CMake command and variable reference."),
         QStringLiteral("library")},
        {QStringLiteral("clangd"),
         QStringLiteral("https://clangd.llvm.org/"),
         QStringLiteral("The clangd language server and its configuration."),
         QStringLiteral("library")},
    };
}

QList<DocEntry> pluginDocuments(const QStringList& pluginIds)
{
    QList<DocEntry> entries;
    for (const QString& id : pluginIds) {
        DocEntry entry;
        entry.title = id;
        // The repository's own reference; the anchor is the plugin id.
        entry.url = QStringLiteral("docs/PLUGINS.md#%1").arg(id);
        entry.summary = QStringLiteral("Reference page for the %1 plugin.").arg(id);
        entry.kind = QStringLiteral("plugin");
        entries.append(entry);
    }
    return entries;
}

/// One catalogue entry per `std::` name the page table knows, so that typing
/// "vector" or "unique_ptr" into the doc browser finds the reference page and
/// not just the (much smaller) keyword list.
QList<DocEntry> standardLibraryPages()
{
    QList<DocEntry> entries;
    for (auto it = stdPages().constBegin(); it != stdPages().constEnd(); ++it) {
        DocEntry entry;
        entry.title = QStringLiteral("std::%1").arg(it.key());
        entry.url = QStringLiteral("https://en.cppreference.com/w/cpp/%1")
                        .arg(QString::fromLatin1(it.value()));
        entry.summary = QStringLiteral("C++ standard library: %1.").arg(it.key());
        entry.kind = QStringLiteral("library");
        entries.append(entry);
    }
    return entries;
}

QList<DocEntry> search(const QList<DocEntry>& libraries, const QString& query)
{
    const QString needle = query.trimmed();
    if (needle.isEmpty()) {
        QList<DocEntry> index = standardDocuments();
        index += libraries.isEmpty() ? libraryDocuments() : libraries;
        return index;
    }
    QList<DocEntry> hits;
    for (const QString& keyword : keywords()) {
        const QString summary = keywordSummary(keyword);
        if (keyword.contains(needle, Qt::CaseInsensitive)
            || summary.contains(needle, Qt::CaseInsensitive)) {
            DocEntry entry;
            entry.title = keyword;
            entry.summary = summary;
            entry.kind = QStringLiteral("keyword");
            entry.url = QStringLiteral("https://en.cppreference.com/w/cpp/keyword/%1")
                            .arg(keyword);
            hits.append(entry);
        }
    }
    const QList<DocEntry> docs = libraries.isEmpty() ? libraryDocuments() : libraries;
    for (const DocEntry& doc : docs) {
        if (doc.title.contains(needle, Qt::CaseInsensitive)
            || doc.summary.contains(needle, Qt::CaseInsensitive)) {
            hits.append(doc);
        }
    }
    for (const DocEntry& doc : standardLibraryPages()) {
        if (doc.title.contains(needle, Qt::CaseInsensitive)
            || doc.summary.contains(needle, Qt::CaseInsensitive)) {
            hits.append(doc);
        }
    }
    return hits;
}

}  // namespace LightCpp::Workbench::Docs
