// LspModel — implementation. See the header for the shapes this pins down.

#include "LightCpp/Workbench/LspModel.hpp"

#include <QMap>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace LightCpp::Workbench::Lsp {
namespace {

/// Guard against a hostile/broken answer: the IDE would happily allocate a
/// token per entry, and a corrupted `data` array of a hundred million integers
/// is a hang, not a highlight.
constexpr int kMaxSemanticTokens = 4 * 1024 * 1024;

QString stringOf(const QJsonObject& object, const char* key)
{
    return object.value(QLatin1String(key)).toString();
}

int intOf(const QJsonObject& object, const char* key, int fallback = 0)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isDouble() ? value.toInt(fallback) : fallback;
}

/// The `detail` of a class from clangd reads "class Foo : public Bar, private
/// Baz". The base list is the part after the first colon that is not part of
/// "::".
///
/// The subtlety: "class ns::Thing" has a colon but no base list. Scanning left
/// to right and skipping "::" is not enough — after skipping the "::" of
/// "ns::Thing" the search restarts *after* it and can never come back, so the
/// "namespace-colon" and the "base-list colon" look the same. A base-list colon
/// is therefore one that is neither preceded nor followed by another colon,
/// which is exactly what the C++ grammar allows (a scope qualifier is always a
/// colon pair).
QString baseClassesFromDetail(const QString& detail)
{
    for (qsizetype index = 0; index < detail.size(); ++index) {
        if (detail.at(index) != QLatin1Char(':')) {
            continue;
        }
        const bool partOfScopeBefore = index > 0 && detail.at(index - 1) == QLatin1Char(':');
        const bool partOfScopeAfter =
            index + 1 < detail.size() && detail.at(index + 1) == QLatin1Char(':');
        if (partOfScopeBefore || partOfScopeAfter) {
            continue;
        }
        return detail.mid(index + 1).trimmed();
    }
    return {};
}

/// Recursively append the members of one DocumentSymbol node.
void collectMembers(const QJsonArray& children, QList<ClassMember>* out)
{
    for (const QJsonValue& child : children) {
        const QJsonObject object = child.toObject();
        ClassMember member;
        member.name = stringOf(object, "name");
        member.detail = stringOf(object, "detail");
        member.kind = intOf(object, "kind");
        const Range selection = parseRange(object.value(QLatin1String("selectionRange")));
        member.line = selection.startLine;
        member.column = selection.startColumn;
        if (!member.name.isEmpty()) {
            out->append(member);
        }
        // A nested class shows up as a member of its enclosing one, with its
        // own members flattened in after it.
        collectMembers(object.value(QLatin1String("children")).toArray(), out);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Positions
// ---------------------------------------------------------------------------

Range parseRange(const QJsonValue& value)
{
    Range range;
    const QJsonObject object = value.toObject();
    if (object.isEmpty()) {
        return range;
    }
    const QJsonObject start = object.value(QLatin1String("start")).toObject();
    const QJsonObject end = object.value(QLatin1String("end")).toObject();
    range.startLine = intOf(start, "line");
    range.startColumn = intOf(start, "character");
    range.endLine = intOf(end, "line");
    range.endColumn = intOf(end, "character");
    return range;
}

QString pathFromUri(const QString& uri)
{
    if (uri.isEmpty()) {
        return {};
    }
    const QUrl url(uri);
    if (url.isLocalFile()) {
        return url.toLocalFile();
    }
    // Some servers send a bare path; keep it as-is rather than inventing a
    // URL scheme.
    return uri;
}

// ---------------------------------------------------------------------------
// Semantic tokens
// ---------------------------------------------------------------------------

QString SemanticLegend::typeName(int index) const
{
    if (index < 0 || index >= tokenTypes.size()) {
        return {};
    }
    return tokenTypes.at(index);
}

int SemanticLegend::modifierBit(const QString& name) const
{
    const int index = tokenModifiers.indexOf(name);
    return index < 0 ? -1 : index;
}

bool SemanticLegend::hasModifier(int bits, const QString& name) const
{
    const int bit = modifierBit(name);
    if (bit < 0 || bit >= 31) {
        return false;
    }
    return (bits & (1 << bit)) != 0;
}

QStringList SemanticLegend::modifierNames(int bits) const
{
    QStringList names;
    const int count = qMin(tokenModifiers.size(), 31);
    for (int bit = 0; bit < count; ++bit) {
        if ((bits & (1 << bit)) != 0) {
            names.append(tokenModifiers.at(bit));
        }
    }
    return names;
}

SemanticLegend parseLegend(const QJsonObject& initializeResult)
{
    SemanticLegend legend;
    const QJsonObject capabilities =
        initializeResult.value(QLatin1String("capabilities")).toObject();
    const QJsonObject provider =
        capabilities.value(QLatin1String("semanticTokensProvider")).toObject();
    if (provider.isEmpty()) {
        // clangd also uses the older "semanticHighlighting" capability name
        // for its own extension; the standard one is what we read, and a
        // server that only has the old one simply gets no semantic layer.
        return legend;
    }
    const QJsonObject serverLegend =
        provider.value(QLatin1String("legend")).toObject();
    for (const QJsonValue& value :
         serverLegend.value(QLatin1String("tokenTypes")).toArray()) {
        legend.tokenTypes.append(value.toString());
    }
    for (const QJsonValue& value :
         serverLegend.value(QLatin1String("tokenModifiers")).toArray()) {
        legend.tokenModifiers.append(value.toString());
    }
    return legend;
}

QList<SemanticToken> decodeSemanticTokens(const QJsonArray& data, QString* error)
{
    if (error != nullptr) {
        error->clear();
    }
    QList<SemanticToken> tokens;
    if (data.isEmpty()) {
        return tokens;  // a document with nothing to highlight is not an error
    }
    if (data.size() % 5 != 0) {
        if (error != nullptr) {
            *error = QStringLiteral("semantic tokens: %1 integers is not a multiple of 5")
                         .arg(data.size());
        }
        return tokens;
    }
    const int count = data.size() / 5;
    if (count > kMaxSemanticTokens) {
        if (error != nullptr) {
            *error = QStringLiteral("semantic tokens: %1 tokens is more than the editor accepts")
                         .arg(count);
        }
        return tokens;
    }
    tokens.reserve(count);
    int line = 0;
    int column = 0;
    for (int i = 0; i < data.size(); i += 5) {
        const int deltaLine = data.at(i).toInt();
        const int deltaStart = data.at(i + 1).toInt();
        const int length = data.at(i + 2).toInt();
        const int type = data.at(i + 3).toInt();
        const int modifiers = data.at(i + 4).toInt();
        if (deltaLine < 0 || length <= 0 || type < 0 || modifiers < 0
            || (deltaLine == 0 && deltaStart < 0)) {
            // A negative delta cannot be repaired: every token after it would
            // be at a wrong position. Drop the whole answer.
            if (error != nullptr) {
                *error = QStringLiteral(
                             "semantic tokens: invalid delta at entry %1 "
                             "(line %2, start %3, length %4)")
                             .arg(i / 5)
                             .arg(deltaLine)
                             .arg(deltaStart)
                             .arg(length);
            }
            tokens.clear();
            return tokens;
        }
        if (deltaLine > 0) {
            line += deltaLine;
            column = deltaStart;  // absolute on a new line
        } else {
            column += deltaStart;
        }
        SemanticToken token;
        token.line = line;
        token.column = column;
        token.length = length;
        token.type = type;
        token.modifiers = modifiers;
        tokens.append(token);
    }
    return tokens;
}

TokenRole roleFor(const QString& typeName)
{
    // The standard LSP token types, plus clangd's own additions. A type the
    // palette does not know gets `Unknown`, which paints nothing — the safe
    // default, because a wrong colour is worse than the base syntax colour.
    static const QMap<QString, TokenRole> roles = {
        {QStringLiteral("namespace"), TokenRole::Namespace},
        {QStringLiteral("type"), TokenRole::Type},
        {QStringLiteral("class"), TokenRole::Type},
        {QStringLiteral("enum"), TokenRole::Type},
        {QStringLiteral("interface"), TokenRole::Type},
        {QStringLiteral("struct"), TokenRole::Type},
        {QStringLiteral("union"), TokenRole::Type},
        {QStringLiteral("concept"), TokenRole::Type},
        {QStringLiteral("typeParameter"), TokenRole::TypeParameter},
        {QStringLiteral("parameter"), TokenRole::Parameter},
        {QStringLiteral("variable"), TokenRole::Variable},
        {QStringLiteral("property"), TokenRole::Property},
        {QStringLiteral("enumMember"), TokenRole::EnumMember},
        {QStringLiteral("function"), TokenRole::Function},
        {QStringLiteral("method"), TokenRole::Function},
        {QStringLiteral("macro"), TokenRole::Macro},
        {QStringLiteral("keyword"), TokenRole::Keyword},
        {QStringLiteral("modifier"), TokenRole::Modifier},
        {QStringLiteral("comment"), TokenRole::Comment},
        {QStringLiteral("string"), TokenRole::String},
        {QStringLiteral("number"), TokenRole::Number},
        {QStringLiteral("regexp"), TokenRole::Regexp},
        {QStringLiteral("operator"), TokenRole::Operator},
        {QStringLiteral("decorator"), TokenRole::Decorator},
        {QStringLiteral("bracket"), TokenRole::Bracket},
        {QStringLiteral("label"), TokenRole::Label},
        {QStringLiteral("event"), TokenRole::Event},
        {QStringLiteral("unknown"), TokenRole::Unknown},
    };
    return roles.value(typeName, TokenRole::Unknown);
}

QString roleName(TokenRole role)
{
    switch (role) {
    case TokenRole::Unknown:       return QStringLiteral("unknown");
    case TokenRole::Namespace:     return QStringLiteral("namespace");
    case TokenRole::Type:          return QStringLiteral("type");
    case TokenRole::TypeParameter: return QStringLiteral("type parameter");
    case TokenRole::Parameter:     return QStringLiteral("parameter");
    case TokenRole::Variable:      return QStringLiteral("variable");
    case TokenRole::Property:      return QStringLiteral("property");
    case TokenRole::EnumMember:    return QStringLiteral("enum member");
    case TokenRole::Function:      return QStringLiteral("function");
    case TokenRole::Macro:         return QStringLiteral("macro");
    case TokenRole::Keyword:       return QStringLiteral("keyword");
    case TokenRole::Modifier:      return QStringLiteral("modifier");
    case TokenRole::Comment:       return QStringLiteral("comment");
    case TokenRole::String:        return QStringLiteral("string");
    case TokenRole::Number:        return QStringLiteral("number");
    case TokenRole::Regexp:        return QStringLiteral("regexp");
    case TokenRole::Operator:      return QStringLiteral("operator");
    case TokenRole::Decorator:     return QStringLiteral("decorator");
    case TokenRole::Bracket:       return QStringLiteral("bracket");
    case TokenRole::Label:         return QStringLiteral("label");
    case TokenRole::Event:         return QStringLiteral("event");
    }
    return QStringLiteral("unknown");
}

TokenSnapshot mergeSnapshots(const TokenSnapshot& older,
                             const TokenSnapshot& newer)
{
    if (older.generation > newer.generation) {
        return older;
    }
    // The newer answer is a full snapshot, so it *replaces* the older one.
    // The only thing worth carrying over is nothing at all: keeping an old
    // token that the server no longer reports would paint a token at a
    // position the newer answer deliberately dropped.
    return newer;
}

// ---------------------------------------------------------------------------
// Hierarchy
// ---------------------------------------------------------------------------

QList<HierarchyItem> parseHierarchyItems(const QJsonValue& result)
{
    QList<HierarchyItem> items;
    const QJsonArray array = result.toArray();
    items.reserve(array.size());
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        HierarchyItem item;
        item.name = stringOf(object, "name");
        item.detail = stringOf(object, "detail");
        item.kind = intOf(object, "kind");
        item.uri = stringOf(object, "uri");
        item.range = parseRange(object.value(QLatin1String("range")));
        const QJsonValue selection = object.value(QLatin1String("selectionRange"));
        item.selectionRange = selection.isObject() ? parseRange(selection) : item.range;
        item.data = object.value(QLatin1String("data")).toObject();
        if (item.isValid()) {
            items.append(item);
        }
    }
    return items;
}

QList<CallEdge> parseCallEdges(const QJsonValue& result)
{
    QList<CallEdge> edges;
    const QJsonArray array = result.toArray();
    edges.reserve(array.size());
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        // incomingCalls: {"from": item, "fromRanges": [...]}
        // outgoingCalls: {"to": item, "fromRanges": [...]}
        // Both call the related function `item` here; which member it came
        // from is the caller's business (it knows which request it sent), and
        // the ranges mean "inside the function that was asked about".
        QJsonObject related = object.value(QLatin1String("from")).toObject();
        if (related.isEmpty()) {
            related = object.value(QLatin1String("to")).toObject();
        }
        if (related.isEmpty()) {
            continue;
        }
        const QList<HierarchyItem> parsed =
            parseHierarchyItems(QJsonArray{related});
        if (parsed.isEmpty()) {
            continue;
        }
        CallEdge edge;
        edge.item = parsed.first();
        for (const QJsonValue& range :
             object.value(QLatin1String("fromRanges")).toArray()) {
            edge.fromRanges.append(parseRange(range));
        }
        edges.append(edge);
    }
    return edges;
}

// ---------------------------------------------------------------------------
// Class view
// ---------------------------------------------------------------------------

QList<ClassType> buildClassView(const QJsonValue& documentSymbols)
{
    QList<ClassType> types;
    const QJsonArray array = documentSymbols.toArray();
    for (const QJsonValue& value : array) {
        const QJsonObject object = value.toObject();
        const int kind = intOf(object, "kind");
        const bool isType = kind == SymbolKind::Class || kind == SymbolKind::Struct
                            || kind == SymbolKind::Interface || kind == SymbolKind::Object;
        if (!isType) {
            continue;
        }
        ClassType type;
        type.name = stringOf(object, "name");
        type.detail = stringOf(object, "detail");
        type.baseClasses = baseClassesFromDetail(type.detail);
        type.kind = kind;
        const Range selection = parseRange(object.value(QLatin1String("selectionRange")));
        type.line = selection.startLine;
        type.column = selection.startColumn;
        collectMembers(object.value(QLatin1String("children")).toArray(),
                       &type.members);
        if (!type.name.isEmpty()) {
            types.append(type);
        }
    }
    return types;
}

// ---------------------------------------------------------------------------
// Call graph
// ---------------------------------------------------------------------------

QString nodeKey(const HierarchyItem& item)
{
    return item.uri + QLatin1Char('#') + QString::number(item.selectionRange.startLine)
           + QLatin1Char(':') + QString::number(item.selectionRange.startColumn);
}

int CallGraph::indexOf(const QString& key) const
{
    for (int i = 0; i < nodes.size(); ++i) {
        if (nodes.at(i).key == key) {
            return i;
        }
    }
    return -1;
}

int CallGraph::addNode(const HierarchyItem& item)
{
    const QString key = nodeKey(item);
    const int existing = indexOf(key);
    if (existing >= 0) {
        return existing;
    }
    CallGraphNode node;
    node.key = key;
    node.name = item.name.isEmpty() ? QStringLiteral("(anonymous)") : item.name;
    node.file = pathFromUri(item.uri);
    node.line = item.selectionRange.startLine;
    nodes.append(node);
    return nodes.size() - 1;
}

void CallGraph::addCall(const HierarchyItem& caller, const HierarchyItem& callee)
{
    const int from = addNode(caller);
    const int to = addNode(callee);
    const QPair<int, int> edge(from, to);
    if (!edges.contains(edge)) {
        edges.append(edge);
    }
}

namespace {

QString escapeDot(const QString& text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return escaped;
}

void appendTree(const CallGraph& graph, int index, int depth,
                QSet<int>* onPath, QStringList* out)
{
    const QString indent(static_cast<qsizetype>(depth) * 2, QLatin1Char(' '));
    const CallGraphNode& node = graph.nodes.at(index);
    const QString location = node.file.isEmpty()
        ? QString()
        : QStringLiteral(" (%1:%2)").arg(node.file).arg(node.line + 1);
    if (onPath->contains(index)) {
        out->append(indent + node.name + location + QStringLiteral("  [recursive]"));
        return;
    }
    out->append(indent + node.name + location);
    onPath->insert(index);
    for (const auto& edge : graph.edges) {
        if (edge.first == index) {
            appendTree(graph, edge.second, depth + 1, onPath, out);
        }
    }
    onPath->remove(index);
}

bool reachesCycle(const CallGraph& graph, int index, QSet<int>* onPath,
                  QSet<int>* done)
{
    if (onPath->contains(index)) {
        return true;
    }
    if (done->contains(index)) {
        return false;
    }
    onPath->insert(index);
    for (const auto& edge : graph.edges) {
        if (edge.first != index) {
            continue;
        }
        if (reachesCycle(graph, edge.second, onPath, done)) {
            onPath->remove(index);
            return true;
        }
    }
    onPath->remove(index);
    done->insert(index);
    return false;
}

}  // namespace

QString CallGraph::toDot(const QString& title) const
{
    QString dot;
    dot += QStringLiteral("digraph \"%1\" {\n")
               .arg(escapeDot(title.isEmpty() ? QStringLiteral("calls") : title));
    dot += QStringLiteral("  graph [rankdir=LR];\n");
    dot += QStringLiteral("  node [shape=box, fontname=\"monospace\"];\n");
    for (int i = 0; i < nodes.size(); ++i) {
        const CallGraphNode& node = nodes.at(i);
        const QString label = node.file.isEmpty()
            ? node.name
            : QStringLiteral("%1\\n%2:%3")
                  .arg(node.name, node.file)
                  .arg(node.line + 1);
        dot += QStringLiteral("  n%1 [label=\"%2\"];\n").arg(i).arg(escapeDot(label));
    }
    for (const auto& edge : edges) {
        dot += QStringLiteral("  n%1 -> n%2;\n").arg(edge.first).arg(edge.second);
    }
    dot += QStringLiteral("}\n");
    return dot;
}

QString CallGraph::toTreeText(const QString& rootKey) const
{
    const int root = indexOf(rootKey);
    if (root < 0) {
        return {};
    }
    QStringList lines;
    QSet<int> onPath;
    appendTree(*this, root, 0, &onPath, &lines);
    return lines.join(QLatin1Char('\n'));
}

bool CallGraph::hasCycleFrom(const QString& rootKey) const
{
    const int root = indexOf(rootKey);
    if (root < 0) {
        return false;
    }
    QSet<int> onPath;
    QSet<int> done;
    return reachesCycle(*this, root, &onPath, &done);
}

// ---------------------------------------------------------------------------
// Workspace edits
// ---------------------------------------------------------------------------

QString applyTextEdits(const QString& text, const QList<TextEdit>& edits,
                       int* applied, int* dropped)
{
    if (applied != nullptr) {
        *applied = 0;
    }
    if (dropped != nullptr) {
        *dropped = 0;
    }

    // Split once: a range has to be clamped to the line's real extent, and
    // doing that on a QStringList keeps the arithmetic honest.
    QStringList lines = text.split(QLatin1Char('\n'));

    QList<TextEdit> ordered = edits;
    std::sort(ordered.begin(), ordered.end(), [](const TextEdit& a, const TextEdit& b) {
        if (a.line != b.line) {
            return a.line > b.line;
        }
        if (a.column != b.column) {
            return a.column > b.column;
        }
        return a.endLine > b.endLine;
    });

    int ok = 0;
    int lost = 0;
    for (const TextEdit& edit : ordered) {
        if (edit.line < 0 || edit.line >= lines.size() || edit.endLine < edit.line
            || edit.endLine >= lines.size() || edit.column < 0 || edit.endColumn < 0) {
            ++lost;
            continue;
        }
        QString& startLine = lines[edit.line];
        QString& endLine = lines[edit.endLine];
        const int startColumn = qBound(0, edit.column, static_cast<int>(startLine.size()));
        const int endColumn = qBound(0, edit.endColumn, static_cast<int>(endLine.size()));
        if (edit.line == edit.endLine && endColumn < startColumn) {
            ++lost;
            continue;
        }
        if (edit.line == edit.endLine) {
            startLine = startLine.left(startColumn) + edit.newText
                        + startLine.mid(endColumn);
        } else {
            QString joined = startLine.left(startColumn) + edit.newText
                             + endLine.mid(endColumn);
            lines[edit.line] = joined;
            lines.erase(lines.begin() + edit.endLine,
                        lines.begin() + edit.endLine + 1);
        }
        ++ok;
    }
    if (applied != nullptr) {
        *applied = ok;
    }
    if (dropped != nullptr) {
        *dropped = lost;
    }
    return lines.join(QLatin1Char('\n'));
}

QList<QPair<QString, QList<TextEdit>>> groupByFile(const QList<TextEdit>& edits)
{
    QList<QPair<QString, QList<TextEdit>>> grouped;
    for (const TextEdit& edit : edits) {
        int bucket = -1;
        for (int i = 0; i < grouped.size(); ++i) {
            if (grouped.at(i).first == edit.filePath) {
                bucket = i;
                break;
            }
        }
        if (bucket < 0) {
            grouped.append(qMakePair(edit.filePath, QList<TextEdit>()));
            bucket = grouped.size() - 1;
        }
        grouped[bucket].second.append(edit);
    }
    return grouped;
}

}  // namespace LightCpp::Workbench::Lsp
