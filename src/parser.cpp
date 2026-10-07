// shya - parser: token stream -> AST.
#include "shya.h"

namespace shya {

// ============================================================ kind names ====

const char* nodeKindName(NK k) {
    switch (k) {
        case NK::Num: return "Num";
        case NK::Str: return "Str";
        case NK::Tpl: return "Tpl";
        case NK::Bool: return "Bool";
        case NK::Void: return "Void";
        case NK::Ident: return "Ident";
        case NK::MathConst: return "MathConst";
        case NK::ArrayLit: return "ArrayLit";
        case NK::ObjectLit: return "ObjectLit";
        case NK::Prop: return "Prop";
        case NK::Unary: return "Unary";
        case NK::Binary: return "Binary";
        case NK::Compare: return "Compare";
        case NK::Ternary: return "Ternary";
        case NK::Call: return "Call";
        case NK::Member: return "Member";
        case NK::Index: return "Index";
        case NK::Spread: return "Spread";
        case NK::Await: return "Await";
        case NK::MacroApply: return "MacroApply";
        case NK::SlotRef: return "SlotRef";
        case NK::SlotList: return "SlotList";
        case NK::RangeExpr: return "RangeExpr";
        case NK::TsRaw: return "TsRaw";
        case NK::Optionalize: return "Optionalize";
        case NK::Assign: return "Assign";
        case NK::TypeRef: return "TypeRef";
        case NK::Program: return "Program";
        case NK::Block: return "Block";
        case NK::ExprStmt: return "ExprStmt";
        case NK::Empty: return "Empty";
        case NK::IncDec: return "IncDec";
        case NK::Decl: return "Decl";
        case NK::Define: return "Define";
        case NK::Declare: return "Declare";
        case NK::If: return "If";
        case NK::Case: return "Case";
        case NK::CaseArm: return "CaseArm";
        case NK::ForWhile: return "ForWhile";
        case NK::ForOf: return "ForOf";
        case NK::ForEach: return "ForEach";
        case NK::ForRange: return "ForRange";
        case NK::FnDecl: return "FnDecl";
        case NK::Return: return "Return";
        case NK::Throw: return "Throw";
        case NK::Try: return "Try";
        case NK::Break: return "Break";
        case NK::Continue: return "Continue";
        case NK::Import: return "Import";
        case NK::Export: return "Export";
        case NK::MacroDecl: return "MacroDecl";
        case NK::When: return "When";
        case NK::WhenArm: return "WhenArm";
        case NK::Each: return "Each";
        case NK::SlotScope: return "SlotScope";
    }
    return "?";
}

namespace {

void dumpNode(const NodePtr& n, std::ostringstream& os, int depth) {
    if (!n) return;
    os << std::string(static_cast<std::size_t>(depth) * 2, ' ') << nodeKindName(n->kind);
    if (!n->text.empty()) os << " '" << n->text << "'";
    if (n->kind == NK::Num || n->kind == NK::MathConst) os << " " << n->num;
    if (n->kind == NK::Str) os << " \"" << n->text << "\"";
    if (n->flag) os << " [flag]";
    if (n->flag2) os << " [flag2]";
    if (!n->typeAnn.empty()) os << " :" << n->typeAnn;
    if (!n->names.empty()) {
        os << " names=[";
        for (std::size_t i = 0; i < n->names.size(); ++i) os << (i ? "," : "") << n->names[i];
        os << "]";
    }
    os << " @" << n->pos.line << ":" << n->pos.col << '\n';
    for (const auto& kid : {n->a, n->b, n->c, n->d}) dumpNode(kid, os, depth + 1);
    for (const auto& t : n->targets) dumpNode(t, os, depth + 1);
    for (const auto& v : n->values) dumpNode(v, os, depth + 1);
    for (const auto& p : n->patterns) dumpNode(p, os, depth + 1);
    for (const auto& s : n->list) dumpNode(s, os, depth + 1);
}

}  // namespace

std::string dumpAst(const NodePtr& root) {
    std::ostringstream os;
    dumpNode(root, os, 0);
    return os.str();
}

// ================================================================ parser ====

Parser::Parser(std::vector<Token> tokens, DiagBag& bag, ParserOptions opt)
    : t_(std::move(tokens)), bag_(bag), opt_(opt) {
    if (t_.empty()) {
        Token e;
        e.kind = Tok::End;
        t_.push_back(e);
    }
}

void Parser::errorHere(const std::string& code, const std::string& msg) {
    bag_.error(cur().pos, code, msg);
}

bool Parser::expectPunct(const char* p, const char* ctx) {
    if (acceptPunct(p)) return true;
    errorHere("SYN001", std::string("期望 `") + p + "`" +
                            (ctx && *ctx ? std::string("（在 ") + ctx + " 中）" : std::string()) +
                            "，但遇到 `" + (cur().kind == Tok::End ? std::string("文件结束") : cur().text) + "`");
    return false;
}

bool Parser::expectKeyword(const char* k, const char* ctx) {
    if (acceptKeyword(k)) return true;
    errorHere("SYN001", std::string("期望关键字 `") + k + "`" +
                            (ctx && *ctx ? std::string("（在 ") + ctx + " 中）" : std::string()) +
                            "，但遇到 `" + (cur().kind == Tok::End ? std::string("文件结束") : cur().text) + "`");
    return false;
}

bool Parser::startsStatement() const {
    const Token& t = cur();
    if (t.kind == Tok::End) return false;
    if (t.kind == Tok::Keyword) {
        static const std::set<std::string> stmtKw = {
            "if", "case", "for", "return", "throw", "try", "break", "continue",
            "import", "export", "fn", "macro", "define", "declare", "let", "const", "async",
        };
        return stmtKw.count(t.text) > 0;
    }
    return true;
}

NodePtr Parser::parseProgram() {
    auto prog = mk(NK::Program, cur().pos);
    while (!atEnd()) {
        if (cur().isPunct(";")) {
            take();
            continue;
        }
        std::size_t before = i_;
        NodePtr st = parseStatement();
        if (st) prog->list.push_back(st);
        if (i_ == before) {
            // guarantee forward progress
            errorHere("SYN002", "无法解析的语句，已跳过 `" + cur().text + "`");
            take();
        }
    }
    return prog;
}

// ------------------------------------------------------------- statements ---

NodePtr Parser::parseBlock() {
    auto b = mk(NK::Block, cur().pos);
    if (!expectPunct("{", "块")) return b;
    while (!atEnd() && !checkPunct("}")) {
        if (cur().isPunct(";")) {
            take();
            continue;
        }
        std::size_t before = i_;
        NodePtr st = parseStatement();
        if (st) b->list.push_back(st);
        if (i_ == before) {
            errorHere("SYN002", "无法解析的语句，已跳过 `" + cur().text + "`");
            take();
        }
    }
    expectPunct("}", "块");
    return b;
}

NodePtr Parser::parseIf() {
    auto n = mk(NK::If, cur().pos);
    expectKeyword("if", "条件语句");
    n->a = parseParenExpr("if");
    n->b = checkPunct("{") ? parseBlock() : parseStatement();
    NodePtr tail = n;
    for (;;) {
        if (checkKeyword("elif")) {
            take();
            auto e = mk(NK::If, cur().pos);
            e->a = parseParenExpr("elif");
            e->b = checkPunct("{") ? parseBlock() : parseStatement();
            tail->c = e;
            tail = e;
            continue;
        }
        if (checkKeyword("else")) {
            take();
            tail->c = checkPunct("{") ? parseBlock() : parseStatement();
        }
        break;
    }
    return n;
}

// `(expr)` or a bare `expr` - the parentheses are optional in shya.
NodePtr Parser::parseParenExpr(const char* ctx) {
    if (acceptPunct("(")) {
        NodePtr e = parseExpression();
        expectPunct(")", ctx);
        return e;
    }
    return parseExpression();
}

NodePtr Parser::parseCase() {
    auto n = mk(NK::Case, cur().pos);
    expectKeyword("case", "分支语句");
    if (checkPunct("(")) {
        take();
        n->a = parseExpression();
        expectPunct(")", "case");
    } else if (!checkPunct("{")) {
        n->a = parseExpression();
    }
    expectPunct("{", "case");
    while (!atEnd() && !checkPunct("}")) {
        if (cur().isPunct(";") || cur().isPunct(",")) {
            take();
            continue;
        }
        auto arm = mk(NK::CaseArm, cur().pos);
        if (acceptKeyword("default")) {
            arm->flag = true;  // default arm
        } else {
            for (;;) {
                // A pattern list is comma separated: each entry is its own label.
                arm->patterns.push_back(parseTernary());
                if (acceptPunct(",")) continue;
                break;
            }
        }
        expectPunct(":", "case 分支");
        // A case arm body is exactly one statement (`;` = empty, `fallthrough`).
        if (acceptKeyword("fallthrough")) {
            arm->flag2 = true;
        } else if (checkPunct(";")) {
            take();
        } else {
            std::size_t before = i_;
            NodePtr body = parseStatement();
            if (body) arm->list.push_back(body);
            if (i_ == before) {
                errorHere("SYN002", "case 分支体解析失败，已跳过 `" + cur().text + "`");
                take();
            }
        }
        n->list.push_back(arm);
    }
    expectPunct("}", "case");
    return n;
}

NodePtr Parser::parseFor() {
    Pos p = cur().pos;
    expectKeyword("for", "循环");
    bool parens = false;
    if (acceptPunct("(")) parens = true;

    // `for x of ...` / `for a b of ...` vs `for(cond)`
    std::size_t save = i_;
    std::vector<std::string> names;
    bool isOf = false;
    if (check(Tok::Identifier) || check(Tok::Under)) {
        names.push_back(cur().kind == Tok::Under ? "_" : take().text);
        if (cur().kind == Tok::Identifier && cur().text != "of") {
            // Second loop variable (`for attr value of ...`)
            names.push_back(take().text);
        }
        if (checkKeyword("of")) isOf = true;
    }
    if (!isOf) {
        i_ = save;
        names.clear();
    }

    if (isOf) {
        expectKeyword("of", "for-of");
        auto n = mk(NK::ForOf, p);
        n->names = names;
        n->a = parseExpression();
        if (parens) expectPunct(")", "for-of");
        n->b = checkPunct("{") ? parseBlock() : parseStatement();
        return n;
    }

    // for(condition) { ... }
    auto n = mk(NK::ForWhile, p);
    n->a = parseExpression();
    if (parens) expectPunct(")", "for");
    n->b = checkPunct("{") ? parseBlock() : parseStatement();
    return n;
}

NodePtr Parser::parseFnDecl(bool isAsync, bool isExport) {
    Pos p = cur().pos;
    expectKeyword("fn", "函数声明");
    auto n = mk(NK::FnDecl, p);
    n->flag = isAsync;
    n->flag2 = isExport;
    if (check(Tok::Identifier)) {
        n->text = take().text;
    } else {
        errorHere("SYN003", "函数声明缺少函数名");
    }
    expectPunct("(", "函数参数");
    while (!atEnd() && !checkPunct(")")) {
        bool rest = acceptPunct("...");
        std::string pname;
        if (check(Tok::Identifier)) {
            pname = take().text;
        } else {
            errorHere("SYN004", "函数参数名无效");
            take();
        }
        std::string ann;
        if (acceptPunct(":")) ann = parseParamAnnotation();
        NodePtr def;
        if (acceptPunct("=")) {
            // No comma expression here, otherwise a following parameter is eaten.
            def = parseTernary();
        }
        n->names.push_back(pname);
        n->typeAnns.push_back(rest ? "..." + ann : ann);
        n->defaults.push_back(def);
        if (!acceptPunct(",")) break;
    }
    expectPunct(")", "函数参数");
    if (acceptPunct(":")) n->typeAnn2 = parseTypeAnnotation();
    if (checkPunct("{")) {
        NodePtr b = parseBlock();
        n->list = b->list;
    }
    return n;
}

NodePtr Parser::parseMacroDecl() {
    Pos p = cur().pos;
    expectKeyword("macro", "宏定义");
    expectPunct("@", "宏定义");
    auto n = mk(NK::MacroDecl, p);
    if (check(Tok::Identifier)) {
        n->text = take().text;
    } else {
        errorHere("SYN005", "宏定义缺少宏名");
    }
    expectPunct("(", "宏参数");
    while (!atEnd() && !checkPunct(")")) {
        bool variadic = acceptPunct("...");
        if (!expectPunct("#", "宏参数")) break;
        std::string pname;
        if (checkName()) {
            pname = takeName();
        } else {
            errorHere("SYN006", "宏参数缺少名字");
            take();
        }
        std::string ann;
        if (acceptPunct(":")) ann = parseTypeAnnotation();
        n->names.push_back(pname);
        n->typeAnns.push_back(ann);
        n->flags.push_back(variadic);
        if (!acceptPunct(",")) break;
    }
    expectPunct(")", "宏参数");
    expectPunct("{", "宏体");
    macroDepth_++;
    parseMacroTemplateBody(n);
    macroDepth_--;
    expectPunct("}", "宏体");
    return n;
}

NodePtr Parser::parseImport() {
    auto n = mk(NK::Import, cur().pos);
    expectKeyword("import", "导入");
    std::size_t start = i_;
    auto isWord = [](const Token& x) {
        return x.kind == Tok::Identifier || x.kind == Tok::Keyword || x.kind == Tok::Number ||
               x.kind == Tok::String || x.kind == Tok::MathLit;
    };
    auto addName = [&](const std::string& name) {
        n->names.push_back(name);
        if (!name.empty() && name[0] == '@') n->flag = true;
    };

    // `import "./x"` / `import x from "./x"` / `import * as ns from "./x"` /
    // `import { a, @macro as m } from "./x"`.
    if (check(Tok::String)) {
        n->text = "sideeffect";
        n->spec = take().value;
    } else if (checkPunct("*")) {
        n->text = "namespace";
        take();
        expectKeyword("as", "import * as");
        if (check(Tok::Identifier)) addName(take().text);
        expectKeyword("from", "import");
        if (check(Tok::String)) n->spec = take().value;
        else errorHere("SYN027", "`import` 缺少模块路径字符串");
    } else if (checkPunct("{")) {
        n->text = "named";
        take();
        while (!atEnd() && !checkPunct("}")) {
            std::string prefix;
            if (acceptPunct("@")) prefix = "@";
            if (check(Tok::Identifier) || check(Tok::Keyword)) {
                std::string name = prefix + take().text;
                if (acceptKeyword("as")) {
                    if (check(Tok::Identifier)) take();
                    else errorHere("SYN028", "`as` 后面需要新名字");
                }
                addName(name);
            } else {
                errorHere("SYN029", "`import { }` 里需要名字");
                take();
            }
            if (!acceptPunct(",")) break;
        }
        expectPunct("}", "import");
        expectKeyword("from", "import");
        if (check(Tok::String)) n->spec = take().value;
        else errorHere("SYN027", "`import` 缺少模块路径字符串");
    } else {
        n->text = "default";
        if (check(Tok::Identifier)) addName(take().text);
        else errorHere("SYN029", "`import` 后面需要名字或模块路径");
        if (acceptKeyword("as")) {
            if (check(Tok::Identifier)) take();
        }
        expectKeyword("from", "import");
        if (check(Tok::String)) n->spec = take().value;
        else errorHere("SYN027", "`import` 缺少模块路径字符串");
    }
    acceptPunct(";");

    // Rebuild the verbatim statement text for plain JavaScript passthrough.
    std::string raw = "import ";
    for (std::size_t k = start; k < i_; ++k) {
        const Token& t = t_[k];
        if (t.isPunct(";")) break;
        bool needSpace = !raw.empty() && isWord(t) && raw.back() != '{' && raw.back() != '(' &&
                         raw.back() != '[' && raw.back() != ',' && raw.back() != ' ' &&
                         raw.back() != '*';
        if (needSpace) raw += ' ';
        raw += t.text;
    }
    n->raw = raw;
    n->flag2 = n->spec.size() > 5 && n->spec.compare(n->spec.size() - 5, 5, ".shya") == 0;
    return n;
}
NodePtr Parser::parseTry() {
    auto n = mk(NK::Try, cur().pos);
    expectKeyword("try", "try");
    n->a = checkPunct("{") ? parseBlock() : parseStatement();
    std::string errName;
    if (acceptKeyword("catch")) {
        if (acceptPunct("(")) {
            if (check(Tok::Identifier)) errName = take().text;
            expectPunct(")", "catch");
        } else if (check(Tok::Identifier) && peek().isPunct("{")) {
            errName = take().text;
        }
        n->text = errName;
        n->b = checkPunct("{") ? parseBlock() : parseStatement();
    }
    if (acceptKeyword("finally")) {
        n->c = checkPunct("{") ? parseBlock() : parseStatement();
    }
    return n;
}

NodePtr Parser::parseVarDecl(const std::string& kw) {
    Pos p = cur().pos;
    take();  // 'let' | 'const'
    auto n = mk(NK::Decl, p);
    n->text = kw;
    for (;;) {
        auto d = mk(NK::Decl, cur().pos);
        d->text = kw;
        if (check(Tok::Identifier)) {
            d->text = kw;
            d->names.push_back(take().text);
        } else {
            errorHere("SYN007", "`" + kw + "` 后面需要变量名");
            break;
        }
        if (acceptPunct(":")) d->typeAnn = parseTypeAnnotation();
        if (acceptPunct("=")) d->a = parseExpression();
        n->list.push_back(d);
        if (!acceptPunct(",")) break;
    }
    acceptPunct(";");
    return n;
}

NodePtr Parser::parseDefine() {
    Pos p = cur().pos;
    expectKeyword("define", "编译期常量");
    auto n = mk(NK::Define, p);
    if (check(Tok::Identifier)) {
        n->text = take().text;
    } else {
        errorHere("SYN008", "`define` 后面需要常量名");
    }
    if (acceptPunct(":")) n->typeAnn = parseTypeAnnotation();
    expectPunct("=", "define");
    n->a = parseExpression();
    acceptPunct(";");
    return n;
}

NodePtr Parser::parseDeclare() {
    Pos p = cur().pos;
    expectKeyword("declare", "外置声明");
    auto n = mk(NK::Declare, p);

    // `declare fn name(params): Ret` -- a host function signature.
    if (checkKeyword("fn")) {
        take();
        n->flag = true;
        if (check(Tok::Identifier)) n->text = take().text;
        else errorHere("SYN023", "`declare fn` 缺少函数名");
        expectPunct("(", "declare fn");
        while (!atEnd() && !checkPunct(")")) {
            bool rest = acceptPunct("...");
            std::string pname = check(Tok::Identifier) ? take().text : std::string();
            if (pname.empty()) errorHere("SYN004", "参数名无效");
            std::string ann;
            if (acceptPunct(":")) ann = parseParamAnnotation();
            NodePtr def;
            if (acceptPunct("=")) def = parseTernary();
            n->names.push_back(pname);
            n->typeAnns.push_back(rest ? "..." + ann : ann);
            n->defaults.push_back(def);
            if (!acceptPunct(",")) break;
        }
        expectPunct(")", "declare fn");
        n->typeAnn2 = acceptPunct(":") ? parseTypeAnnotation() : std::string("void");
        acceptPunct(";");
        return n;
    }

    // `declare Name { field: T  method(a: T): R }`
    if (check(Tok::Identifier)) n->text = take().text;
    else errorHere("SYN024", "`declare` 后面需要类型名或 `fn`");
    if (acceptPunct("<")) {
        errorHere("SYN025", "`declare` 暂不支持泛型参数");
        while (!atEnd() && !checkPunct(">")) take();
        expectPunct(">", "declare 泛型参数");
    }
    expectPunct("{", "declare");
    while (!atEnd() && !checkPunct("}")) {
        if (cur().isPunct(";") || cur().isPunct(",")) {
            take();
            continue;
        }
        auto m = mk(NK::Prop, cur().pos);
        if (check(Tok::Identifier) || check(Tok::Keyword)) m->text = take().text;
        else {
            errorHere("SYN026", "`declare` 成员缺少名字");
            take();
            continue;
        }
        if (acceptPunct("?")) m->flag2 = true;  // optional field
        if (checkPunct("(")) {
            m->flag = true;  // method
            take();
            while (!atEnd() && !checkPunct(")")) {
                bool rest = acceptPunct("...");
                std::string pname = check(Tok::Identifier) ? take().text : std::string();
                std::string ann;
                if (acceptPunct(":")) ann = parseParamAnnotation();
                NodePtr def;
                if (acceptPunct("=")) def = parseTernary();
                m->names.push_back(pname);
                m->typeAnns.push_back(rest ? "..." + ann : ann);
                m->defaults.push_back(def);
                if (!acceptPunct(",")) break;
            }
            expectPunct(")", "declare 方法");
            m->typeAnn = acceptPunct(":") ? parseTypeAnnotation() : std::string("void");
        } else {
            expectPunct(":", "declare 字段");
            m->typeAnn = parseTypeAnnotation();
        }
        n->list.push_back(m);
        acceptPunct(";");
        acceptPunct(",");
    }
    expectPunct("}", "declare");
    return n;
}

NodePtr Parser::parseStatement() {
    const Token& t = cur();

    if (t.isPunct(";")) {
        take();
        return mk(NK::Empty, t.pos);
    }
    if (t.isPunct("{")) {
        // A statement that begins with `{` is a block unless it is unmistakably a
        // non-empty object literal. The code generator parenthesises such a statement,
        // because JavaScript would otherwise read `{ x: 1 };` as a block with a label.
        if (looksLikeObjectLit(false)) {
            auto st = mk(NK::ExprStmt, t.pos);
            st->a = parseObjectLit();
            acceptPunct(";");
            return st;
        }
        return parseBlock();
    }
    // `<slot> … </slot>`. `<` cannot begin an expression, so at a statement position it
    // is unambiguously a scope marker; `a < b` is untouched because it starts with `a`.
    if (t.isPunct("<")) return parseSlotScope(false);
    if (t.kind == Tok::Under && peek().kind != Tok::At) {
        take();
        acceptPunct(";");
        return mk(NK::Empty, t.pos);
    }
    if (t.kind == Tok::Keyword) {
        const std::string& k = t.text;
        if (k == "if") return parseIf();
        if (k == "case") return parseCase();
        if (k == "for") return parseFor();
        if (k == "try") return parseTry();
        if (k == "fn") return parseFnDecl(false, false);
        if (k == "async") {
            if (peek().isKeyword("fn")) {
                take();
                return parseFnDecl(true, false);
            }
            errorHere("SYN009", "`async` 只能用在 `fn` 前面");
            take();
            return nullptr;
        }
        if (k == "macro") return parseMacroDecl();
        if (k == "define") return parseDefine();
        if (k == "declare") return parseDeclare();
        if (k == "let") return parseVarDecl("let");
        if (k == "const") return parseVarDecl("const");
        if (k == "import") return parseImport();
        if (k == "return") {
            auto n = mk(NK::Return, t.pos);
            take();
            if (!checkPunct(";") && !checkPunct("}") && !atEnd()) n->a = parseExpression();
            acceptPunct(";");
            return n;
        }
        if (k == "throw") {
            auto n = mk(NK::Throw, t.pos);
            take();
            n->a = parseExpression();
            acceptPunct(";");
            return n;
        }
        if (k == "break" || k == "continue") {
            auto n = mk(k == "break" ? NK::Break : NK::Continue, t.pos);
            take();
            if (check(Tok::Identifier)) n->text = take().text;
            acceptPunct(";");
            return n;
        }
        if (k == "export") {
            auto n = mk(NK::Export, t.pos);
            take();
            if (acceptKeyword("default")) {
                n->flag = true;
                n->a = parseExpression();
                acceptPunct(";");
                return n;
            }
            if (checkPunct("{") || checkPunct("*")) {
                std::string raw;
                while (!atEnd() && !checkPunct(";")) {
                    const Token& x = cur();
                    if (!raw.empty() && x.kind != Tok::Punct) raw += ' ';
                    raw += x.text;
                    take();
                }
                acceptPunct(";");
                n->raw = raw;
                return n;
            }
            std::size_t before = i_;
            NodePtr inner = parseStatement();
            if (i_ == before) {
                errorHere("SYN010", "`export` 后面需要声明或表达式");
                take();
                return n;
            }
            n->a = inner;
            return n;
        }
    }

    // `x: Type = expr` - declaration with an explicit annotation.
    if (t.kind == Tok::Identifier && peek().isPunct(":")) {
        std::size_t save = i_;
        std::string name = take().text;
        take();  // ':'
        std::string ann = parseTypeAnnotation();
        if (acceptPunct("=")) {
            auto n = mk(NK::Decl, t.pos);
            n->text = "infer";
            auto d = mk(NK::Decl, t.pos);
            d->text = "infer";
            d->names.push_back(name);
            d->typeAnn = ann;
            d->a = parseExpression();
            n->list.push_back(d);
            acceptPunct(";");
            return n;
        }
        i_ = save;
    }

    // Expression / assignment statement (possibly with trailing macro splices
    // inside a macro template body).
    auto st = mk(NK::ExprStmt, t.pos);
    NodePtr first = parseExpression();

    std::vector<NodePtr> parts;
    parts.push_back(first);
    if (macroDepth_ > 0) {
        // `#x @each(...)` continues the same line; a `@when` on the next line
        // starts a new statement.
        while (cur().kind == Tok::At && !cur().newlineBefore &&
               (peek().isIdent("each") || peek().isIdent("when"))) {
            if (peek().isIdent("each")) parts.push_back(parseTemplateEach());
            else parts.push_back(parseTemplateWhen());
        }
    }
    if (parts.size() == 1) {
        // Assignment and self-increment are statements, not expressions.
        if (first->kind == NK::Assign || first->kind == NK::IncDec) {
            acceptPunct(";");
            return first;
        }
        st->a = first;
    } else {
        // Fragment concatenation: the head is an expression, the rest are
        // chain-link fragments appended to it.
        auto cat = mk(NK::Binary, t.pos);
        cat->text = "concat";
        cat->a = parts[0];
        for (std::size_t i = 1; i < parts.size(); ++i) {
            if (!cat->b) {
                cat->b = parts[i];
            } else {
                auto nx = mk(NK::Binary, t.pos);
                nx->text = "concat";
                nx->a = cat;
                nx->b = parts[i];
                cat = nx;
            }
        }
        st->a = cat;
    }
    acceptPunct(";");
    return st;
}

// ------------------------------------------------------------ macro body ----

void Parser::parseMacroTemplateBody(NodePtr body) {
    while (!atEnd() && !checkPunct("}")) {
        if (cur().isPunct(";") || cur().isPunct(",")) {
            take();
            continue;
        }
        std::size_t before = i_;
        NodePtr st = parseStatement();
        if (st) body->list.push_back(st);
        if (i_ == before) {
            errorHere("SYN002", "宏体内无法解析的内容，已跳过 `" + cur().text + "`");
            take();
        }
    }
}

NodePtr Parser::parseMacroBody() {
    auto b = mk(NK::Program, cur().pos);
    macroDepth_++;
    parseMacroTemplateBody(b);
    macroDepth_--;
    return b;
}

// ------------------------------------------------------------ expressions ---

NodePtr Parser::parseExpression() { return parseAssignment(); }

NodePtr Parser::parseAssignment() {
    Pos p = cur().pos;
    // Parse a comma separated target list, then check for '='.
    std::vector<NodePtr> first;
    first.push_back(parseTernary());
    while (acceptPunct(",")) first.push_back(parseTernary());
    // Compound assignment: `x += 1`, `x ^= 2` ...
    if (first.size() == 1) {
        static const char* kCompound[] = {"+=", "-=", "*=", "/=", "%=", "^=", "**="};
        for (const char* op : kCompound) {
            if (checkPunct(op)) {
                take();
                auto n = mk(NK::Assign, p);
                n->text = std::string(op, std::strlen(op) - 1);
                n->targets = first;
                n->values.push_back(parseAssignment());
                return n;
            }
        }
    }
    if (checkPunct("=")) {
        take();
        auto n = mk(NK::Assign, p);
        n->targets = first;
        n->values.push_back(parseAssignment());
        while (acceptPunct(",")) n->values.push_back(parseAssignment());
        // `a, b = b, a` parses the right side as one comma sequence; split it so
        // the assignment stays simultaneous. `seq` keeps the node alive while
        // its list is copied out.
        if (n->values.size() == 1 && n->values[0] && n->values[0]->kind == NK::ArrayLit &&
            n->values[0]->flag) {
            NodePtr seq = n->values[0];
            n->values = seq->list;
        }
        return n;
    }
    if (first.size() == 1) return first[0];
    // A bare comma list outside an assignment: only meaningful inside `for`.
    auto seq = mk(NK::ArrayLit, p);
    seq->flag = true;  // sequence, not a real array
    seq->list = first;
    return seq;
}

NodePtr Parser::parseTernary() {
    NodePtr cond = parseLogicalOr();
    if (checkPunct("?") && !peek().isPunct("(")) {
        Pos p = cur().pos;
        take();
        auto n = mk(NK::Ternary, p);
        n->a = cond;
        n->b = parseAssignment();
        expectPunct(":", "三元表达式");
        n->c = parseAssignment();
        return n;
    }
    return cond;
}

NodePtr Parser::parseLogicalOr() {
    NodePtr l = parseLogicalAnd();
    while (checkPunct("||")) {
        Pos p = cur().pos;
        take();
        auto n = mk(NK::Binary, p);
        n->text = "||";
        n->a = l;
        n->b = parseLogicalAnd();
        l = n;
    }
    return l;
}

NodePtr Parser::parseLogicalAnd() {
    NodePtr l = parseEquality();
    while (checkPunct("&&")) {
        Pos p = cur().pos;
        take();
        auto n = mk(NK::Binary, p);
        n->text = "&&";
        n->a = l;
        n->b = parseEquality();
        l = n;
    }
    return l;
}

NodePtr Parser::parseEquality() {
    NodePtr l = parseRelational();
    for (;;) {
        // Recognise the comparison operators, including the two-word forms
        // `is not` and `not instanceof`. Returns how many tokens to consume.
        auto readCompareOp = [&](std::string& op) -> int {
            if (checkPunct("==")) op = "==";
            else if (checkPunct("!=")) op = "!=";
            else if (checkPunct("===")) op = "===";
            else if (checkPunct("!==")) op = "!==";
            else if (checkKeyword("is")) {
                op = peek().isKeyword("not") ? "is not" : "is";
                return op == "is not" ? 2 : 1;
            } else if (checkKeyword("instanceof")) op = "instanceof";
            else if (checkKeyword("not") && peek().isKeyword("instanceof")) {
                op = "not instanceof";
                return 2;
            } else if (checkKeyword("not") && peek().isKeyword("is")) {
                op = "is not";
                return 2;
            } else {
                return 0;
            }
            return 1;
        };

        std::string op;
        int consume = readCompareOp(op);
        if (consume == 0) break;
        Pos p = cur().pos;
        for (int k = 0; k < consume; ++k) take();
        auto n = mk(NK::Compare, p);
        n->text = op;
        n->a = l;
        n->b = (op == "is" || op == "is not") ? parseTypeNameRHS() : parseRelational();
        // Chained comparison: `1 < 2 < 3` becomes a Compare chain.
        std::vector<NodePtr> ops{l};
        std::vector<NodePtr> rhs{n->b};
        std::vector<std::string> names{n->text};
        for (;;) {
            std::string op2;
            int consume2 = readCompareOp(op2);
            if (consume2 == 0) break;
            for (int k = 0; k < consume2; ++k) take();
            ops.push_back(rhs.back());
            rhs.push_back((op2 == "is" || op2 == "is not") ? parseTypeNameRHS()
                                                           : parseRelational());
            names.push_back(op2);
        }
        auto chain = mk(NK::Compare, p);
        chain->a = ops[0];
        chain->list = rhs;
        chain->names = names;
        chain->text = names[0];
        l = chain;
    }
    return l;
}

// The right hand side of `is` names a type; some type names are keywords.
NodePtr Parser::parseTypeNameRHS() {
    static const std::set<std::string> typeKeywords = {
        "fn", "void", "null", "undefined", "number", "string", "boolean", "object",
    };
    const Token& t = cur();
    if (t.kind == Tok::Identifier ||
        (t.kind == Tok::Keyword && typeKeywords.count(t.text) > 0)) {
        auto n = mk(NK::Ident, t.pos);
        n->text = take().text;
        return n;
    }
    return parseRelational();
}

NodePtr Parser::parseRelational() {    NodePtr l = parseAdditive();
    // `</` closes a `<slot>` scope. It is not an operator in any expression, so it must
    // never be read as `<` followed by a division: `key: value</slot>` has to end the
    // value at the `<`.
    auto startsScopeClose = [&]() { return checkPunct("<") && peek().isPunct("/"); };
    for (;;) {
        std::string op;
        if (startsScopeClose()) break;
        // A relational operator never continues across a line break, exactly like member
        // access and postfix macros. Without this, `keep: 1` on one line followed by
        // `<translation>…</translation>` on the next reads as `1 < translation`.
        if (cur().newlineBefore) break;
        if (checkPunct("<")) op = "<";
        else if (checkPunct("<=")) op = "<=";
        else if (checkPunct(">")) op = ">";
        else if (checkPunct(">=")) op = ">=";
        else break;
        Pos p = cur().pos;
        take();
        std::vector<NodePtr> rhs{parseAdditive()};
        std::vector<std::string> names{op};
        for (;;) {
            std::string op2;
            if (startsScopeClose()) break;
            if (cur().newlineBefore) break;
            if (checkPunct("<")) op2 = "<";
            else if (checkPunct("<=")) op2 = "<=";
            else if (checkPunct(">")) op2 = ">";
            else if (checkPunct(">=")) op2 = ">=";
            else break;
            take();
            rhs.push_back(parseAdditive());
            names.push_back(op2);
        }
        auto chain = mk(NK::Compare, p);
        chain->a = l;
        chain->list = rhs;
        chain->names = names;
        chain->text = names[0];
        l = chain;
    }
    return l;
}

NodePtr Parser::parseAdditive() {
    NodePtr l = parseMultiplicative();
    while (checkPunct("+") || checkPunct("-")) {
        Pos p = cur().pos;
        std::string op = take().text;
        auto n = mk(NK::Binary, p);
        n->text = op;
        n->a = l;
        n->b = parseMultiplicative();
        l = n;
    }
    return l;
}

NodePtr Parser::parseMultiplicative() {
    NodePtr l = parsePower();
    for (;;) {
        std::string op;
        if (checkPunct("*")) op = "*";
        else if (checkPunct("/")) op = "/";
        else if (checkPunct("~/")) op = "~/";
        else if (checkPunct("+/")) op = "+/";
        else if (checkPunct("-/")) op = "-/";
        else if (checkPunct("\\/")) op = "\\/";
        else if (checkPunct("%")) op = "%";
        else break;
        Pos p = cur().pos;
        take();
        auto n = mk(NK::Binary, p);
        n->text = op;
        n->a = l;
        n->b = parsePower();
        l = n;
    }
    return l;
}

NodePtr Parser::parsePower() {
    NodePtr base = parseUnary();
    if (checkPunct("^")) {
        Pos p = cur().pos;
        take();
        auto n = mk(NK::Binary, p);
        n->text = "^";
        n->a = base;
        n->b = parsePower();  // right associative
        return n;
    }
    return base;
}

NodePtr Parser::parseUnary() {
    const Token& t = cur();
    if (t.isPunct("!") || t.isKeyword("not")) {
        // `not instanceof` is handled by the equality layer.
        if (t.isKeyword("not") && peek().isKeyword("instanceof")) return parsePostfix();
        Pos p = t.pos;
        take();
        auto n = mk(NK::Unary, p);
        n->text = "!";
        n->a = parseUnary();
        return n;
    }
    if (t.isPunct("-") || t.isPunct("+")) {
        Pos p = t.pos;
        std::string op = take().text;
        auto n = mk(NK::Unary, p);
        n->text = op;
        n->a = parseUnary();
        return n;
    }
    if (t.isPunct("++") || t.isPunct("--")) {
        Pos p = t.pos;
        auto n = mk(NK::IncDec, p);
        n->text = take().text;
        n->flag = true;  // prefix form
        n->a = parseUnary();
        return n;
    }
    if (t.isPunct("...")) {
        Pos p = t.pos;
        take();
        auto n = mk(NK::Spread, p);
        n->a = parseUnary();
        return n;
    }
    if (t.isKeyword("await")) {
        Pos p = t.pos;
        take();
        auto n = mk(NK::Await, p);
        n->a = parseUnary();
        return n;
    }
    if (t.isKeyword("typeof")) {
        Pos p = t.pos;
        take();
        auto n = mk(NK::Unary, p);
        n->text = "typeof";
        n->a = parseUnary();
        return n;
    }
    if (t.isKeyword("new")) {
        Pos p = t.pos;
        take();
        auto n = mk(NK::Unary, p);
        n->text = "new";
        n->a = parsePostfix();
        return n;
    }
    // '?' prefix inside macro templates: optionalize the following fragment.
    if (t.isPunct("?") && macroDepth_ > 0 && !peek().isPunct("(")) {
        Pos p = t.pos;
        take();
        auto n = mk(NK::Optionalize, p);
        n->a = parseUnary();
        return n;
    }
    return parsePostfix();
}

NodePtr Parser::parsePostfix() {
    NodePtr e = parsePrimary();
    for (;;) {
        // A member access never continues across a line break: shya uses line
        // breaks as statement separators (`console log(1)` / `console log(2)`).
        if (cur().kind == Tok::Identifier && !cur().newlineBefore) {
            Pos p = cur().pos;
            std::string name = take().text;
            auto m = mk(NK::Member, p);
            m->a = e;
            m->text = name;
            if (checkPunct("(")) {
                m->flag = true;
                m->list = parseCallArgs(m, false)->list;
            }
            e = m;
            continue;
        }
        if (cur().kind == Tok::Hash && !cur().newlineBefore) {
            Pos p = cur().pos;
            if (macroDepth_ == 0) {
                errorHere("SYN011", "`#名字` 只能出现在宏定义体内");
            }
            take();
            std::string name;
            if (checkName()) name = takeName();
            else errorHere("SYN012", "`#` 后面需要插槽名");
            auto slot = mk(NK::SlotRef, p);
            slot->text = name;
            auto m = mk(NK::Member, p);
            m->a = e;
            m->b = slot;
            if (checkPunct("(")) {
                m->flag = true;
                m->list = parseCallArgs(m, false)->list;
            }
            e = m;
            continue;
        }
        if (checkPunct("(")) {
            e = parseCallArgs(e, false);
            continue;
        }
        if (checkPunct("?")) {
            // safe call: callee?(args)
            if (peek().isPunct("(")) {
                take();
                e = parseCallArgs(e, true);
                continue;
            }
            break;
        }
        if (checkPunct("[")) {
            Pos p = cur().pos;
            take();
            auto n = mk(NK::Index, p);
            n->a = e;
            n->b = parseTernary();
            expectPunct("]", "下标");
            e = n;
            continue;
        }
        if (cur().kind == Tok::At && !cur().newlineBefore && peek().kind == Tok::Identifier) {
            // '@each' / '@when' are template splices, handled by the caller.
            if (peek().isIdent("each") || peek().isIdent("when")) break;
            NodePtr app = parseMacroApply(cur(), false);
            app->a = e;  // the postfix target becomes the macro's first argument
            e = app;
            continue;
        }
        if (checkPunct("++") || checkPunct("--")) {
            // Self-increment is a statement, never part of a larger expression.
            Pos p = cur().pos;
            auto n = mk(NK::IncDec, p);
            n->text = take().text;
            n->a = e;
            e = n;
            break;
        }
        break;
    }
    return e;
}

NodePtr Parser::parseCallArgs(NodePtr callee, bool safe) {
    Pos p = cur().pos;
    auto n = mk(NK::Call, p);
    n->flag = safe;
    n->a = callee;
    expectPunct("(", "参数列表");
    while (!atEnd() && !checkPunct(")")) {
        // Comma is the argument separator here, so no comma expression.
        n->list.push_back(parseTernary());
        if (!acceptPunct(",")) break;
    }
    expectPunct(")", "参数列表");
    return n;
}

NodePtr Parser::parseArrayLit() {
    auto n = mk(NK::ArrayLit, cur().pos);
    expectPunct("[", "数组字面量");
    while (!atEnd() && !checkPunct("]")) {
        n->list.push_back(parseTernary());
        if (!acceptPunct(",")) break;
    }
    expectPunct("]", "数组字面量");
    return n;
}

NodePtr Parser::parseObjectLit() {
    auto n = mk(NK::ObjectLit, cur().pos);
    expectPunct("{", "对象字面量");
    while (!atEnd() && !checkPunct("}")) {
        if (cur().isPunct(",") || cur().isPunct(";")) {
            take();
            continue;
        }
        // `<slot> … </slot>` inside an object literal contributes members conditionally.
        if (cur().isPunct("<")) {
            n->list.push_back(parseSlotScope(true));
            acceptPunct(",");
            continue;
        }
        NodePtr prop = parseObjectMember();
        if (!prop) break;
        n->list.push_back(prop);
        // The separator is optional: a member followed directly by `</scope>` is as
        // natural as one followed by a comma.
        acceptPunct(",");
    }
    expectPunct("}", "对象字面量");
    return n;
}

// One `key: value` or `key(params) { body }` member. Returns null after reporting, so the
// caller can stop instead of looping on an unparsable token.
NodePtr Parser::parseObjectMember() {
    auto prop = mk(NK::Prop, cur().pos);
    // `async name(params) { … }` — the async modifier on a method definition.
    // `{ async: 1 }` is still a key/value pair whose key happens to be `async`,
    // because the token after it is a colon rather than a member name.
    bool isAsync = false;
    if (cur().kind == Tok::Keyword && cur().text == "async" &&
        (peek().kind == Tok::Identifier || peek().kind == Tok::Keyword)) {
        isAsync = true;
        take();
    }
    if (cur().kind == Tok::Identifier || cur().kind == Tok::Keyword) {
        prop->text = take().text;
    } else if (cur().kind == Tok::String) {
        prop->text = take().value;
    } else {
        errorHere("SYN013", "对象字面量的键必须是标识符或字符串");
        return nullptr;
    }
    if (acceptPunct(":")) {
        prop->a = parseTernary();
    } else if (checkPunct("(")) {
        // Method definition: `key(params) { body }`.
        //
        // This is how a host object that carries behaviour is written - a noname
        // skill object is exactly `{ trigger: {…}, filter(event, player) {…}, … }` -
        // so it is part of the object literal grammar. It is still a *member*, not a
        // key/value pair: the value is the method itself.
        auto fn = mk(NK::FnDecl, prop->pos);
        fn->text = prop->text;
        fn->flag = isAsync;
        expectPunct("(", "方法参数");
        while (!atEnd() && !checkPunct(")")) {
            bool rest = acceptPunct("...");
            std::string pname = check(Tok::Identifier) ? take().text : std::string();
            std::string ann;
            if (acceptPunct(":")) ann = parseParamAnnotation();
            fn->names.push_back(pname);
            fn->typeAnns.push_back(rest ? "..." + ann : ann);
            fn->defaults.push_back(nullptr);
            if (!acceptPunct(",")) break;
        }
        expectPunct(")", "方法参数");
        if (acceptPunct(":")) fn->typeAnn2 = parseTypeAnnotation();
        if (checkPunct("{")) fn->list = parseBlock()->list;
        prop->a = fn;
    } else {
        // Shorthand `{ a }` is not part of the language: the key and the value are
        // always both written.
        bag_.error(prop->pos, "SYN030",
                   "对象字面量只允许键值对，不支持简写；请写成 `" + prop->text + ": " +
                       prop->text + "`");
        auto id = mk(NK::Ident, prop->pos);
        id->text = prop->text;
        prop->a = id;
    }
    return prop;
}

// `</name>`, i.e. `<` `/` name `>`.
bool Parser::atScopeClose(const std::string& name) const {
    if (!checkPunct("<") || !peek().isPunct("/")) return false;
    const Token& tk = peek(2);
    if (tk.kind != Tok::Identifier && tk.kind != Tok::Keyword) return false;
    if (!name.empty() && tk.text != name) return false;
    return peek(3).isPunct(">");
}

// `<slot> … </slot>`: a scope that disappears entirely when the slot was not supplied at
// the call site. It may hold statements or, inside an object literal, members - which is
// the case it exists for, since a member cannot otherwise be made conditional.
NodePtr Parser::parseSlotScope(bool memberPosition) {
    const Pos p = cur().pos;
    if (macroDepth_ == 0) {
        bag_.error(p, "SYN035", "`<插槽> … </插槽>` 只能出现在宏定义体内");
    }
    auto n = mk(NK::SlotScope, p);
    expectPunct("<", "插槽作用域");
    if (cur().kind == Tok::Identifier || cur().kind == Tok::Keyword) {
        n->text = take().text;
    } else {
        errorHere("SYN033", "`<` 后面需要插槽名（不带 `#`）");
        while (!atEnd() && !checkPunct(">")) take();
        acceptPunct(">");
        return n;
    }
    expectPunct(">", "插槽作用域");

    if (memberPosition) {
        while (!atEnd() && !atScopeClose(n->text) && !checkPunct("}")) {
            if (cur().isPunct(",") || cur().isPunct(";")) {
                take();
                continue;
            }
            if (cur().isPunct("<")) {
                n->list.push_back(parseSlotScope(true));
                acceptPunct(",");
                continue;
            }
            NodePtr prop = parseObjectMember();
            if (!prop) break;
            n->list.push_back(prop);
            acceptPunct(",");
        }
    } else {
        while (!atEnd() && !atScopeClose(n->text)) {
            if (cur().isPunct(";")) {
                take();
                continue;
            }
            std::size_t before = i_;
            NodePtr st = parseStatement();
            if (st) n->list.push_back(st);
            if (i_ == before) {
                errorHere("SYN002", "`<" + n->text + ">` 内无法解析的内容，已跳过 `" +
                                        cur().text + "`");
                take();
            }
        }
    }

    if (atScopeClose(n->text)) {
        take();  // '<'
        take();  // '/'
        take();  // name
        expectPunct(">", "插槽作用域结束");
    } else {
        bag_.error(p, "SYN034", "`<" + n->text + ">` 没有对应的 `</" + n->text + ">`");
    }
    return n;
}

NodePtr Parser::parseRangeOrExpr(bool allowRange) {
    if (allowRange) {
        Pos p = cur().pos;
        NodePtr start = parseTernary();
        if (acceptPunct(":")) {
            auto r = mk(NK::RangeExpr, p);
            r->a = start;
            r->b = parseTernary();
            // The step is part of the range literal: `start:end,step`.
            bool numeric = peek().kind == Tok::Number || peek().kind == Tok::MathLit ||
                           (peek().isPunct("-") &&
                            (peek(2).kind == Tok::Number || peek(2).kind == Tok::MathLit));
            if (checkPunct(",") && numeric) {
                take();
                r->c = parseUnary();
            }
            return r;
        }
        return start;
    }
    return parseTernary();
}

NodePtr Parser::parseMacroArgList() { return nullptr; }

NodePtr Parser::parsePrimary() {
    const Token& t = cur();
    switch (t.kind) {
        case Tok::Number: {
            auto n = mk(NK::Num, t.pos);
            n->num = t.num;
            n->text = t.text;
            take();
            return n;
        }
        case Tok::MathLit: {
            auto n = mk(NK::MathConst, t.pos);
            n->num = t.num;
            n->text = t.text;
            take();
            return n;
        }
        case Tok::String: {
            auto n = mk(NK::Str, t.pos);
            n->text = t.value;
            take();
            return n;
        }
        case Tok::TemplateString: {
            auto n = mk(NK::Tpl, t.pos);
            n->raw = t.text;
            take();
            // Split `${...}` holes and parse each as a shya expression.
            const std::string& raw = n->raw;
            std::size_t i = 1;  // skip the opening backtick
            std::string lit;
            while (i < raw.size()) {
                char c = raw[i];
                if (c == '\\' && i + 1 < raw.size()) {
                    lit.push_back(c);
                    lit.push_back(raw[i + 1]);
                    i += 2;
                    continue;
                }
                if (c == '`') break;
                if (c == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
                    n->names.push_back(lit);
                    lit.clear();
                    i += 2;
                    int depth = 1;
                    std::string exprSrc;
                    while (i < raw.size() && depth > 0) {
                        char d = raw[i];
                        if (d == '{') ++depth;
                        else if (d == '}') {
                            --depth;
                            if (depth == 0) {
                                ++i;
                                break;
                            }
                        }
                        exprSrc.push_back(d);
                        ++i;
                    }
                    Lexer subLex(exprSrc, "<template>");
                    auto toks = subLex.tokenize(bag_);
                    Parser subParser(toks, bag_);
                    subParser.macroDepth_ = macroDepth_;
                    n->list.push_back(subParser.parseExpression());
                    continue;
                }
                lit.push_back(c);
                ++i;
            }
            n->names.push_back(lit);
            return n;
        }
        case Tok::TsBlock: {
            auto n = mk(NK::TsRaw, t.pos);
            n->raw = t.raw;
            take();
            return n;
        }
        case Tok::Under: {
            auto n = mk(NK::Ident, t.pos);
            n->text = "_";
            n->flag = true;  // placeholder
            take();
            return n;
        }
        case Tok::Hash: {
            Pos p = t.pos;
            if (macroDepth_ == 0) bag_.error(p, "SYN011", "`#名字` 只能出现在宏定义体内");
            take();
            std::string name;
            if (checkName()) name = takeName();
            else errorHere("SYN012", "`#` 后面需要插槽名");
            auto n = mk(NK::SlotRef, p);
            n->text = name;
            return n;
        }
        case Tok::At: {
            if (peek().isIdent("each")) return parseTemplateEach();
            if (peek().isIdent("when")) return parseTemplateWhen();
            if (peek().isIdent("ts")) {
                // '@ts' not followed by '{' - the lexer only makes a block for
                // the '{' form, so anything else is a mistake.
                errorHere("SYN014", "`@ts` 后面必须紧跟 `{`");
                take();
                take();
                return mk(NK::Empty, t.pos);
            }
            return parseMacroApply(t, true);
        }
        case Tok::Punct: {
            if (t.text == "(") {
                take();
                NodePtr e = parseExpression();
                expectPunct(")", "括号表达式");
                return e;
            }
            if (t.text == "[") return parseArrayLit();
            if (t.text == "{") return parseObjectLit();
            break;
        }
        case Tok::Keyword: {
            if (t.text == "true" || t.text == "false") {
                auto n = mk(NK::Bool, t.pos);
                n->flag = (t.text == "true");
                take();
                return n;
            }
            if (t.text == "void" || t.text == "null" || t.text == "undefined") {
                auto n = mk(NK::Void, t.pos);
                take();
                return n;
            }
            if (t.text == "this") {
                auto n = mk(NK::Ident, t.pos);
                n->text = "this";
                take();
                return n;
            }
            if (t.text == "fn") {
                // anonymous function expression
                return parseFnDecl(false, false);
            }
            if (t.text == "new") {
                Pos p = t.pos;
                take();
                auto n = mk(NK::Unary, p);
                n->text = "new";
                n->a = parsePostfix();
                return n;
            }
            break;
        }
        default:
            break;
    }
    // identifiers, including keywords used as macro names
    if (t.kind == Tok::Identifier || t.kind == Tok::Keyword) {
        auto n = mk(NK::Ident, t.pos);
        n->text = t.text;
        take();
        return n;
    }
    errorHere("SYN015", "无法解析的表达式，遇到 `" + (t.kind == Tok::End ? std::string("文件结束") : t.text) + "`");
    if (!atEnd()) take();
    return mk(NK::Ident, t.pos);
}

// -------------------------------------------------------------- macros -----

NodePtr Parser::parseTemplateEach() {
    Pos p = cur().pos;
    take();  // '@'
    take();  // 'each'
    auto n = mk(NK::Each, p);
    expectPunct("(", "@each");
    expectPunct("#", "@each");
    if (checkName()) n->text = takeName();
    else errorHere("SYN016", "@each 缺少循环变量");
    expectKeyword("of", "@each");
    expectPunct("#", "@each");
    if (checkName()) n->names.push_back(takeName());
    else errorHere("SYN017", "@each 缺少插槽列表名");
    expectPunct(")", "@each");
    if (checkPunct("{")) {
        auto b = mk(NK::Block, cur().pos);
        expectPunct("{", "@each");
        parseMacroTemplateBody(b);
        expectPunct("}", "@each");
        n->list = b->list;
    } else {
        n->list.push_back(parseStatement());
    }
    return n;
}

NodePtr Parser::parseTemplateWhen() {
    Pos p = cur().pos;
    take();  // '@'
    take();  // 'when'
    auto n = mk(NK::When, p);
    expectPunct("(", "@when");
    if (!checkPunct(")")) {
        n->a = parseExpression();
    } else {
        auto t = mk(NK::Bool, p);
        t->flag = true;
        n->a = t;
    }
    expectPunct(")", "@when");
    if (checkPunct("{")) {
        take();
        // Named arms (#y:/#n: or y:/n:) or a single unnamed body.
        bool named = false;
        if (cur().kind == Tok::Hash && (peek().isIdent("y") || peek().isIdent("n"))) named = true;
        if ((cur().isIdent("y") || cur().isIdent("n")) && peek().isPunct(":")) named = true;
        if (named) {
            while (!atEnd() && !checkPunct("}")) {
                if (cur().isPunct(",") || cur().isPunct(";")) {
                    take();
                    continue;
                }
                auto arm = mk(NK::WhenArm, cur().pos);
                if (cur().kind == Tok::Hash) take();
                if (check(Tok::Identifier)) arm->text = take().text;
                else errorHere("SYN018", "@when 分支缺少名字");
                expectPunct(":", "@when 分支");
                parseWhenArmBody(arm);
                n->list.push_back(arm);
            }
            expectPunct("}", "@when");
        } else {
            auto arm = mk(NK::WhenArm, cur().pos);
            parseMacroTemplateBody(arm);
            expectPunct("}", "@when");
            n->list.push_back(arm);
        }
    } else {
        auto arm = mk(NK::WhenArm, p);
        arm->list.push_back(parseStatement());
        n->list.push_back(arm);
    }
    return n;
}

bool Parser::isWhenArmStart() const {
    if (checkPunct("}")) return true;
    if (cur().kind == Tok::Hash && (peek().isIdent("y") || peek().isIdent("n")) &&
        peek(2).isPunct(":"))
        return true;
    if ((cur().isIdent("y") || cur().isIdent("n")) && peek().isPunct(":")) return true;
    return false;
}

// `#name:` at a macro call site opens a named slot argument.
bool Parser::isNamedSlotStart() const {
    return cur().kind == Tok::Hash && checkNameAt(1) && peek(2).isPunct(":");
}

// A brace block belongs to a macro call only when it starts with `#name:`.
// Otherwise it is the body of the enclosing `for` / `if` statement.
bool Parser::isNamedSlotBlock() const {
    return checkPunct("{") && peek().kind == Tok::Hash && checkNameAt(2) &&
           peek(3).isPunct(":");
}

// At a statement position `{` opens a block, yet `{ name: … }`, `{}` and
// `{ m() { … } }` are object literals. Both start with the same token, so decide by
// looking past the brace. This is not cosmetic: the two spellings produce different
// JavaScript, and a named slot whose value is `{ player: "phaseBegin" }` has to reach the
// macro as an object literal rather than as a block containing `player` and a stray colon.
bool Parser::looksLikeObjectLit(bool allowEmpty) const {
    if (!checkPunct("{")) return false;
    // `{}` is an empty object literal in a value position, but at a statement position it
    // stays a block: `@macro { }` is an invocation with every slot omitted, and treating
    // that brace as a literal would emit a stray `({});` statement.
    if (peek().isPunct("}")) return allowEmpty;
    if (peek().kind == Tok::String && peek(2).isPunct(":")) return true;
    std::size_t k = 1;
    const bool asyncModifier = (peek().kind == Tok::Identifier || peek().kind == Tok::Keyword) &&
                               peek().text == "async" &&
                               (peek(2).kind == Tok::Identifier || peek(2).kind == Tok::Keyword);
    if (asyncModifier) k = 2;  // `{ async name(…`
    const Token& name = peek(k);
    if (name.kind != Tok::Identifier && name.kind != Tok::Keyword) return false;
    if (peek(k + 1).isPunct(":")) return true;  // { name: … }
    if (!peek(k + 1).isPunct("(")) return false;
    // `{ name(params) { body } }` — a method definition. The body is what distinguishes it
    // from a plain call statement such as `{ f() }`, so scan to the matching parenthesis.
    // The scan index is relative to `i_`, exactly like `peek`.
    std::size_t j = k + 2;
    int depth = 1;
    while (i_ + j < t_.size() && depth > 0) {
        const Token& tk = t_[i_ + j];
        if (tk.kind == Tok::End) return false;
        if (tk.isPunct("(")) depth += 1;
        else if (tk.isPunct(")")) depth -= 1;
        ++j;
    }
    return depth == 0 && i_ + j < t_.size() && t_[i_ + j].isPunct("{");
}

void Parser::parseWhenArmBody(const NodePtr& arm) {
    while (!atEnd() && !isWhenArmStart()) {
        if (cur().isPunct(";") || cur().isPunct(",")) {
            take();
            continue;
        }
        std::size_t before = i_;
        NodePtr st = parseStatement();
        if (st) arm->list.push_back(st);
        if (i_ == before) {
            errorHere("SYN002", "@when 分支体内无法解析的内容，已跳过 `" + cur().text + "`");
            take();
        }
    }
}

NodePtr Parser::parseMacroApply(const Token& atToken, bool prefix) {
    Pos p = atToken.pos;
    take();  // '@'
    auto n = mk(NK::MacroApply, p);
    if (check(Tok::Identifier) || check(Tok::Keyword)) {
        n->text = take().text;
    } else {
        errorHere("SYN019", "`@` 后面需要宏名");
        return n;
    }

    // '@range' has its own argument shape.
    if (n->text == "range" && prefix) {
        n->list.push_back(parseRangeOrExpr(true));
        return n;
    }

    if (checkPunct("(")) {
        take();
        while (!atEnd() && !checkPunct(")")) {
            n->list.push_back(parseRangeOrExpr(n->text == "range"));
            if (!acceptPunct(",")) break;
        }
        expectPunct(")", "宏调用");
        return n;
    }

    if (isNamedSlotBlock()) {
        // Named slot block: `{ #red: ...  #black: ... }`.
        take();
        while (!atEnd() && !checkPunct("}")) {
            if (cur().isPunct(",") || cur().isPunct(";")) {
                take();
                continue;
            }
            if (cur().kind != Tok::Hash) {
                errorHere("SYN020", "命名插槽需要写成 `#名字:` 的形式");
                // Skip the offending token to guarantee progress.
                take();
                continue;
            }
            auto arg = mk(NK::Prop, cur().pos);
            take();  // '#'
            if (checkName()) arg->text = takeName();
            else errorHere("SYN020", "命名插槽缺少名字");
            expectPunct(":", "命名插槽");
            auto b = mk(NK::Block, cur().pos);
            if (looksLikeObjectLit()) {
                // The slot value is an object literal, not a block of statements:
                // `#trigger: { player: "phaseBegin" }`. It is wrapped in an ExprStmt so
                // that the same one-expression unwrapping applies as for any other
                // expression used as a slot value.
                auto st = mk(NK::ExprStmt, cur().pos);
                st->a = parseObjectLit();
                b->list.push_back(st);
            } else {
                while (!atEnd() && !checkPunct("}") && !isNamedSlotStart()) {
                    if (cur().isPunct(",") || cur().isPunct(";")) {
                        take();
                        continue;
                    }
                    std::size_t before = i_;
                    NodePtr st = parseStatement();
                    if (st) b->list.push_back(st);
                    if (i_ == before) {
                        errorHere("SYN002", "命名插槽内无法解析的内容，已跳过 `" + cur().text + "`");
                        take();
                    }
                }
            }
            arg->a = b;
            n->list.push_back(arg);
        }
        expectPunct("}", "命名插槽");
        return n;
    }

    // Postfix form: `x @op arg arg ...` collects unparenthesised slot arguments.
    // Each such argument is a single call fragment (`recover(2)`), never a
    // member chain, so `@share _p recover(2) draw(2)` yields three arguments.
    if (!prefix) {
        while (true) {
            const Token& c = cur();
            if (c.newlineBefore) break;  // the next line starts a new statement
            bool startsExpr = c.kind == Tok::Identifier || c.kind == Tok::Hash ||
                              c.kind == Tok::Number || c.kind == Tok::String ||
                              c.kind == Tok::TemplateString || c.kind == Tok::Under ||
                              c.kind == Tok::TsBlock ||
                              (c.kind == Tok::Punct && (c.text == "(" || c.text == "[" ||
                                                        c.text == "{" || c.text == "...")) ||
                              (c.kind == Tok::Keyword &&
                               (c.text == "true" || c.text == "false" || c.text == "void" ||
                                c.text == "fn" || c.text == "this" || c.text == "new"));
            if (!startsExpr) break;
            // A block would be a statement body, not an argument.
            if (cur().isPunct("{")) break;
            if (n->text == "range") {
                n->list.push_back(parseRangeOrExpr(true));
            } else {
                n->list.push_back(parseSlotFragment());
            }
        }
    }
    return n;
}

// One unparenthesised macro slot argument: a primary with an optional call and
// optional index/safe-call tail. Use `@macro(a + b)` for richer expressions.
NodePtr Parser::parseSlotFragment() {
    const Token& t = cur();
    if (t.isPunct("-") || t.isPunct("+") || t.isPunct("!") || t.isKeyword("not")) {
        Pos p = t.pos;
        std::string op = t.isKeyword("not") ? "!" : take().text;
        if (t.isKeyword("not")) take();
        auto n = mk(NK::Unary, p);
        n->text = op;
        n->a = parseSlotFragment();
        return n;
    }
    if (t.isPunct("...")) {
        Pos p = t.pos;
        take();
        auto n = mk(NK::Spread, p);
        n->a = parseSlotFragment();
        return n;
    }
    NodePtr e = parsePrimary();
    for (;;) {
        if (checkPunct("(")) {
            e = parseCallArgs(e, false);
            continue;
        }
        if (checkPunct("?") && peek().isPunct("(")) {
            take();
            e = parseCallArgs(e, true);
            continue;
        }
        if (checkPunct("[")) {
            Pos p = cur().pos;
            take();
            auto n = mk(NK::Index, p);
            n->a = e;
            n->b = parseTernary();
            expectPunct("]", "下标");
            e = n;
            continue;
        }
        break;
    }
    // Mark a bare `name(...)` / `name` fragment so the checker knows the name
    // is a member of the receiver, not a free identifier.
    if (e && e->kind == NK::Call && e->a && e->a->kind == NK::Ident) e->flag2 = true;
    if (e && e->kind == NK::Ident && !e->flag) e->flag2 = true;
    return e;
}

// ------------------------------------------------------------ type syntax ---

bool Parser::isTypeStart(const Token& t) const {
    if (t.kind == Tok::Identifier) return true;
    if (t.kind == Tok::String) return true;
    if (t.kind == Tok::Number) return true;
    if (t.kind == Tok::Punct && (t.text == "(" || t.text == "[")) return true;
    if (t.kind == Tok::Keyword) {
        static const std::set<std::string> kw = {"void",  "null",  "undefined", "number",
                                                 "string", "boolean", "true",   "false"};
        return kw.count(t.text) > 0;
    }
    return false;
}

std::string Parser::parseParamAnnotation() {
    // A parameter cannot be a function type: shya has no function values, so there is
    // no way to pass one, and accepting the annotation would advertise a capability the
    // language does not have.
    Pos p = cur().pos;
    std::string ann = parseTypeAnnotation();
    if (ann == "fn") {
        bag_.error(p, "SYN032",
                   "参数类型不能是函数类型 `fn`：shya 不支持把函数作为参数传递（回调）");
    }
    return ann;
}

std::string Parser::parseTypeAnnotation() {
    // Lightweight type grammar; the normalised text is re-parsed by the checker.
    std::function<std::string()> parseUnion = [&]() -> std::string {
        std::string left = [&]() -> std::string {
            std::string base;
            const Token& t = cur();
            if (t.isPunct("(")) {
                take();
                base = parseTypeAnnotation();
                expectPunct(")", "类型");
            } else if (t.kind == Tok::String) {
                base = "\"" + take().value + "\"";
            } else if (t.kind == Tok::Number) {
                base = take().text;
            } else if (t.kind == Tok::Identifier || t.kind == Tok::Keyword) {
                std::string name = take().text;
                if (name == "bool") name = "boolean";
                if (name == "null" || name == "undefined") name = "void";
                base = name;
                if (checkPunct("<")) {
                    take();
                    base += "<";
                    bool first = true;
                    while (!atEnd() && !checkPunct(">")) {
                        if (!first) base += ",";
                        base += parseTypeAnnotation();
                        first = false;
                        if (!acceptPunct(",")) break;
                    }
                    expectPunct(">", "泛型参数");
                    base += ">";
                }
            } else {
                errorHere("SYN021", "无法解析的类型标注");
                if (!atEnd()) take();
                base = "unknown";
            }
            // postfix [] / ?
            for (;;) {
                if (checkPunct("[") && peek().isPunct("]")) {
                    take();
                    take();
                    if (base.rfind("array<", 0) == 0) {
                        base = "array<array<" + base.substr(6);
                        // '(array<X>)[]' -> array<array<X>>
                        if (!base.empty() && base.back() == '>') base.pop_back();
                        base += ">";
                    } else {
                        base = "array<" + base + ">";
                    }
                    continue;
                }
                if (checkPunct("?")) {
                    take();
                    base += "?";
                    continue;
                }
                break;
            }
            return base;
        }();
        while (checkPunct("|")) {
            take();
            std::string right = [&]() -> std::string {
                std::string b;
                if (cur().kind == Tok::Identifier || cur().kind == Tok::Keyword ||
                    cur().kind == Tok::String || cur().kind == Tok::Number) {
                    if (cur().kind == Tok::String) b = "\"" + take().value + "\"";
                    else b = take().text;
                } else {
                    errorHere("SYN021", "无法解析的联合类型成员");
                    if (!atEnd()) take();
                    b = "unknown";
                }
                return b;
            }();
            left += "|" + right;
        }
        return left;
    };
    return parseUnion();
}

}  // namespace shya
