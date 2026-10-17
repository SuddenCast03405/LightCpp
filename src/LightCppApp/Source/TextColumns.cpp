#include "LightCpp/App/TextColumns.hpp"

#include <QChar>

namespace LightCpp::App::TextColumns {
namespace {

/// Characters that take no cell at all: combining marks and the zero-width
/// joiners/spaces. Without these a line with a combining accent would be
/// converted one character too early.
bool takesNoCell(uint codePoint)
{
    return (codePoint >= 0x0300 && codePoint <= 0x036F)   // combining diacritics
           || (codePoint >= 0x0483 && codePoint <= 0x0489)
           || (codePoint >= 0x0591 && codePoint <= 0x05BD)
           || (codePoint >= 0x0610 && codePoint <= 0x061A)
           || (codePoint >= 0x064B && codePoint <= 0x065F)
           || (codePoint >= 0x0E31 && codePoint <= 0x0E3A)
           || (codePoint >= 0x0E47 && codePoint <= 0x0E4E)
           || (codePoint >= 0x1160 && codePoint <= 0x11FF)  // Hangul medial/final
           || (codePoint >= 0x1AB0 && codePoint <= 0x1AFF)
           || (codePoint >= 0x1DC0 && codePoint <= 0x1DFF)
           || (codePoint >= 0x200B && codePoint <= 0x200F)  // ZWSP, ZWNJ, ZWJ, marks
           || (codePoint >= 0x20D0 && codePoint <= 0x20F0)
           || (codePoint >= 0xFE00 && codePoint <= 0xFE0F)  // variation selectors
           || (codePoint >= 0xFE20 && codePoint <= 0xFE2F)
           || (codePoint >= 0xE0100 && codePoint <= 0xE01EF);
}

/// Characters a terminal draws two cells wide. This is wcwidth's rule
/// approximated by the blocks that cover real text — CJK, Hangul, kana,
/// fullwidth forms, and emoji — rather than a full East Asian Width table.
bool takesTwoCells(uint codePoint)
{
    return (codePoint >= 0x1100 && codePoint <= 0x115F)   // Hangul Jamo initial
           || (codePoint >= 0x2E80 && codePoint <= 0x303E)  // CJK radicals, Kangxi
           || (codePoint >= 0x3041 && codePoint <= 0x33FF)  // kana .. CJK compat
           || (codePoint >= 0x3400 && codePoint <= 0x4DBF)  // CJK ext A
           || (codePoint >= 0x4E00 && codePoint <= 0x9FFF)  // CJK unified
           || (codePoint >= 0xA000 && codePoint <= 0xA4CF)  // Yi
           || (codePoint >= 0xA960 && codePoint <= 0xA97F)
           || (codePoint >= 0xAC00 && codePoint <= 0xD7A3)  // Hangul syllables
           || (codePoint >= 0xF900 && codePoint <= 0xFAFF)  // CJK compat ideographs
           || (codePoint >= 0xFE10 && codePoint <= 0xFE19)
           || (codePoint >= 0xFE30 && codePoint <= 0xFE6F)  // CJK compat forms
           || (codePoint >= 0xFF00 && codePoint <= 0xFF60)  // fullwidth forms
           || (codePoint >= 0xFFE0 && codePoint <= 0xFFE6)
           || (codePoint >= 0x1F300 && codePoint <= 0x1F64F)  // emoji
           || (codePoint >= 0x1F900 && codePoint <= 0x1F9FF)
           || (codePoint >= 0x20000 && codePoint <= 0x3FFFD);  // CJK ext B+
}

int cellsFor(uint codePoint)
{
    if (takesNoCell(codePoint)) {
        return 0;
    }
    return takesTwoCells(codePoint) ? 2 : 1;
}

}  // namespace

int utf16ColumnForDisplayColumn(const QString& line, int displayColumn)
{
    if (displayColumn <= 0 || line.isEmpty()) {
        return 0;
    }

    int cells = 0;
    int units = 0;
    int i = 0;
    while (i < line.size()) {
        const QChar first = line.at(i);
        // A surrogate pair is one code point (one display width) but two UTF-16
        // code units — the difference this function exists for.
        int width = 1;
        uint codePoint = first.unicode();
        if (first.isHighSurrogate() && i + 1 < line.size()
            && line.at(i + 1).isLowSurrogate()) {
            codePoint = QChar::surrogateToUcs4(first, line.at(i + 1));
            width = 2;
        }

        const int cellCount = cellsFor(codePoint);
        if (cellCount > 0) {
            // This character starts at the requested cell: that is the answer.
            if (cells >= displayColumn) {
                break;
            }
            // ... or the column points into the middle of a double-width
            // character, in which case the character it falls inside is the one
            // the compiler meant.
            if (cells + cellCount > displayColumn) {
                break;
            }
        }
        cells += cellCount;
        units += width;
        i += width;
    }
    return units;
}

}  // namespace LightCpp::App::TextColumns
