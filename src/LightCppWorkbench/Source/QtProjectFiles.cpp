// QtProjectFiles — implementation. See the header for what is and is not
// evaluated.

#include "LightCpp/Workbench/QtProjectFiles.hpp"

#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
#include <QXmlStreamReader>

#include <functional>

namespace LightCpp::Workbench {
namespace {

QStringList splitValue(const QString& value)
{
    QStringList words;
    QString current;
    bool inQuotes = false;
    for (const QChar ch : value) {
        if (ch == QLatin1Char('"')) {
            inQuotes = !inQuotes;
            continue;
        }
        if (ch.isSpace() && !inQuotes) {
            if (!current.isEmpty()) {
                words << current;
                current.clear();
            }
            continue;
        }
        current.append(ch);
    }
    if (!current.isEmpty()) {
        words << current;
    }
    return words;
}

/// `$$PWD/x` and `$${PWD}/x` resolve against the project directory; anything
/// else with a `$$` is a qmake function this reader does not run.
QString expandPwd(QString value, const QString& baseDirectory, QStringList& unresolved)
{
    if (value.contains(QLatin1String("$$"))) {
        static const QRegularExpression pwdPattern(
            QStringLiteral("\\$\\$\\{?PWD\\}?"));
        if (value.contains(QLatin1String("$$PWD")) || value.contains(QLatin1String("$${PWD}"))) {
            value.replace(pwdPattern, baseDirectory);
        }
        if (value.contains(QLatin1String("$$"))) {
            unresolved << value;
            return QString();
        }
    }
    return value;
}

void appendTo(QStringList& target, const QStringList& values)
{
    for (const QString& value : values) {
        if (!value.isEmpty() && !target.contains(value)) {
            target << value;
        }
    }
}

void removeFrom(QStringList& target, const QStringList& values)
{
    for (const QString& value : values) {
        target.removeAll(value);
    }
}

}  // namespace

bool QmakeProject::usesWidgets() const
{
    return qtModules.contains(QStringLiteral("widgets"))
           || forms.size() > 0;
}

QString QmakeProject::cxxStandard() const
{
    for (const QString& option : config) {
        if (option.startsWith(QLatin1String("c++"))) {
            return option;
        }
    }
    return {};
}

QmakeProject parseQmakeProject(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QTextStream stream(&file);
    const QString text = stream.readAll();
    QmakeProject project = parseQmakeText(text, QFileInfo(path).absolutePath());
    project.path = path;
    if (project.target.isEmpty()) {
        project.target = QFileInfo(path).completeBaseName();
    }
    if (project.templateType.isEmpty()) {
        project.templateType = QStringLiteral("app");
    }
    return project;
}

QmakeProject parseQmakeText(const QString& text, const QString& baseDirectory, int depth)
{
    QmakeProject project;
    project.path = baseDirectory;
    if (depth > 8) {
        return project;  // include cycle or absurd nesting
    }

    // Join continuation lines: qmake ends a statement with '\' + newline.
    QStringList logical;
    QString pending;
    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        QString line = raw;
        const qsizetype comment = line.indexOf(QLatin1Char('#'));
        if (comment >= 0) {
            line = line.left(comment);
        }
        line = line.trimmed();
        if (line.endsWith(QLatin1Char('\\'))) {
            pending += line.left(line.size() - 1) + QLatin1Char(' ');
            continue;
        }
        if (!pending.isEmpty()) {
            line = pending + line;
            pending.clear();
        }
        if (!line.isEmpty()) {
            logical << line;
        }
    }
    if (!pending.isEmpty()) {
        logical << pending;
    }

    static const QRegularExpression assignment(
        QStringLiteral("^([A-Za-z_][A-Za-z0-9_.]*)\\s*(\\+=|-=|\\*=|=(?!=))\\s*(.*)$"));

    for (const QString& line : logical) {
        // include(other.pri) / include(other.pri, "", true)
        static const QRegularExpression includePattern(
            QStringLiteral("^include\\s*\\(\\s*([^,)]+)"));
        if (const auto match = includePattern.match(line); match.hasMatch()) {
            const QString relative = match.captured(1).trimmed().remove(QLatin1Char('"'));
            const QString resolved = QFileInfo(QDir(baseDirectory).filePath(relative)).absoluteFilePath();
            project.includes << resolved;
            QFile included(resolved);
            if (included.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream stream(&included);
                const QmakeProject nested =
                    parseQmakeText(stream.readAll(), QFileInfo(resolved).absolutePath(), depth + 1);
                appendTo(project.sources, nested.sources);
                appendTo(project.headers, nested.headers);
                appendTo(project.forms, nested.forms);
                appendTo(project.resources, nested.resources);
                appendTo(project.includePaths, nested.includePaths);
                appendTo(project.defines, nested.defines);
                appendTo(project.libraries, nested.libraries);
                appendTo(project.qtModules, nested.qtModules);
                appendTo(project.config, nested.config);
                project.unresolvedExpressions += nested.unresolvedExpressions;
            }
            continue;
        }

        const auto match = assignment.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        const QString name = match.captured(1);
        const QString op = match.captured(2);
        QString value = expandPwd(match.captured(3).trimmed(), baseDirectory,
                                  project.unresolvedExpressions);
        if (value.isEmpty() && !match.captured(3).trimmed().isEmpty()) {
            continue;  // an expression this reader cannot evaluate
        }
        QStringList words = splitValue(value);
        // A bare "+=" with only a value keeps the first word; qmake takes the
        // whole rest for TARGET and TEMPLATE, which are single-valued.
        const bool singleValued = name == QLatin1String("TARGET")
                                  || name == QLatin1String("TEMPLATE")
                                  || name == QLatin1String("VERSION");

        if (name == QLatin1String("TARGET") && !words.isEmpty()) {
            project.target = words.first();
            continue;
        }
        if (name == QLatin1String("TEMPLATE")) {
            project.templateType = words.value(0).toLower();
            continue;
        }
        if (name == QLatin1String("SUBDIRS")) {
            if (op == QLatin1String("-=")) {
                removeFrom(project.subdirs, words);
            } else {
                appendTo(project.subdirs, words);
            }
            continue;
        }

        QStringList* target = nullptr;
        if (name == QLatin1String("SOURCES"))        target = &project.sources;
        else if (name == QLatin1String("HEADERS"))   target = &project.headers;
        else if (name == QLatin1String("FORMS"))     target = &project.forms;
        else if (name == QLatin1String("RESOURCES")) target = &project.resources;
        else if (name == QLatin1String("INCLUDEPATH")) target = &project.includePaths;
        else if (name == QLatin1String("DEFINES"))   target = &project.defines;
        else if (name == QLatin1String("LIBS"))      target = &project.libraries;
        else if (name == QLatin1String("QT"))        target = &project.qtModules;
        else if (name == QLatin1String("CONFIG"))    target = &project.config;
        if (target == nullptr) {
            continue;
        }
        if (op == QLatin1String("-=")) {
            removeFrom(*target, words);
        } else if (op == QLatin1String("*=")) {
            QStringList kept;
            const QString pattern = words.value(0);
            QRegularExpression expression(
                QRegularExpression::wildcardToRegularExpression(pattern));
            for (const QString& entry : std::as_const(*target)) {
                if (expression.match(entry).hasMatch()) {
                    kept << entry;
                }
            }
            *target = kept;
        } else if (op == QLatin1String("=") && !singleValued) {
            *target = words;
        } else {
            appendTo(*target, words);
        }
    }

    project.valid = true;
    return project;
}

// ---------------------------------------------------------------------------
// .ui
// ---------------------------------------------------------------------------

QList<UiWidget> UiForm::ofClass(const QString& className) const
{
    QList<UiWidget> out;
    for (const UiWidget& widget : widgets) {
        if (widget.className == className) {
            out << widget;
        }
    }
    return out;
}

UiForm parseUiForm(const QString& xml)
{
    UiForm form;
    QDomDocument document;
    if (!document.setContent(xml)) {
        return form;
    }
    const QDomElement root = document.documentElement();
    if (root.tagName() != QLatin1String("ui")) {
        return form;
    }
    form.className = root.firstChildElement(QStringLiteral("class")).text();

    // The form's own widget is the <widget class="QMainWindow" name="...">.
    std::function<void(const QDomElement&, const QString&, QList<UiWidget>&)> readWidget;
    readWidget = [&readWidget](const QDomElement& element, const QString& parent,
                               QList<UiWidget>& out) {
        UiWidget widget;
        widget.className = element.attribute(QStringLiteral("class"));
        widget.name = element.attribute(QStringLiteral("name"));
        widget.parent = parent;
        // <property name="geometry"><rect><x>0</x>...</rect></property>
        for (QDomElement property = element.firstChildElement(QStringLiteral("property"));
             !property.isNull();
             property = property.nextSiblingElement(QStringLiteral("property"))) {
            const QString propertyName = property.attribute(QStringLiteral("name"));
            if (propertyName == QLatin1String("geometry")) {
                const QDomElement rect = property.firstChildElement(QStringLiteral("rect"));
                widget.x = rect.firstChildElement(QStringLiteral("x")).text().toInt();
                widget.y = rect.firstChildElement(QStringLiteral("y")).text().toInt();
                widget.width = rect.firstChildElement(QStringLiteral("width")).text().toInt();
                widget.height = rect.firstChildElement(QStringLiteral("height")).text().toInt();
            } else if (propertyName == QLatin1String("text")) {
                widget.text = property.firstChildElement(QStringLiteral("string")).text();
            }
        }
        if (widget.name.isEmpty() && widget.className.isEmpty()) {
            return;
        }
        out << widget;
        for (QDomElement child = element.firstChildElement(QStringLiteral("widget"));
             !child.isNull();
             child = child.nextSiblingElement(QStringLiteral("widget"))) {
            readWidget(child, widget.name, out);
        }
    };

    for (QDomElement widget = root.firstChildElement(QStringLiteral("widget"));
         !widget.isNull();
         widget = widget.nextSiblingElement(QStringLiteral("widget"))) {
        readWidget(widget, QString(), form.widgets);
    }

    for (QDomElement connection = root.firstChildElement(QStringLiteral("connections"))
                                      .firstChildElement(QStringLiteral("connection"));
         !connection.isNull();
         connection = connection.nextSiblingElement(QStringLiteral("connection"))) {
        const QString sender = connection.firstChildElement(QStringLiteral("sender")).text();
        const QString signal = connection.firstChildElement(QStringLiteral("signal")).text();
        const QString receiver = connection.firstChildElement(QStringLiteral("receiver")).text();
        const QString slot = connection.firstChildElement(QStringLiteral("slot")).text();
        form.connections << QStringLiteral("%1.%2 -> %3.%4")
                                .arg(sender, signal, receiver, slot);
    }
    form.valid = !form.className.isEmpty() || !form.widgets.isEmpty();
    return form;
}

// ---------------------------------------------------------------------------
// .qrc
// ---------------------------------------------------------------------------

QString QrcFile::resourcePath(const QrcResource& resource) const
{
    QString prefix = resource.prefix;
    if (!prefix.startsWith(QLatin1Char('/'))) {
        prefix.prepend(QLatin1Char('/'));
    }
    if (prefix.endsWith(QLatin1Char('/'))) {
        prefix.chop(1);
    }
    const QString name = resource.alias.isEmpty()
                             ? QFileInfo(resource.path).fileName()
                             : resource.alias;
    return QStringLiteral(":") + prefix + QLatin1Char('/') + name;
}

QrcFile parseQrc(const QString& xml, const QString& qrcPath)
{
    QrcFile file;
    QDomDocument document;
    if (!document.setContent(xml)) {
        return file;
    }
    const QDomElement root = document.documentElement();
    if (root.tagName() != QLatin1String("RCC")) {
        return file;
    }
    const QDir base = QFileInfo(qrcPath).absoluteDir();
    for (QDomElement group = root.firstChildElement(QStringLiteral("qresource"));
         !group.isNull();
         group = group.nextSiblingElement(QStringLiteral("qresource"))) {
        const QString prefix = group.attribute(QStringLiteral("prefix"));
        for (QDomElement entry = group.firstChildElement(QStringLiteral("file"));
             !entry.isNull();
             entry = entry.nextSiblingElement(QStringLiteral("file"))) {
            QrcResource resource;
            resource.prefix = prefix;
            resource.alias = entry.attribute(QStringLiteral("alias"));
            resource.path = entry.text().trimmed();
            resource.absolutePath = QFileInfo(base.filePath(resource.path)).absoluteFilePath();
            resource.exists = QFileInfo::exists(resource.absolutePath);
            file.resources << resource;
        }
    }
    file.valid = true;
    return file;
}

// ---------------------------------------------------------------------------
// QML
// ---------------------------------------------------------------------------

QStringList qmlKeywords()
{
    return {
        QStringLiteral("as"), QStringLiteral("break"), QStringLiteral("case"),
        QStringLiteral("catch"), QStringLiteral("continue"), QStringLiteral("default"),
        QStringLiteral("delete"), QStringLiteral("do"), QStringLiteral("else"),
        QStringLiteral("finally"), QStringLiteral("for"), QStringLiteral("function"),
        QStringLiteral("if"), QStringLiteral("import"), QStringLiteral("in"),
        QStringLiteral("instanceof"), QStringLiteral("new"), QStringLiteral("on"),
        QStringLiteral("property"), QStringLiteral("readonly"), QStringLiteral("required"),
        QStringLiteral("return"), QStringLiteral("signal"), QStringLiteral("switch"),
        QStringLiteral("throw"), QStringLiteral("try"), QStringLiteral("typeof"),
        QStringLiteral("var"), QStringLiteral("void"), QStringLiteral("while"),
        QStringLiteral("with"), QStringLiteral("pragma"),
    };
}

QStringList qmlBuiltinTypes()
{
    return {
        QStringLiteral("bool"), QStringLiteral("color"), QStringLiteral("date"),
        QStringLiteral("double"), QStringLiteral("enumeration"), QStringLiteral("font"),
        QStringLiteral("int"), QStringLiteral("list"), QStringLiteral("matrix4x4"),
        QStringLiteral("point"), QStringLiteral("quaternion"), QStringLiteral("real"),
        QStringLiteral("rect"), QStringLiteral("size"), QStringLiteral("string"),
        QStringLiteral("time"), QStringLiteral("url"), QStringLiteral("var"),
        QStringLiteral("vector2d"), QStringLiteral("vector3d"), QStringLiteral("vector4d"),
        QStringLiteral("Item"), QStringLiteral("Rectangle"), QStringLiteral("Text"),
        QStringLiteral("Image"), QStringLiteral("MouseArea"), QStringLiteral("Component"),
        QStringLiteral("Loader"), QStringLiteral("ListView"), QStringLiteral("Repeater"),
        QStringLiteral("Window"), QStringLiteral("ApplicationWindow"),
        QStringLiteral("Connections"), QStringLiteral("Timer"), QStringLiteral("QtObject"),
    };
}

bool isQtProjectFile(const QString& path)
{
    const QString lower = path.toLower();
    return lower.endsWith(QLatin1String(".pro")) || lower.endsWith(QLatin1String(".pri"))
           || lower.endsWith(QLatin1String(".ui")) || lower.endsWith(QLatin1String(".qrc"))
           || lower.endsWith(QLatin1String(".qml"));
}

QStringList qmakeArguments(const QString& projectFile,
                           const QString& buildDirectory,
                           const QString& configuration)
{
    // Run qmake *in* the build directory so the generated Makefile does not
    // land next to the sources (shadow build), which is the only layout where
    // a Debug and a Release build can coexist.
    QStringList arguments;
    arguments << projectFile;
    if (configuration.compare(QLatin1String("Debug"), Qt::CaseInsensitive) == 0) {
        arguments << QStringLiteral("CONFIG+=debug")
                  << QStringLiteral("CONFIG-=release");
    } else if (configuration.compare(QLatin1String("Release"), Qt::CaseInsensitive) == 0) {
        arguments << QStringLiteral("CONFIG+=release")
                  << QStringLiteral("CONFIG-=debug");
    }
    if (!buildDirectory.isEmpty()) {
        arguments << QStringLiteral("-o")
                  << QDir(buildDirectory).filePath(QStringLiteral("Makefile"));
    }
    return arguments;
}

}  // namespace LightCpp::Workbench
