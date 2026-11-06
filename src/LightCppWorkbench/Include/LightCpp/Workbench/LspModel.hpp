// LspModel — the parts of the Language Server Protocol answer that the IDE
// handles as data rather than as widgets.
//
// lightcpp.1.md §六 asks for semantic highlighting, call hierarchy, type
// hierarchy, inheritance, a class view and a call graph. clangd answers all of
// them, but each answer is a JSON shape whose edges are easy to get wrong:
//
//   * semantic tokens are a *delta* stream of 5-tuples, not absolute
//     positions — a decoder that forgets `deltaLine` resets every token to
//     line 0 of the file and paints the wrong lines, silently.
//   * `callHierarchy/incomingCalls` names the caller as `from` and puts the
//     call sites in `fromRanges`; `outgoingCalls` does the mirror image with
//     `to`. Reading the wrong member produces a graph pointing backwards.
//   * `documentSymbol` may answer with a nested DocumentSymbol tree or with a
//     flat SymbolInformation list. Both are legal, and only the former has
//     children.
//
// Everything here is a pure function over QJsonValue, so the shapes above are
// pinned by unit tests instead of by a live clangd.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

namespace LightCpp::Workbench::Lsp {

// ---------------------------------------------------------------------------
// Positions
// ---------------------------------------------------------------------------

/// An LSP range. Lines and columns are 0-based; the column unit is UTF-16
/// code units, which is what QTextDocument indexes with.
struct Range {
    int startLine = 0;
    int startColumn = 0;
    int endLine = 0;
    int endColumn = 0;

    [[nodiscard]] bool isValid() const
    {
        return endLine > startLine || (endLine == startLine && endColumn >= startColumn);
    }
    [[nodiscard]] bool contains(int line, int column) const
    {
        if (line < startLine || line > endLine) {
            return false;
        }
        if (line == startLine && column < startColumn) {
            return false;
        }
        if (line == endLine && column > endColumn) {
            return false;
        }
        return true;
    }
};

/// Read a `range` / `selectionRange` object. A malformed range yields the
/// default (0,0)-(0,0) rather than throwing.
[[nodiscard]] Range parseRange(const QJsonValue& value);

// ---------------------------------------------------------------------------
// Semantic tokens
// ---------------------------------------------------------------------------

/// One decoded semantic token, in absolute document coordinates.
struct SemanticToken {
    int line = 0;      ///< 0-based
    int column = 0;    ///< 0-based, UTF-16 units
    int length = 0;    ///< in UTF-16 code units
    int type = 0;      ///< index into the legend's tokenTypes
    int modifiers = 0; ///< bit set, index into the legend's tokenModifiers
};

/// What the server said its token types and modifiers are called, in the order
/// the integers index them. Sent once, in the initialize response.
struct SemanticLegend {
    QStringList tokenTypes;
    QStringList tokenModifiers;

    [[nodiscard]] bool isEmpty() const { return tokenTypes.isEmpty(); }
    [[nodiscard]] QString typeName(int index) const;
    [[nodiscard]] bool hasModifier(int bits, const QString& name) const;
    [[nodiscard]] QStringList modifierNames(int bits) const;
    /// The bit the named modifier occupies, or -1.
    [[nodiscard]] int modifierBit(const QString& name) const;
};

/// Read `result.capabilities.semanticTokensProvider.legend` from the
/// initialize response. An empty legend means the server cannot do semantic
/// tokens at all, and no request should be sent.
[[nodiscard]] SemanticLegend parseLegend(const QJsonObject& initializeResult);

/// Decode `textDocument/semanticTokens/full` `result.data`.
///
/// The stream is a flat array of 5 * n integers
/// (deltaLine, deltaStartChar, length, tokenType, tokenModifiers), where the
/// line is relative to the previous token's line and the start character is
/// relative to the previous token's start *on the same line* — and absolute
/// when the delta line is non-zero. Both rules are in the spec and both are
/// implemented here; a length that does not divide by five yields an empty
/// list and a message in `error`, since a partial decode paints garbage.
[[nodiscard]] QList<SemanticToken> decodeSemanticTokens(const QJsonArray& data,
                                                        QString* error = nullptr);

/// The coarse role the editor paints a token with. The mapping from the
/// server's own type name is deliberately lossy: the editor has a palette, not
/// a spec.
enum class TokenRole {
    Unknown = 0,
    Namespace,
    Type,        ///< "type", "struct", "class", "enum", "interface", "concept"
    TypeParameter,
    Parameter,
    Variable,
    Property,
    EnumMember,
    Function,    ///< "function", "method"
    Macro,
    Keyword,
    Modifier,
    Comment,
    String,
    Number,
    Regexp,
    Operator,
    Decorator,
    Bracket,
    Label,
    Event,
};

/// Map an LSP token type name (clangd's plus the standard set) to a role.
[[nodiscard]] TokenRole roleFor(const QString& typeName);

/// A short human name for a role — used by the "Semantic tokens" legend in the
/// settings page and by the tests.
[[nodiscard]] QString roleName(TokenRole role);

/// Merge a new full-token answer over an earlier one for the same document:
/// the server sends the whole set each time, but a token that moved must not
/// leave its old paint behind. Returns the new list unchanged when `previous`
/// is empty; otherwise drops previous tokens whose span is no longer covered.
///
/// The caller re-paints from scratch after every answer, so this exists only
/// for the case where a document is edited while a request is in flight and
/// the late answer must not resurrect a stale token that the newer answer
/// already removed. `generation` is the document version each list was
/// computed against; an older generation is dropped entirely.
struct TokenSnapshot {
    int generation = 0;
    QList<SemanticToken> tokens;
};

[[nodiscard]] TokenSnapshot mergeSnapshots(const TokenSnapshot& older,
                                           const TokenSnapshot& newer);

// ---------------------------------------------------------------------------
// Hierarchy (call / type / inheritance)
// ---------------------------------------------------------------------------

/// One item of a call or type hierarchy: the server's opaque `data` has to be
/// echoed back verbatim in the follow-up request, so it is kept as JSON.
struct HierarchyItem {
    QString name;
    QString detail;
    int kind = 0;
    QString uri;
    Range range;
    Range selectionRange;
    QJsonObject data;

    [[nodiscard]] bool isValid() const { return !uri.isEmpty(); }
};

/// One edge of a call hierarchy: the related function plus the ranges inside
/// the *requested* function where the call happens.
struct CallEdge {
    HierarchyItem item;
    QList<Range> fromRanges;
};

/// `textDocument/prepareCallHierarchy` and
/// `typeHierarchy/supertypes|subtypes` all answer with an array of items.
[[nodiscard]] QList<HierarchyItem> parseHierarchyItems(const QJsonValue& result);

/// `callHierarchy/incomingCalls` / `outgoingCalls`.
[[nodiscard]] QList<CallEdge> parseCallEdges(const QJsonValue& result);

/// The local path of an item's URI, decoded from the `file://` form.
[[nodiscard]] QString pathFromUri(const QString& uri);

// ---------------------------------------------------------------------------
// Class view
// ---------------------------------------------------------------------------

/// One member of a class in the class view.
struct ClassMember {
    QString name;
    QString detail;
    int kind = 0;
    int line = 0;
    int column = 0;
};

/// One class/struct of the class view, with its own members.
struct ClassType {
    QString name;
    QString detail;      ///< the server's `detail`, e.g. "class Foo : public Bar"
    QString baseClasses; ///< extracted from detail after ':' when present
    int kind = 0;
    int line = 0;
    int column = 0;
    QList<ClassMember> members;
};

/// Build the class view from a `textDocument/documentSymbol` answer: every
/// class/struct/interface/union is a row, its children are the members.
/// Nested classes are flattened into the parent's member list but keep their
/// own name, so a nested type is findable by name.
[[nodiscard]] QList<ClassType> buildClassView(const QJsonValue& documentSymbols);

/// LSP SymbolKind values used by the class view.
namespace SymbolKind {
constexpr int Class = 5;
constexpr int Method = 6;
constexpr int Property = 7;
constexpr int Field = 8;
constexpr int Constructor = 9;
constexpr int Enum = 10;
constexpr int Interface = 11;
constexpr int Function = 12;
constexpr int Variable = 13;
constexpr int Constant = 14;
constexpr int Struct = 23;
constexpr int Event = 24;
constexpr int Operator = 25;
constexpr int TypeParameter = 26;
constexpr int Namespace = 3;
constexpr int Object = 19;
constexpr int EnumMember = 22;
}  // namespace SymbolKind

// ---------------------------------------------------------------------------
// Call graph
// ---------------------------------------------------------------------------

/// A directed call graph, built one hierarchy answer at a time.
///
/// The nodes are keyed by (uri, selection start) rather than by name: two
/// overloads of `run()` are two nodes, and a graph that merges them claims a
/// call that may not exist.
struct CallGraphNode {
    QString key;
    QString name;
    QString file;
    int line = 0;
};

struct CallGraph {
    QList<CallGraphNode> nodes;
    QList<QPair<int, int>> edges;  ///< (caller index, callee index)

    /// Add `item` as a node, returning its index (existing node reused).
    int addNode(const HierarchyItem& item);
    /// Add "caller calls callee" — both are added when new.
    void addCall(const HierarchyItem& caller, const HierarchyItem& callee);

    [[nodiscard]] bool isEmpty() const { return nodes.isEmpty(); }
    [[nodiscard]] int indexOf(const QString& key) const;

    /// Graphviz DOT, for "export the call graph".
    [[nodiscard]] QString toDot(const QString& title = QString()) const;

    /// An indented tree rooted at `rootKey`, cycles cut at the first repeat.
    /// This is what the panel shows when Graphviz is not installed.
    [[nodiscard]] QString toTreeText(const QString& rootKey) const;

    /// True when the graph contains a cycle reachable from `rootKey`.
    [[nodiscard]] bool hasCycleFrom(const QString& rootKey) const;
};

/// The node key of an item: "<uri>#<line>:<column>".
[[nodiscard]] QString nodeKey(const HierarchyItem& item);

// ---------------------------------------------------------------------------
// Workspace edits (rename / code actions)
// ---------------------------------------------------------------------------

/// One text edit with the document it belongs to.
struct TextEdit {
    QString filePath;
    int line = 0;
    int column = 0;
    int endLine = 0;
    int endColumn = 0;
    QString newText;
};

/// Apply `edits` to `text` (all edits must belong to the same document),
/// bottom-up. Returns the new text; edits whose range falls outside the
/// document are dropped and counted in `dropped`.
///
/// This is the same rule the in-editor path uses, moved out of the widget so
/// it can also be applied to a file that is *not* open — which is the
/// difference between a rename that completes and one that half-completes.
[[nodiscard]] QString applyTextEdits(const QString& text,
                                     const QList<TextEdit>& edits,
                                     int* applied = nullptr,
                                     int* dropped = nullptr);

/// Group edits by their document, preserving first-seen order.
[[nodiscard]] QList<QPair<QString, QList<TextEdit>>>
groupByFile(const QList<TextEdit>& edits);

}  // namespace LightCpp::Workbench::Lsp
