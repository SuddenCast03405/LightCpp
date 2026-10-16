// FindScan — where a find/replace scan resumes after it matched.
//
// One decision, three scans: Find Next/Previous, the live match counter and
// Replace All walk the same document with QTextDocument::find. A match of
// width zero ("\b", "x*", "^") has no end to move to, so a scan that reuses
// the match end re-finds the very same spot for ever — Next appears dead,
// the counter pins at its cap, and Replace All writes the replacement there
// thousands of times. Stepping one character past is the whole fix; keeping
// it in one place is why this is not the same "if" written three times.

#pragma once

namespace LightCpp::App::FindScan {

/// Where a scan resumes after it matched [start, start+length).
///
/// A normal match resumes at its end. A zero-width match resumes one
/// character further along, in the direction the scan runs — the only way
/// to make progress. Returns -1 when there is no room left in that
/// direction, and the caller either wraps around (the search) or stops
/// (the counting and replacing loops).
///
/// `docEnd` is the last position a cursor may occupy, i.e.
/// QTextDocument::characterCount() - 1.
[[nodiscard]] int resumeAfter(int start, int length, int docEnd,
                              bool backward = false);

}  // namespace LightCpp::App::FindScan
