// TextColumns — a compiler's column, expressed in the editor's units.
//
// gcc counts columns in *display* cells by default — East Asian characters are
// two cells wide, so a column on a line holding CJK text means a different
// character than the same number does in the editor, which counts UTF-16 code
// units. Measured on this machine (LC_ALL=C, g++ 16.2) with the line
//
//     int 变量 = 未定义;
//
// where the token `未定义` starts 17 bytes / 15 display cells / 13 characters
// into the line. gcc reported 18 with -fdiagnostics-column-unit=byte, 16 with
// =display, and 16 by default — so the default is display, not bytes: the
// editor was handed 16-1 = 15 where it needed 13, i.e. one cell too far for
// every CJK character before the mistake (and 2 too far per character if a
// project asks gcc for byte columns).
//
// What cannot be fixed here: a project that passes -fdiagnostics-column-unit
// itself. The IDE parses the build's log, it does not own the flags, so the
// unit is whatever the project chose; display is the default and therefore the
// one worth matching.
//
// Kept as one function because the same number is consumed twice — by the
// squiggle layer and by navigation — and the two conventions are otherwise
// indistinguishable at the call site.

#pragma once

#include <QString>

namespace LightCpp::App::TextColumns {

/// UTF-16 code-unit offset of the character that sits at `displayColumn`
/// cells into `line`. Both are zero-based (the parser has already removed
/// gcc's one-based origin), matching what QTextCursor counts.
///
/// A column that lands inside a double-width character — the odd column of a
/// CJK character — resolves to that character rather than to the one after it.
[[nodiscard]] int utf16ColumnForDisplayColumn(const QString& line, int displayColumn);

}  // namespace LightCpp::App::TextColumns
