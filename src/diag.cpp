// shya - diagnostic rendering.
#include "shya.h"

namespace shya {

namespace {

std::string severityLabel(Severity s) {
    switch (s) {
        case Severity::Error: return "error";
        case Severity::Warning: return "warning";
        case Severity::Note: return "note";
    }
    return "error";
}

// Returns the 1-based `line` of `source` without its trailing newline.
std::string sourceLine(const std::string& source, int line) {
    int cur = 1;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= source.size(); ++i) {
        if (i == source.size() || source[i] == '\n') {
            if (cur == line) {
                std::string s = source.substr(start, i - start);
                while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
                return s;
            }
            ++cur;
            start = i + 1;
        }
    }
    return std::string();
}

// Column width of a UTF-8 lead byte sequence table, used so the caret lines up
// with the printed source line for CJK identifiers.
int utf8WidthAt(const std::string& s, int col) {
    int c = 1;
    int width = 0;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        int bytes = 1;
        if ((ch & 0x80) == 0x00) bytes = 1;
        else if ((ch & 0xE0) == 0xC0) bytes = 2;
        else if ((ch & 0xF0) == 0xE0) bytes = 3;
        else if ((ch & 0xF8) == 0xF0) bytes = 4;
        if (c == col) {
            // CJK / fullwidth ranges render two columns wide in a terminal.
            if (bytes == 3) {
                unsigned int cp = ((ch & 0x0F) << 12);
                if (i + 1 < s.size()) cp |= (static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6;
                if (i + 2 < s.size()) cp |= (static_cast<unsigned char>(s[i + 2]) & 0x3F);
                width = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                                (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                                (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                                (cp >= 0xFFE0 && cp <= 0xFFE6)
                            ? 2
                            : 1;
            } else {
                width = 1;
            }
            break;
        }
        i += static_cast<std::size_t>(bytes);
        ++c;
    }
    return width == 0 ? 1 : width;
}

}  // namespace

std::string renderDiagnostics(const DiagBag& bag, const std::string& source,
                              const std::string& filename) {
    std::ostringstream os;
    for (const auto& d : bag.items()) {
        os << filename << ':' << d.pos.line << ':' << d.pos.col << ": "
           << severityLabel(d.severity) << ": " << d.message;
        if (!d.code.empty()) os << " [" << d.code << ']';
        os << '\n';
        std::string src = sourceLine(source, d.pos.line);
        if (!src.empty()) {
            os << "  " << src << '\n';
            os << "  ";
            for (int i = 1; i < d.pos.col; ++i) {
                if (i - 1 < static_cast<int>(src.size()))
                    os << std::string(static_cast<std::size_t>(utf8WidthAt(src, i)), ' ');
                else
                    os << ' ';
            }
            os << "^\n";
        }
    }
    return os.str();
}

}  // namespace shya
