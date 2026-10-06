// shya - lexer.
#include "shya.h"

namespace shya {

namespace {

const std::unordered_set<std::string>& keywords() {
    static const std::unordered_set<std::string> kw = {
        "if",     "elif",    "else",       "case",   "default", "fallthrough",
        "for",    "of",      "break",      "continue", "let",   "const",
        "define", "declare", "macro",      "import", "from",   "export",
        "as",     "fn",      "async",      "await",  "return",  "throw",
        "try",    "catch",   "finally",    "is",     "not",     "instanceof",
        "void",   "true",    "false",      "this",   "typeof",  "new",
        "null",   "undefined",
    };
    return kw;
}

bool isIdentStart(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_' || c >= 0x80;
}

bool isIdentPart(unsigned char c) {
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }

// Multi-character punctuation, tried longest first.
const char* kPuncts[] = {
    "...", "**=", "===", "!==", "~/", "\\/", "+/", "-/",  "==",  "!=",
    "<=",  ">=",  "&&",  "||",  "++", "--", "+=", "-=",  "*=",  "/=",
    "%=",  "^=",  "=>",  "->",  "**", "(",  ")",  "[",   "]",   "{",
    "}",   ",",   ":",   ";",   ".",  "=",  "+",  "-",   "*",   "/",
    "%",   "<",   ">",   "!",   "?",  "^",  "&",  "|",   "~",   "\\",
};

}  // namespace

Lexer::Lexer(std::string source, std::string filename)
    : src_(std::move(source)), file_(std::move(filename)) {}

Pos Lexer::here() const {
    Pos p;
    p.line = line_;
    p.col = col_;
    p.offset = static_cast<int>(i_);
    return p;
}

char Lexer::advance() {
    char c = src_[i_++];
    if (c == '\n') {
        ++line_;
        col_ = 1;
    } else {
        ++col_;
    }
    return c;
}

void Lexer::skipTrivia(DiagBag& bag) {
    for (;;) {
        if (eof()) return;
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') {
            advance();
            continue;
        }
        if (c == '/' && peek(1) == '/') {
            while (!eof() && peek() != '\n') advance();
            continue;
        }
        if (c == '/' && peek(1) == '*') {
            Pos start = here();
            advance();
            advance();
            int depth = 1;
            while (!eof() && depth > 0) {
                if (peek() == '/' && peek(1) == '*') {
                    advance();
                    advance();
                    ++depth;
                } else if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    --depth;
                } else {
                    advance();
                }
            }
            if (depth > 0) bag.error(start, "LEX003", "块注释没有闭合，缺少 `*/`");
            continue;
        }
        // A BOM at the very start of the file.
        if (i_ == 0 && static_cast<unsigned char>(c) == 0xEF) {
            advance();
            advance();
            advance();
            continue;
        }
        return;
    }
}

Token Lexer::lexNumber(DiagBag& bag) {
    Pos start = here();
    std::string text;
    bool isFloat = false;

    auto digits = [&](bool (*pred)(unsigned char)) {
        while (!eof() && (pred(static_cast<unsigned char>(peek())) || peek() == '_')) {
            char c = advance();
            if (c != '_') text.push_back(c);
        }
    };

    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'b' || peek(1) == 'B' ||
                          peek(1) == 'o' || peek(1) == 'O')) {
        text.push_back(advance());
        text.push_back(advance());
        digits([](unsigned char c) {
            return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        });
    } else {
        digits(isDigit);
        if (peek() == '.' && isDigit(static_cast<unsigned char>(peek(1)))) {
            isFloat = true;
            text.push_back(advance());
            digits(isDigit);
        } else if (peek() == '.' && !isIdentStart(static_cast<unsigned char>(peek(1))) &&
                   peek(1) != '.') {
            // '1.' is a valid float literal.
            isFloat = true;
            text.push_back(advance());
        }
        if (peek() == 'e' || peek() == 'E') {
            std::size_t save = i_;
            int saveLine = line_, saveCol = col_;
            std::string exp;
            exp.push_back(advance());
            if (peek() == '+' || peek() == '-') exp.push_back(advance());
            if (isDigit(static_cast<unsigned char>(peek()))) {
                while (!eof() && (isDigit(static_cast<unsigned char>(peek())) || peek() == '_')) {
                    char c = advance();
                    if (c != '_') exp.push_back(c);
                }
                text += exp;
                isFloat = true;
            } else {
                i_ = save;
                line_ = saveLine;
                col_ = saveCol;
            }
        }
    }

    Token t;
    t.kind = Tok::Number;
    t.pos = start;
    t.end = here();
    t.text = text;
    try {
        if (!text.empty() && (text[0] == '0') &&
            (text.size() > 1 && (text[1] == 'x' || text[1] == 'X' || text[1] == 'b' ||
                                 text[1] == 'B' || text[1] == 'o' || text[1] == 'O'))) {
            t.num = static_cast<double>(std::stoull(text, nullptr, 0));
        } else {
            t.num = std::stod(text);
        }
    } catch (...) {
        bag.error(start, "LEX001", "无法解析的数值字面量 `" + text + "`");
        t.num = 0;
    }
    (void)isFloat;
    return t;
}

Token Lexer::lexString(DiagBag& bag) {
    Pos start = here();
    char quote = advance();
    std::string value;
    bool closed = false;
    while (!eof()) {
        char c = advance();
        if (c == quote) {
            closed = true;
            break;
        }
        if (c == '\\') {
            if (eof()) break;
            char e = advance();
            value.push_back('\\');
            value.push_back(e);
            continue;
        }
        if (c == '\n') {
            bag.error(start, "LEX004", "字符串字面量在换行前没有闭合");
            break;
        }
        value.push_back(c);
    }
    if (!closed && eof()) {
        bag.error(start, "LEX004", "字符串字面量没有闭合");
    }
    Token t;
    t.kind = Tok::String;
    t.pos = start;
    t.end = here();
    t.text = std::string(1, quote) + value + std::string(1, quote);
    t.value = value;
    return t;
}

Token Lexer::lexTemplate(DiagBag& bag) {
    Pos start = here();
    advance();  // backtick
    std::string raw;
    raw.push_back('`');
    int depth = 0;
    bool closed = false;
    while (!eof()) {
        char c = peek();
        if (c == '\\') {
            raw.push_back(advance());
            if (!eof()) raw.push_back(advance());
            continue;
        }
        if (depth == 0 && c == '`') {
            raw.push_back(advance());
            closed = true;
            break;
        }
        if (depth == 0 && c == '$' && peek(1) == '{') {
            raw.push_back(advance());
            raw.push_back(advance());
            ++depth;
            continue;
        }
        if (depth > 0) {
            if (c == '{') ++depth;
            else if (c == '}') --depth;
            raw.push_back(advance());
            continue;
        }
        raw.push_back(advance());
    }
    if (!closed) bag.error(start, "LEX004", "模板字符串没有闭合，缺少反引号");
    Token t;
    t.kind = Tok::TemplateString;
    t.pos = start;
    t.end = here();
    t.text = raw;
    t.value = raw;
    return t;
}

// ~pi / ~e / ~lg10 / ~ln4 / ~deg~e ... folded to a double at lex time.
Token Lexer::lexMath(DiagBag& bag) {
    Pos start = here();
    advance();  // '~'
    std::string raw = "~";

    struct Const {
        const char* utf8;
        const char* ascii;
        double value;
        const char* js;
    };
    static const Const consts[] = {
        {"\xCF\x80", "pi", 3.14159265358979323846, "Math.PI"},
        {"\xE2\x84\xAF", "e", 2.71828182845904523536, "Math.E"},
        {"\xCF\x84", "tau", 6.28318530717958647692, ""},
        {"\xE2\x88\x9E", "inf", std::numeric_limits<double>::infinity(), "Infinity"},
    };
    struct Fn {
        const char* ascii;
        int arity;  // 1 = needs a numeric argument
        double (*apply)(double);
    };
    static const Fn fns[] = {
        {"lg", 1, [](double x) { return std::log10(x); }},
        {"ln", 1, [](double x) { return std::log(x); }},
        {"db", 1, [](double x) { return 10.0 * std::log10(x); }},
        {"deg", 1, [](double x) { return x * 3.14159265358979323846 / 180.0; }},
        {"rad", 1, [](double x) { return x * 180.0 / 3.14159265358979323846; }},
        {"sqrt", 1, [](double x) { return std::sqrt(x); }},
    };

    auto readSymbol = [&]() -> std::string {
        // Either an ASCII identifier or one of the unicode math symbols.
        for (const auto& c : consts) {
            std::size_t n = std::strlen(c.utf8);
            if (src_.compare(i_, n, c.utf8) == 0) {
                i_ += n;
                return std::string(c.utf8);
            }
        }
        std::string s;
        while (!eof()) {
            unsigned char c = static_cast<unsigned char>(peek());
            // Names are alphabetic only: `~lg10` is `lg` applied to `10`.
            if (c == '_' || c == '$' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                c >= 0x80) {
                s.push_back(advance());
            } else {
                break;
            }
        }
        return s;
    };

    std::string sym = readSymbol();
    if (sym.empty()) {
        bag.error(start, "LEX005", "`~` 后面必须跟数学常量名或函数名（如 ~pi、~ln2）");
        Token t;
        t.kind = Tok::MathLit;
        t.pos = start;
        t.end = here();
        t.text = raw;
        t.num = 0;
        return t;
    }
    raw += sym;

    auto findConst = [&](const std::string& s) -> const Const* {
        for (const auto& c : consts) {
            if (s == c.utf8 || s == c.ascii) return &c;
        }
        return nullptr;
    };
    auto findFn = [&](const std::string& s) -> const Fn* {
        for (const auto& f : fns) {
            if (s == f.ascii) return &f;
        }
        return nullptr;
    };

    std::optional<double> value;
    if (const Const* c = findConst(sym)) {
        value = c->value;
    } else if (const Fn* f = findFn(sym)) {
        // Expect a numeric argument: a plain number or another ~... literal.
        skipTrivia(bag);
        std::optional<double> arg;
        if (peek() == '~') {
            Token sub = lexMath(bag);
            arg = sub.num;
            raw += sub.text;
        } else if (isDigit(static_cast<unsigned char>(peek())) ||
                   (peek() == '.' && isDigit(static_cast<unsigned char>(peek(1))))) {
            Token num = lexNumber(bag);
            arg = num.num;
            raw += num.text;
        } else if (peek() == '-' && isDigit(static_cast<unsigned char>(peek(1)))) {
            advance();
            Token num = lexNumber(bag);
            arg = -num.num;
            raw += "-" + num.text;
        }
        if (!arg) {
            std::string got;
            while (!eof() && isIdentPart(static_cast<unsigned char>(peek()))) got.push_back(advance());
            std::string hint;
            if (got == "e" || got == "pi") {
                hint = "；若要自然常数/圆周率请写 ~" + got;
            }
            bag.error(start, "LEX006",
                      "`~" + sym + "` 需要一个数值参数" + (got.empty() ? std::string() : "，但遇到 `" + got + "`") + hint);
            value = 0.0;
        } else {
            value = f->apply(*arg);
        }
    } else {
        bag.error(start, "LEX007", "未知的数学字面量 `~" + sym + "`");
        value = 0.0;
    }

    Token t;
    t.kind = Tok::MathLit;
    t.pos = start;
    t.end = here();
    t.text = raw;
    t.value = raw;
    t.num = value.value_or(0.0);
    return t;
}

// '@ts{ ... }' - captures the payload verbatim with brace/string/comment awareness.
Token Lexer::lexTsBlock(DiagBag& bag) {
    Pos start = here();
    advance();  // '@'
    std::string head;
    while (!eof() && isIdentPart(static_cast<unsigned char>(peek()))) head.push_back(advance());
    std::string raw = "@" + head;
    while (!eof() && (peek() == ' ' || peek() == '\t')) raw.push_back(advance());
    if (peek() != '{') {
        // Not a raw block after all - hand the pieces back to the caller by
        // returning a marker token; callers only invoke this when '{' follows.
        Token t;
        t.kind = Tok::At;
        t.pos = start;
        t.end = here();
        t.text = "@";
        return t;
    }
    raw.push_back(advance());  // '{'
    int depth = 1;
    std::string payload;
    while (!eof() && depth > 0) {
        char c = peek();
        if (c == '"' || c == '\'' || c == '`') {
            char q = advance();
            payload.push_back(q);
            while (!eof()) {
                char d = advance();
                payload.push_back(d);
                if (d == '\\' && !eof()) {
                    payload.push_back(advance());
                    continue;
                }
                if (d == q) break;
            }
            continue;
        }
        if (c == '/' && (peek(1) == '/' || peek(1) == '*')) {
            if (peek(1) == '/') {
                while (!eof() && peek() != '\n') payload.push_back(advance());
            } else {
                payload.push_back(advance());
                payload.push_back(advance());
                while (!eof() && !(peek() == '*' && peek(1) == '/')) payload.push_back(advance());
                if (!eof()) {
                    payload.push_back(advance());
                    payload.push_back(advance());
                }
            }
            continue;
        }
        if (c == '{') ++depth;
        if (c == '}') {
            --depth;
            if (depth == 0) {
                advance();
                break;
            }
        }
        payload.push_back(advance());
    }
    if (depth > 0) bag.error(start, "LEX008", "`@ts{` 块没有闭合，缺少 `}`");

    Token t;
    t.kind = Tok::TsBlock;
    t.pos = start;
    t.end = here();
    t.text = raw + payload + "}";
    t.value = payload;
    t.raw = payload;
    return t;
}

std::vector<Token> Lexer::tokenize(DiagBag& bag) {
    std::vector<Token> out;
    for (;;) {
        int lineBefore = line_;
        skipTrivia(bag);
        if (eof()) break;
        bool nl = (line_ != lineBefore);
        Pos start = here();
        unsigned char c = static_cast<unsigned char>(peek());
        Token tok;

        if (isDigit(c) || (c == '.' && isDigit(static_cast<unsigned char>(peek(1))))) {
            tok = lexNumber(bag);
        } else if (isIdentStart(c)) {
            std::string text;
            while (!eof() && isIdentPart(static_cast<unsigned char>(peek()))) text.push_back(advance());
            tok.pos = start;
            tok.end = here();
            tok.text = text;
            tok.value = text;
            if (text == "_") {
                tok.kind = Tok::Under;
            } else if (keywords().count(text)) {
                tok.kind = Tok::Keyword;
                // `null` / `undefined` were removed from the language; the token
                // still lexes so that parsing recovers, but it is an error.
                if (text == "null" || text == "undefined") {
                    bag.error(start, "LEX009",
                              "shya 只有 `void`：`" + text +
                                  "` 已从语言中移除，请改写为 `void`");
                }
            } else {
                tok.kind = Tok::Identifier;
            }
        } else if (c == '"' || c == '\'') {
            tok = lexString(bag);
        } else if (c == '`') {
            tok = lexTemplate(bag);
        } else if (c == '~') {
            // `~/` is the truncating-division operator, not a math literal.
            if (peek(1) == '/') {
                advance();
                advance();
                tok.kind = Tok::Punct;
                tok.pos = start;
                tok.end = here();
                tok.text = "~/";
            } else {
                tok = lexMath(bag);
            }
        } else if (c == '@') {
            // '@ts{' would be a verbatim block; every other '@name' is a macro.
            std::size_t j = i_ + 1;
            std::string name;
            while (j < src_.size() && isIdentPart(static_cast<unsigned char>(src_[j])))
                name.push_back(src_[j++]);
            while (j < src_.size() && (src_[j] == ' ' || src_[j] == '\t')) ++j;
            if (name == "ts" && j < src_.size() && src_[j] == '{') {
                tok = lexTsBlock(bag);
            } else {
                advance();
                tok.kind = Tok::At;
                tok.pos = start;
                tok.end = here();
                tok.text = "@";
            }
        } else if (c == '#') {
            advance();
            tok.kind = Tok::Hash;
            tok.pos = start;
            tok.end = here();
            tok.text = "#";
        } else {
            // punctuation (longest match)
            bool matched = false;
            for (const char* p : kPuncts) {
                std::size_t n = std::strlen(p);
                if (src_.compare(i_, n, p) == 0) {
                    for (std::size_t k = 0; k < n; ++k) advance();
                    tok.kind = Tok::Punct;
                    tok.pos = start;
                    tok.end = here();
                    tok.text = p;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                advance();
                bag.error(start, "LEX002",
                          std::string("无法识别的字符 `") + static_cast<char>(c) + "`");
                continue;
            }
        }
        tok.newlineBefore = nl;
        out.push_back(tok);
    }

    Token end;
    end.kind = Tok::End;
    end.pos = here();
    end.end = here();
    out.push_back(end);
    return out;
}

}  // namespace shya
