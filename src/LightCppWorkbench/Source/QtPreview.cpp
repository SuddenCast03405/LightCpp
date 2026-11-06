// QtPreview — implementation.

#include "LightCpp/Workbench/QtPreview.hpp"

#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace LightCpp::Workbench::Qt {
namespace {

QString escape(const QString& text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar ch : text) {
        switch (ch.unicode()) {
        case '&':  out += QLatin1String("&amp;"); break;
        case '<':  out += QLatin1String("&lt;"); break;
        case '>':  out += QLatin1String("&gt;"); break;
        case '"':  out += QLatin1String("&quot;"); break;
        default:   out += ch; break;
        }
    }
    return out;
}

const char* kStyle =
    "<style>"
    "body{font-family:sans-serif;font-size:12px;color:#ddd;background:#1e1e1e;margin:12px}"
    "h2{font-size:14px;color:#9cdcfe;margin:0 0 8px 0}"
    "h3{font-size:12px;color:#ce9178;margin:12px 0 4px 0}"
    ".canvas{position:relative;background:#252526;border:1px solid #3c3c3c;"
    "overflow:hidden}"
    ".w{position:absolute;border:1px solid #569cd6;background:#2d2d30;"
    "color:#dcdcaa;font-size:10px;padding:1px 2px;overflow:hidden;"
    "white-space:nowrap;text-overflow:ellipsis}"
    ".w.type{background:#3a2d2d;border-color:#ce9178;color:#ce9178}"
    ".missing{color:#f48771}"
    ".ok{color:#6a9955}"
    "table{border-collapse:collapse;width:100%}"
    "td,th{padding:2px 6px;border-bottom:1px solid #333;text-align:left;font-size:11px}"
    "th{color:#9cdcfe;font-weight:normal}"
    ".muted{color:#888}"
    ".badge{display:inline-block;padding:0 4px;border-radius:3px;background:#3c3c3c;"
    "color:#ddd;font-size:10px;margin-right:4px}"
    "</style>";

}  // namespace

QString uiPreviewHtml(const UiForm& form, const QString& formName)
{
    QString html = QString::fromLatin1(kStyle);
    const QString title = formName.isEmpty() ? form.className : formName;
    html += QStringLiteral("<h2>%1</h2>").arg(escape(title));
    if (!form.valid) {
        html += QStringLiteral("<p class=\"muted\">Not a valid .ui document.</p>");
        return html;
    }
    html += QStringLiteral("<p class=\"muted\">%1 widget(s), %2 connection(s)</p>")
                .arg(form.widgets.size())
                .arg(form.connections.size());

    // The canvas is as large as the form's own extent, capped so a form laid
    // out for a 4K screen still fits a pane.
    int maxX = 0;
    int maxY = 0;
    bool anyGeometry = false;
    for (const UiWidget& widget : form.widgets) {
        if (widget.width > 0 && widget.height > 0) {
            anyGeometry = true;
        }
        maxX = qMax(maxX, widget.x + widget.width);
        maxY = qMax(maxY, widget.y + widget.height);
    }
    if (anyGeometry) {
        constexpr int kMaxWidth = 900;
        constexpr int kMaxHeight = 560;
        const double scale = qMin(1.0, qMin(static_cast<double>(kMaxWidth) / qMax(maxX, 1),
                                             static_cast<double>(kMaxHeight) / qMax(maxY, 1)));
        html += QStringLiteral("<div class=\"canvas\" style=\"width:%1px;height:%2px\">")
                    .arg(static_cast<int>(maxX * scale) + 8)
                    .arg(static_cast<int>(maxY * scale) + 8);
        for (const UiWidget& widget : form.widgets) {
            if (widget.width <= 0 || widget.height <= 0) {
                continue;
            }
            const bool isContainer =
                widget.className.contains(QLatin1String("Layout"))
                || widget.className.contains(QLatin1String("Widget"))
                || widget.className.startsWith(QLatin1String("QGroup"))
                || widget.className.contains(QLatin1String("View"));
            const QString label = widget.text.isEmpty()
                ? QStringLiteral("%1<br><span class=\"muted\">%2</span>")
                      .arg(escape(widget.className), escape(widget.name))
                : QStringLiteral("%1<br><span class=\"muted\">%2</span>")
                      .arg(escape(widget.text), escape(widget.className));
            html += QStringLiteral("<div class=\"w%1\" style=\"left:%2px;top:%3px;"
                                   "width:%4px;height:%5px\" title=\"%6\">%7</div>")
                        .arg(isContainer ? QStringLiteral(" type") : QString())
                        .arg(static_cast<int>(widget.x * scale))
                        .arg(static_cast<int>(widget.y * scale))
                        .arg(qMax(2, static_cast<int>(widget.width * scale)))
                        .arg(qMax(2, static_cast<int>(widget.height * scale)))
                        .arg(escape(QStringLiteral("%1 (%2)").arg(widget.name, widget.className)),
                             label);
        }
        html += QStringLiteral("</div>");
    } else {
        // No geometry at all (a layout-only form): a list is the only honest
        // rendering.
        html += QStringLiteral("<h3>Widgets</h3><table><tr><th>Name</th><th>Class</th>"
                               "<th>Parent</th></tr>");
        for (const UiWidget& widget : form.widgets) {
            html += QStringLiteral("<tr><td>%1</td><td>%2</td><td class=\"muted\">%3</td></tr>")
                        .arg(escape(widget.name), escape(widget.className),
                             escape(widget.parent));
        }
        html += QStringLiteral("</table>");
    }

    if (!form.connections.isEmpty()) {
        html += QStringLiteral("<h3>Connections</h3><table><tr><th>Signal → slot</th></tr>");
        for (const QString& connection : form.connections) {
            html += QStringLiteral("<tr><td>%1</td></tr>").arg(escape(connection));
        }
        html += QStringLiteral("</table>");
    }
    return html;
}

QString qrcPreviewHtml(const QrcFile& qrc, const QString& qrcName)
{
    QString html = QString::fromLatin1(kStyle);
    html += QStringLiteral("<h2>%1</h2>")
                .arg(escape(qrcName.isEmpty() ? QStringLiteral("Resources") : qrcName));
    if (!qrc.valid) {
        html += QStringLiteral("<p class=\"muted\">Not a valid .qrc document.</p>");
        return html;
    }
    // Group by prefix, preserving the file's order: the prefix is what the
    // code writes as ":/prefix/name", so it is the useful grouping.
    QList<QString> prefixes;
    for (const QrcResource& resource : qrc.resources) {
        if (!prefixes.contains(resource.prefix)) {
            prefixes.append(resource.prefix);
        }
    }
    int missing = 0;
    for (const QString& prefix : prefixes) {
        html += QStringLiteral("<h3>%1</h3>")
                    .arg(escape(prefix.isEmpty() ? QStringLiteral("/") : prefix));
        html += QStringLiteral("<table><tr><th>Resource</th><th>File</th><th>Status</th></tr>");
        for (const QrcResource& resource : qrc.resources) {
            if (resource.prefix != prefix) {
                continue;
            }
            if (!resource.exists) {
                ++missing;
            }
            html += QStringLiteral("<tr><td>%1</td><td class=\"muted\">%2</td>"
                                   "<td class=\"%3\">%4</td></tr>")
                        .arg(escape(qrc.resourcePath(resource)),
                             escape(resource.absolutePath),
                             resource.exists ? QStringLiteral("ok")
                                             : QStringLiteral("missing"),
                             resource.exists ? QStringLiteral("ok")
                                             : QStringLiteral("missing"));
        }
        html += QStringLiteral("</table>");
    }
    if (qrc.resources.isEmpty()) {
        html += QStringLiteral("<p class=\"muted\">The resource file lists no files.</p>");
    } else if (missing > 0) {
        html += QStringLiteral("<p class=\"missing\">%1 file(s) are listed but not on disk — "
                               "the resource will fail to build.</p>")
                    .arg(missing);
    }
    return html;
}

QStringList qmlObjectTree(const QString& qmlText)
{
    // A QML document is a tree of `Type { ... }` blocks. A line-based reader is
    // enough for a structure view, but "pending id" has to mean *pending for
    // the next object at this depth*: the `id:` of `Item` comes after its `{`,
    // so it belongs to the object already open, not to the `Rectangle` that
    // follows it. Tracking the id on the open-object stack is the whole
    // difference between "Rectangle (id: box)" and "Rectangle (id: root)".
    static const QRegularExpression objectStart(
        QStringLiteral("^\\s*([A-Z][A-Za-z0-9_.]*)\\s*\\{"));
    static const QRegularExpression idProperty(
        QStringLiteral("^\\s*id\\s*:\\s*([A-Za-z_][A-Za-z0-9_]*)"));

    struct Frame {
        QString type;
        QString id;
        int depth = 0;
    };

    QStringList lines;
    QList<Frame> open;
    QList<qsizetype> lineIndex;  // parallel to `open`: where the name was appended

    int depth = 0;
    for (const QString& raw : qmlText.split(QLatin1Char('\n'))) {
        QString code = raw;
        // Strip a trailing comment so `Item { // root` still reads as an open.
        if (const qsizetype comment = code.indexOf(QLatin1String("//")); comment >= 0) {
            code.truncate(comment);
        }
        const QString line = code.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        const auto object = objectStart.match(code);
        if (object.hasMatch()) {
            const QString type = object.captured(1);
            lines.append(QString(static_cast<qsizetype>(depth) * 2, QLatin1Char(' ')) + type);
            open.append(Frame{type, QString(), depth});
            lineIndex.append(lines.size() - 1);
            depth += 1;
            // Anything after the `{` on the same line is still this object's.
            const qsizetype after = code.indexOf(QLatin1Char('{')) + 1;
            const QString rest = code.mid(after);
            if (const auto id = idProperty.match(rest); id.hasMatch() && !open.isEmpty()) {
                open.last().id = id.captured(1);
            }
            continue;
        }

        if (line.startsWith(QLatin1Char('}'))) {
            qsizetype closes = line.count(QLatin1Char('}'));
            while (closes > 0 && !open.isEmpty()) {
                const qsizetype index = lineIndex.takeLast();
                const Frame frame = open.takeLast();
                if (!frame.id.isEmpty()) {
                    lines[index] += QStringLiteral("  (id: %1)").arg(frame.id);
                }
                depth = frame.depth;
                --closes;
            }
            depth = qMax(0, depth);
            continue;
        }

        if (const auto id = idProperty.match(code); id.hasMatch() && !open.isEmpty()) {
            // `id:` names the object it appears in, which is the innermost open
            // one — a nested `Rectangle` has already been pushed by the time
            // its own `id:` line is read.
            open.last().id = id.captured(1);
        }
    }

    // An unclosed document still deserves to show its ids.
    while (!open.isEmpty()) {
        const qsizetype index = lineIndex.takeLast();
        const Frame frame = open.takeLast();
        if (!frame.id.isEmpty()) {
            lines[index] += QStringLiteral("  (id: %1)").arg(frame.id);
        }
    }
    return lines;
}

QString qmlStructureHtml(const QString& qmlText, const QString& fileName)
{
    QString html = QString::fromLatin1(kStyle);
    html += QStringLiteral("<h2>%1</h2>")
                .arg(escape(fileName.isEmpty() ? QStringLiteral("QML") : fileName));
    const QStringList tree = qmlObjectTree(qmlText);
    if (tree.isEmpty()) {
        html += QStringLiteral("<p class=\"muted\">No QML object found.</p>");
        return html;
    }
    html += QStringLiteral("<p class=\"muted\">Structure view — QML rendering needs a QML "
                           "engine, which this build does not link.</p>");
    html += QStringLiteral("<pre>");
    for (const QString& line : tree) {
        html += escape(line) + QLatin1Char('\n');
    }
    html += QStringLiteral("</pre>");
    return html;
}

bool hasPreview(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("ui") || suffix == QLatin1String("qrc")
           || suffix == QLatin1String("qml") || suffix == QLatin1String("qmltypes");
}

}  // namespace LightCpp::Workbench::Qt
