#include "lightcpp/text.h"

#include <algorithm>

namespace lightcpp::text {
namespace {

/// 解码一个 UTF-8 码点，返回字节数；非法字节按单字节处理，保证不会死循环。
std::size_t decode(const std::string& s, std::size_t pos, unsigned int* cp) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };
    const unsigned char c = byte(pos);
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0 && pos + 1 < s.size()) {
        *cp = ((c & 0x1Fu) << 6) | (byte(pos + 1) & 0x3Fu);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && pos + 2 < s.size()) {
        *cp = ((c & 0x0Fu) << 12) | ((byte(pos + 1) & 0x3Fu) << 6) | (byte(pos + 2) & 0x3Fu);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && pos + 3 < s.size()) {
        *cp = ((c & 0x07u) << 18) | ((byte(pos + 1) & 0x3Fu) << 12) | ((byte(pos + 2) & 0x3Fu) << 6) |
              (byte(pos + 3) & 0x3Fu);
        return 4;
    }
    *cp = c;
    return 1;
}

bool isWide(unsigned int cp) {
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
           (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
           (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

}  // namespace

std::size_t displayWidth(const std::string& text) {
    std::size_t width = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        unsigned int cp = 0;
        const std::size_t len = decode(text, pos, &cp);
        width += isWide(cp) ? std::size_t{2} : std::size_t{1};
        pos += len;
    }
    return width;
}

std::string padRight(const std::string& text, std::size_t width) {
    const std::size_t current = displayWidth(text);
    return current >= width ? text : text + std::string(width - current, ' ');
}

std::string escapeCell(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
            case '|': out += "\\|"; break;
            case '\n':
            case '\r': out += ' '; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string renderTable(const std::vector<std::string>& headers,
                        const std::vector<std::vector<std::string>>& rows) {
    std::vector<std::vector<std::string>> cells;
    cells.reserve(rows.size() + 1);
    std::vector<std::string> head;
    head.reserve(headers.size());
    for (const std::string& h : headers) {
        head.push_back(escapeCell(h));
    }
    cells.push_back(head);
    for (const auto& row : rows) {
        std::vector<std::string> line;
        line.reserve(headers.size());
        for (std::size_t i = 0; i < headers.size(); ++i) {
            line.push_back(escapeCell(i < row.size() ? row[i] : std::string()));
        }
        cells.push_back(std::move(line));
    }

    // 最小列宽 3，与 tools/lightcpp_pm.py 的 render_table 一致；否则同一份数据
    // 在两套实现里会生成宽度不同的表格，并排对比时看起来像内容不一致。
    std::vector<std::size_t> widths(headers.size(), 3);
    for (const auto& line : cells) {
        for (std::size_t i = 0; i < line.size(); ++i) {
            widths[i] = std::max(widths[i], displayWidth(line[i]));
        }
    }

    std::string out;
    auto emit = [&](const std::vector<std::string>& line) {
        out += "|";
        for (std::size_t i = 0; i < line.size(); ++i) {
            out += " ";
            out += padRight(line[i], widths[i]);
            out += " |";
        }
        out += "\n";
    };
    emit(cells[0]);
    // 分隔行与 tools/lightcpp_pm.py 保持一致：|---| 形式，两侧不加空格
    out += "|";
    for (std::size_t width : widths) {
        out += std::string(width + 2, '-');
        out += "|";
    }
    out += "\n";
    for (std::size_t i = 1; i < cells.size(); ++i) {
        emit(cells[i]);
    }
    return out;
}

std::string truncate(const std::string& text, std::size_t width) {
    if (displayWidth(text) <= width) {
        return text;
    }
    std::string out;
    std::size_t pos = 0;
    std::size_t used = 0;
    while (pos < text.size()) {
        unsigned int cp = 0;
        const std::size_t len = decode(text, pos, &cp);
        const std::size_t w = isWide(cp) ? std::size_t{2} : std::size_t{1};
        if (used + w + 1 > width) {
            break;
        }
        out.append(text, pos, len);
        used += w;
        pos += len;
    }
    out += "…";
    return out;
}

}  // namespace lightcpp::text
