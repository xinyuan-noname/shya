// shya - the formatting stage that runs over generated JavaScript.
//
// The code generator already emits correctly indented code, so this stage is not a
// pretty-printer: it normalises the layout of the *whole file* (blank lines between
// top-level statements, stray empty statements, trailing whitespace) and it is the single
// place that decides what "well laid out" means for the output.
//
// The one hard rule: text the author wrote inside `@ts{ … }` is verbatim JavaScript. The
// code generator marks the start of every line of such a payload with `kRawMark`; those
// lines pass through untouched, because reflowing them could change what they mean.
#include "shya.h"

#include <cctype>

namespace shya {

namespace {

std::string trimRight(std::string s) {
    while (!s.empty()) {
        const unsigned char c = static_cast<unsigned char>(s.back());
        if (c == ' ' || c == '\t' || c == '\r') s.pop_back();
        else break;
    }
    return s;
}

std::size_t leadingSpaces(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return i;
}

// A line that continues the statement above it: hitting one of these at the left margin
// must not trigger a separating blank line.
bool isContinuation(const std::string& content) {
    if (content.empty()) return false;
    switch (content[0]) {
        case '}': case ')': case ']': case '.': case '?': case ',': case ':': case ';':
            return true;
        // A comment line belongs with what it documents (and with the banner above it),
        // so it never gets a blank line of its own.
        case '/':
            return true;
        default:
            return false;
    }
}

}  // namespace

std::string formatJavaScript(const std::string& src, int indentWidth, bool tidy) {
    const int width = indentWidth > 0 ? indentWidth : 2;

    std::vector<std::pair<int, std::string>> code;  // depth, content (tidy mode)
    std::vector<std::string> lines;                  // final lines, in order
    std::vector<bool> verbatim;

    std::size_t i = 0;
    while (i <= src.size()) {
        const std::size_t nl = src.find('\n', i);
        std::string line = src.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        const bool last = nl == std::string::npos;
        i = last ? src.size() + 1 : nl + 1;

        bool raw = false;
        std::string cleaned;
        cleaned.reserve(line.size());
        for (char c : line) {
            if (c == kRawMark) {
                raw = true;
                continue;
            }
            cleaned.push_back(c);
        }
        cleaned = trimRight(cleaned);

        lines.push_back(cleaned);
        verbatim.push_back(raw);

        if (last) break;
    }

    std::string out;
    bool pendingBlank = false;
    bool emitted = false;
    bool previousBlank = true;

    for (std::size_t k = 0; k < lines.size(); ++k) {
        const std::string& text = lines[k];

        if (verbatim[k]) {
            if (pendingBlank && emitted) out += "\n";
            pendingBlank = false;
            out += text;
            out += '\n';
            emitted = true;
            previousBlank = text.empty();
            continue;
        }

        const std::size_t lead = leadingSpaces(text);
        std::string content = text.substr(lead);
        if (!tidy) {
            // Marks stripped and line endings normalised; layout left as generated.
            if (pendingBlank && emitted) out += "\n";
            pendingBlank = false;
            out += text;
            out += '\n';
            emitted = true;
            continue;
        }

        if (content.empty()) {
            pendingBlank = true;
            continue;
        }
        // An empty statement carries no meaning; drop it.
        if (content == ";") continue;
        // A `@ts` payload that already ended in `;` used to leave `;;` behind.
        if (content.size() >= 2 && content.compare(content.size() - 2, 2, ";;") == 0) {
            content.pop_back();
        }

        const int depth = width > 0 ? static_cast<int>(lead / static_cast<std::size_t>(width)) : 0;
        const bool topLevel = depth == 0;

        if (pendingBlank) {
            if (emitted && !previousBlank) {
                out += "\n";
                previousBlank = true;
            }
            pendingBlank = false;
        }
        // Separate top-level statements so the file scans as a list of declarations.
        // A blank line that was already preserved above counts as that separation, so the
        // two rules must not both fire.
        if (topLevel && emitted && !previousBlank && !isContinuation(content)) {
            out += "\n";
            previousBlank = true;
        }

        out.append(static_cast<std::size_t>(depth) * static_cast<std::size_t>(width), ' ');
        out += content;
        out += '\n';
        emitted = true;
        previousBlank = false;
    }

    while (!out.empty() && out.back() == '\n') out.pop_back();
    out += '\n';
    return out;
}

}  // namespace shya
