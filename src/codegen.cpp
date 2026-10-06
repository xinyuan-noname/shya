// shya - core AST -> ES2026 JavaScript.
#include "shya.h"

#include <cstdlib>

namespace shya {

namespace {
const char kMarker = '\x01';

std::string trimTrailingNewline(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// True for nodes that can only appear as statements.
bool isStatementNode(const NodePtr& n) {
    if (!n) return false;
    switch (n->kind) {
        case NK::Assign:
        case NK::IncDec:
        case NK::Decl:
        case NK::Define:
        case NK::If:
        case NK::Case:
        case NK::ForWhile:
        case NK::ForOf:
        case NK::ForEach:
        case NK::ForRange:
        case NK::FnDecl:
        case NK::Return:
        case NK::Throw:
        case NK::Try:
        case NK::Break:
        case NK::Continue:
        case NK::Import:
        case NK::Export:
        case NK::Block:
        case NK::Program:
        case NK::ExprStmt:
        case NK::MacroDecl:
            return true;
        default:
            return false;
    }
}
}  // namespace

Codegen::Codegen(DiagBag& bag, CodegenOptions opt) : bag_(bag), opt_(opt) {}

// ------------------------------------------------------------------ utils ---

std::string Codegen::pad() const {
    return std::string(static_cast<std::size_t>(depth_) * static_cast<std::size_t>(opt_.indentWidth),
                       ' ');
}

void Codegen::line(const std::string& s) {
    out_ += pad();
    out_ += s;
    out_ += '\n';
}

std::string Codegen::escapeJsString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    out += "\"";
    return out;
}

std::string Codegen::formatNumber(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    if (d == 0) return std::signbit(d) ? "-0" : "0";
    char buf[64];
    if (d == std::floor(d) && std::fabs(d) < 1e21) {
        std::snprintf(buf, sizeof(buf), "%.0f", d);
        return buf;
    }
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    return buf;
}

int Codegen::precedence(const std::string& op) {
    if (op == "||") return 1;
    if (op == "&&") return 2;
    if (op == "==" || op == "!=" || op == "===" || op == "!==" || op == "is" ||
        op == "is not" || op == "instanceof" || op == "not instanceof")
        return 3;
    if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
    if (op == "+" || op == "-") return 5;
    if (op == "*" || op == "/" || op == "%" || op == "~/" || op == "+/" || op == "-/" ||
        op == "\\/")
        return 6;
    if (op == "^") return 8;
    return 11;
}

bool Codegen::hasSideEffects(const NodePtr& n) {
    if (!n) return false;
    switch (n->kind) {
        case NK::Num:
        case NK::Str:
        case NK::Bool:
        case NK::Void:
        case NK::Ident:
        case NK::MathConst:
        case NK::Empty:
            return false;
        case NK::Member:
            return true;  // shya member access always performs a call
        case NK::Index:
            return hasSideEffects(n->a) || hasSideEffects(n->b);
        case NK::Unary:
            return n->text != "!" && n->text != "-" && n->text != "+" && n->text != "typeof"
                       ? true
                       : hasSideEffects(n->a);
        case NK::Binary:
            return hasSideEffects(n->a) || hasSideEffects(n->b);
        default:
            return true;
    }
}

// ------------------------------------------------------------------ scopes ---

bool Codegen::isDeclared(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        if (it->count(name)) return true;
    }
    return false;
}

void Codegen::declareName(const std::string& name) {
    if (scopes_.empty()) scopes_.push_back({});
    scopes_.back().insert(name);
}

int Codegen::assignCountFor(const std::string& name) const {
    for (auto it = assignCounts_.rbegin(); it != assignCounts_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return f->second;
    }
    return 0;
}

void Codegen::scanAssignments(const NodePtr& n, std::unordered_map<std::string, int>& out) {
    if (!n) return;
    switch (n->kind) {
        case NK::Assign:
            for (const auto& t : n->targets) {
                if (t->kind == NK::Ident && !t->flag) out[t->text]++;
            }
            for (const auto& v : n->values) scanAssignments(v, out);
            return;
        case NK::IncDec:
            if (n->a && n->a->kind == NK::Ident) out[n->a->text]++;
            return;
        case NK::FnDecl:
            return;  // its own scope; handled when generating the body
        case NK::Decl: {
            // A declaration is the initial binding, so a later assignment makes
            // the name reassigned (and therefore `let`).
            if (!n->list.empty()) {
                for (const auto& d : n->list) scanAssignments(d, out);
            } else if (!n->names.empty()) {
                out[n->names[0]]++;
            }
            if (n->a) scanAssignments(n->a, out);
            return;
        }
        case NK::Block:
        case NK::Program: {
            for (const auto& s : n->list) scanAssignments(s, out);
            return;
        }
        case NK::If:
            scanAssignments(n->a, out);
            scanAssignments(n->b, out);
            scanAssignments(n->c, out);
            return;
        case NK::ForWhile:
        case NK::ForOf:
            scanAssignments(n->a, out);
            scanAssignments(n->b, out);
            return;
        case NK::ForRange:
            scanAssignments(n->a, out);
            scanAssignments(n->b, out);
            scanAssignments(n->c, out);
            scanAssignments(n->d, out);
            for (const auto& s : n->list) scanAssignments(s, out);
            return;
        case NK::Case:
            if (n->a) scanAssignments(n->a, out);
            for (const auto& arm : n->list)
                for (const auto& s : arm->list) scanAssignments(s, out);
            return;
        case NK::Try:
            scanAssignments(n->a, out);
            scanAssignments(n->b, out);
            scanAssignments(n->c, out);
            return;
        case NK::Export:
            scanAssignments(n->a, out);
            return;
        default:
            break;
    }
    for (const auto& k : {n->a, n->b, n->c, n->d}) {
        if (k) scanAssignments(k, out);
    }
    for (const auto& k : n->list) scanAssignments(k, out);
}

void Codegen::pushAssignCounts(const std::vector<NodePtr>& stmts) {
    std::unordered_map<std::string, int> counts;
    countAssignments(stmts, counts);
    assignCounts_.push_back(std::move(counts));
}

void Codegen::popAssignCounts() {
    if (!assignCounts_.empty()) assignCounts_.pop_back();
}

void Codegen::countAssignments(const std::vector<NodePtr>& stmts,
                               std::unordered_map<std::string, int>& out) {
    for (const auto& s : stmts) scanAssignments(s, out);
}

// -------------------------------------------------------------- expressions --

std::string Codegen::genArguments(const std::vector<NodePtr>& args) {
    std::string s;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) s += ", ";
        s += genExpr(args[i], 0);
    }
    return s;
}

std::string Codegen::genTsRaw(const NodePtr& n) {
    const std::string& src = n->raw;
    std::string out;
    for (std::size_t i = 0; i < src.size();) {
        if (src[i] == kMarker) {
            std::size_t j = i + 1;
            std::string digits;
            while (j < src.size() && src[j] != kMarker) digits.push_back(src[j++]);
            if (j < src.size()) ++j;  // closing marker
            i = j;
            bool ok = !digits.empty();
            std::size_t idx = 0;
            for (char c : digits) {
                if (c < '0' || c > '9') ok = false;
                else idx = idx * 10 + static_cast<std::size_t>(c - '0');
            }
            if (ok && idx < n->list.size()) {
                out += genExpr(n->list[idx], 0);
            } else {
                bag_.error(n->pos, "CGN001", "`@ts` 插槽占位符损坏（编译器错误）");
            }
            continue;
        }
        out.push_back(src[i]);
        ++i;
    }
    // Collapse the indentation of multi-line raw blocks so they line up.
    std::string trimmed = trimTrailingNewline(out);
    return trimmed;
}

std::string Codegen::genFragment(const NodePtr& n, bool optional) {
    if (!n || n->kind == NK::Empty) return "";
    if (n->kind == NK::Optionalize) return genFragment(n->a, true);
    if (n->kind == NK::Ident && n->flag && n->text == "_") return "";
    if (!optional && n->kind == NK::Binary && n->text == "concat") {
        // A nested concatenation keeps its own fragment semantics.
        return genExpr(n, 11);
    }
    if (n->kind == NK::Call) {
        const NodePtr& callee = n->a;
        if (callee && callee->kind == NK::Ident) {
            std::string s = optional ? "?." : ".";
            s += callee->text;
            if (n->flag) s += "?.";
            s += "(" + genArguments(n->list) + ")";
            return s;
        }
        if (callee && callee->kind == NK::Member) {
            std::string s = optional ? "?." : ".";
            s += callee->text;
            if (n->flag) s += "?.";
            s += "(" + genArguments(n->list) + ")";
            return s;
        }
        // A receiver-based call is a complete expression, not a chain link.
        NodePtr copy = n;
        if (optional) bag_.warning(n->pos, "CGN002",
                                   "可选调用作用在带接收者的表达式上，已忽略可选修饰");
        return genExpr(copy, 11);
    }
    if (n->kind == NK::Member) {
        if (n->b) return genFragment(n->b, optional);
        std::string s = optional ? "?." : ".";
        s += n->text;
        s += "(" + genArguments(n->list) + ")";
        return s;
    }
    if (n->kind == NK::Ident) {
        // A bare member name used as a chain link: `x @safe getHp`.
        return (optional ? "?." : ".") + n->text + "()";
    }
    return genExpr(n, 11);
}

Codegen::Rendered Codegen::genExprP(const NodePtr& n) {
    Rendered r;
    if (!n) {
        r.text = "";
        return r;
    }
    switch (n->kind) {
        case NK::Empty:
            r.text = "";
            return r;
        case NK::Num:
        case NK::MathConst:
            r.text = n->kind == NK::Num && !n->text.empty() && n->text[0] == '0' &&
                             (n->text.size() > 1 &&
                              (n->text[1] == 'x' || n->text[1] == 'X' || n->text[1] == 'b' ||
                               n->text[1] == 'B' || n->text[1] == 'o' || n->text[1] == 'O'))
                         ? n->text
                         : formatNumber(n->num);
            return r;
        case NK::Str:
            r.text = escapeJsString(n->text);
            return r;
        case NK::Tpl: {
            std::string s = "`";
            for (std::size_t i = 0; i < n->names.size(); ++i) {
                s += n->names[i];
                if (i < n->list.size()) s += "${" + genExpr(n->list[i], 0) + "}";
            }
            s += "`";
            r.text = s;
            return r;
        }
        case NK::Bool:
            r.text = n->flag ? "true" : "false";
            return r;
        case NK::Void:
            r.text = "undefined";
            return r;
        case NK::Ident:
            r.text = (n->flag && n->text == "_") ? "" : n->text;
            return r;
        case NK::ArrayLit: {
            std::string s = "[";
            for (std::size_t i = 0; i < n->list.size(); ++i) {
                if (i) s += ", ";
                s += genExpr(n->list[i], 0);
            }
            s += "]";
            r.text = s;
            return r;
        }
        case NK::ObjectLit: {
            std::string s = "{ ";
            for (std::size_t i = 0; i < n->list.size(); ++i) {
                if (i) s += ", ";
                s += genExpr(n->list[i], 0);
            }
            s += " }";
            r.text = s;
            return r;
        }
        case NK::Prop: {
            std::string key = n->text;
            bool simpleKey = !key.empty();
            for (char c : key) {
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$'))
                    simpleKey = false;
            }
            std::string k = simpleKey ? key : escapeJsString(key);
            if (n->a && n->a->kind == NK::FnDecl) {
                const NodePtr& fn = n->a;
                std::string s = k + "(" + genParams(fn) + ") " + genBlockOf(fn->list);
                r.text = s;
                return r;
            }
            r.text = k + ": " + (n->a ? genExpr(n->a, 0) : std::string("undefined"));
            return r;
        }
        case NK::Unary: {
            if (n->text == "new") {
                r.text = "new " + genExpr(n->a, 11);
                r.prec = 9;
                return r;
            }
            if (n->text == "typeof") {
                r.text = "typeof " + genExpr(n->a, 9);
                r.prec = 9;
                return r;
            }
            r.text = n->text + genExpr(n->a, 9);
            r.prec = 9;
            return r;
        }
        case NK::Binary: {
            if (n->text == "concat") {
                // Chain concatenation: head expression + chain-link fragments.
                std::vector<NodePtr> parts;
                std::function<void(const NodePtr&)> walk = [&](const NodePtr& x) {
                    if (x && x->kind == NK::Binary && x->text == "concat") {
                        walk(x->a);
                        walk(x->b);
                    } else {
                        parts.push_back(x);
                    }
                };
                walk(n);
                std::string s;
                for (std::size_t i = 0; i < parts.size(); ++i) {
                    if (i == 0) s += genExpr(parts[i], 0);
                    else s += genFragment(parts[i], false);
                }
                r.text = s;
                return r;
            }
            const std::string& op = n->text;
            if (op == "~/") {
                r.text = "Math.trunc(" + genExpr(n->a, 6) + " / " + genExpr(n->b, 7) + ")";
                return r;
            }
            if (op == "+/") {
                r.text = "Math.ceil(" + genExpr(n->a, 6) + " / " + genExpr(n->b, 7) + ")";
                return r;
            }
            if (op == "-/") {
                r.text = "Math.floor(" + genExpr(n->a, 6) + " / " + genExpr(n->b, 7) + ")";
                return r;
            }
            if (op == "\\/") {
                r.text = "Math.round(" + genExpr(n->a, 6) + " / " + genExpr(n->b, 7) + ")";
                return r;
            }
            if (op == "^") {
                r.text = genExpr(n->a, 9) + " ** " + genExpr(n->b, 8);
                r.prec = 8;
                return r;
            }
            int p = precedence(op);
            r.text = genExpr(n->a, p) + " " + op + " " + genExpr(n->b, p + 1);
            r.prec = p;
            return r;
        }
        case NK::Compare: {
            r.text = genCompare(n);
            r.prec = n->list.size() > 1 ? 2 : 3;
            return r;
        }
        case NK::Ternary: {
            r.text = genExpr(n->a, 1) + " ? " + genExpr(n->b, 0) + " : " + genExpr(n->c, 0);
            r.prec = 0;
            return r;
        }
        case NK::Call: {
            r.text = genExpr(n->a, 11) + (n->flag ? "?." : "") + "(" + genArguments(n->list) + ")";
            return r;
        }
        case NK::Member: {
            if (n->b) {
                r.text = genExpr(n->a, 11) + genFragment(n->b, n->flag2);
                return r;
            }
            if (fieldAccesses_ && fieldAccesses_->count(n.get())) {
                // A `declare`d field is a plain property: `p.name`, not `p.name()`.
                r.text = genExpr(n->a, 11) + "." + n->text;
                return r;
            }
            std::string s = genExpr(n->a, 11) + "." + n->text;
            s += "(" + genArguments(n->list) + ")";
            r.text = s;
            return r;
        }
        case NK::Index: {
            r.text = genExpr(n->a, 11) + "[" + genExpr(n->b, 0) + "]";
            return r;
        }
        case NK::Spread: {
            r.text = "..." + genExpr(n->a, 6);
            r.prec = 9;
            return r;
        }
        case NK::Await: {
            r.text = "await " + genExpr(n->a, 9);
            r.prec = 9;
            return r;
        }
        case NK::IncDec: {
            std::string target = genExpr(n->a, 11);
            r.text = n->flag ? n->text + target : target + n->text;
            return r;
        }
        case NK::Optionalize: {
            r.text = genExpr(n->a, 0);
            return r;
        }
        case NK::TsRaw: {
            r.text = genTsRaw(n);
            return r;
        }
        case NK::FnDecl: {
            std::string s;
            if (n->flag) s += "async ";
            s += "function";
            if (!n->text.empty()) s += " " + n->text;
            s += "(" + genParams(n) + ") " + genBlockOf(n->list);
            r.text = s;
            return r;
        }
        case NK::Block:
        case NK::Program: {
            r.text = genBlockOf(n->list);
            return r;
        }
        case NK::RangeExpr:
            bag_.error(n->pos, "CGN003", "范围表达式只能出现在 `@range` 中");
            r.text = "0";
            return r;
        default:
            break;
    }
    bag_.error(n->pos, "CGN004",
               std::string("代码生成阶段遇到无法处理的节点 ") + nodeKindName(n->kind));
    r.text = "undefined";
    return r;
}

std::string Codegen::genExpr(const NodePtr& n, int parentPrec) {
    Rendered r = genExprP(n);
    if (r.text.empty()) return r.text;
    if (r.prec < parentPrec) return "(" + r.text + ")";
    return r.text;
}

std::string Codegen::genCompare(const NodePtr& n) {
    // Single comparison.
    auto one = [&](const std::string& op, const NodePtr& l, const NodePtr& r) -> std::string {
        if (op == "==") return genExpr(l, 4) + " === " + genExpr(r, 5);
        if (op == "!=") return genExpr(l, 4) + " !== " + genExpr(r, 5);
        if (op == "===") return genExpr(l, 4) + " === " + genExpr(r, 5);
        if (op == "!==") return genExpr(l, 4) + " !== " + genExpr(r, 5);
        if (op == "instanceof") return genExpr(l, 4) + " instanceof " + genExpr(r, 5);
        if (op == "not instanceof")
            return "!(" + genExpr(l, 4) + " instanceof " + genExpr(r, 5) + ")";
        if (op == "is" || op == "is not") {
            std::string typeName = r && r->kind == NK::Ident ? r->text : std::string();
            auto test = [&](const std::string& subject) -> std::string {
                if (typeName == "array") return "Array.isArray(" + subject + ")";
                if (typeName == "string") return "typeof " + subject + " === \"string\"";
                if (typeName == "number") return "typeof " + subject + " === \"number\"";
                if (typeName == "boolean") return "typeof " + subject + " === \"boolean\"";
                if (typeName == "fn" || typeName == "function")
                    return "typeof " + subject + " === \"function\"";
                if (typeName == "map") return subject + " instanceof Map";
                if (typeName == "set") return subject + " instanceof Set";
                if (typeName == "void" || typeName == "null" || typeName == "undefined")
                    return subject + " === undefined";
                if (typeName == "object")
                    return "typeof " + subject + " === \"object\" && " + subject + " !== null";
                if (typeName.empty()) {
                    bag_.error(n->pos, "CGN005", "`is` 的右侧必须是类型名");
                    return "false";
                }
                return subject + " instanceof " + typeName;
            };
            bool needTemp = hasSideEffects(l);
            std::string subject = needTemp ? "__shya_is" : genExpr(l, 11);
            std::string expr = test(subject);
            if (op == "is not") expr = "!(" + expr + ")";
            if (needTemp) {
                return "((__shya_is) => " + expr + ")(" + genExpr(l, 0) + ")";
            }
            return expr;
        }
        return genExpr(l, 5) + " " + op + " " + genExpr(r, 6);
    };

    if (n->list.size() <= 1) {
        return one(n->text, n->a, n->list.empty() ? nullptr : n->list[0]);
    }
    // Chained comparison: a < b < c  ->  (a < b) && (b < c).
    // Interior operands appear twice, so those with side effects are hoisted
    // into arrow-function parameters and evaluated exactly once.
    std::vector<NodePtr> operands;
    operands.push_back(n->a);
    for (const auto& r : n->list) operands.push_back(r);

    std::vector<bool> hoist(operands.size(), false);
    bool anyHoist = false;
    for (std::size_t i = 1; i + 1 < operands.size(); ++i) {
        hoist[i] = hasSideEffects(operands[i]);
        if (hoist[i]) anyHoist = true;
    }

    auto subject = [&](std::size_t i) -> std::string {
        if (hoist[i]) return "__shya_cmp" + std::to_string(i);
        return genExpr(operands[i], 5);
    };

    std::string body;
    for (std::size_t i = 0; i + 1 < operands.size(); ++i) {
        std::string op = i < n->names.size() ? n->names[i] : n->text;
        std::string ls = subject(i), rs = subject(i + 1);
        if (i) body += " && ";
        if (op == "==" || op == "===") body += ls + " === " + rs;
        else if (op == "!=" || op == "!==") body += ls + " !== " + rs;
        else if (op == "instanceof") body += ls + " instanceof " + rs;
        else if (op == "not instanceof") body += "!(" + ls + " instanceof " + rs + ")";
        else body += ls + " " + op + " " + rs;
    }

    if (!anyHoist) return body;
    std::string params, args;
    for (std::size_t i = 1; i + 1 < operands.size(); ++i) {
        if (!hoist[i]) continue;
        if (!params.empty()) {
            params += ", ";
            args += ", ";
        }
        params += "__shya_cmp" + std::to_string(i);
        args += genExpr(operands[i], 0);
    }
    return "((" + params + ") => " + body + ")(" + args + ")";
}

// ------------------------------------------------------- params and blocks ---

std::string Codegen::genParams(const NodePtr& n) {
    std::string s;
    for (std::size_t i = 0; i < n->names.size(); ++i) {
        if (i) s += ", ";
        std::string ann = i < n->typeAnns.size() ? n->typeAnns[i] : std::string();
        bool rest = ann.rfind("...", 0) == 0;
        if (rest) s += "...";
        s += n->names[i];
        if (i < n->defaults.size() && n->defaults[i] && !rest) {
            s += " = " + genExpr(n->defaults[i], 0);
        }
    }
    return s;
}

std::string Codegen::genBlockOf(const std::vector<NodePtr>& stmts) {
    std::string saved = out_;
    int savedDepth = depth_;
    out_.clear();
    depth_ = savedDepth + 1;
    scopes_.push_back({});
    pushAssignCounts(stmts);
    genStatements(stmts);
    popAssignCounts();
    scopes_.pop_back();
    depth_ = savedDepth;
    std::string body = out_;
    out_ = saved;
    return "{\n" + body + pad() + "}";
}

void Codegen::genBranchBody(const NodePtr& body) {
    if (!body) return;
    scopes_.push_back({});
    if (body->kind == NK::Block) {
        pushAssignCounts(body->list);
        genStatements(body->list);
        popAssignCounts();
    } else {
        genStatement(body);
    }
    scopes_.pop_back();
}

void Codegen::genIfChain(const NodePtr& n) {
    std::vector<std::pair<NodePtr, NodePtr>> arms;
    NodePtr elseBody = nullptr;
    NodePtr cur = n;
    while (cur && cur->kind == NK::If) {
        arms.push_back({cur->a, cur->b});
        NodePtr next = cur->c;
        if (next && next->kind == NK::If) {
            cur = next;
            continue;
        }
        elseBody = next;
        break;
    }
    for (std::size_t i = 0; i < arms.size(); ++i) {
        std::string head = (i == 0) ? "if (" : "} else if (";
        line(head + genExpr(arms[i].first, 0) + ") {");
        depth_++;
        genBranchBody(arms[i].second);
        depth_--;
    }
    if (elseBody) {
        line("} else {");
        depth_++;
        genBranchBody(elseBody);
        depth_--;
    }
    line("}");
}

// --------------------------------------------------------------- statements --

std::string Codegen::captureStatement(const NodePtr& n) {
    std::string saved = out_;
    out_.clear();
    genStatement(n);
    std::string r = out_;
    out_ = saved;
    return trimTrailingNewline(r);
}

void Codegen::genStatements(const std::vector<NodePtr>& stmts) {
    for (const auto& s : stmts)
        if (s) genStatement(s);
}

void Codegen::genStatement(const NodePtr& n) {
    if (!n) return;
    switch (n->kind) {
        case NK::Empty:
            return;
        case NK::Program:
        case NK::Block: {
            if (n->list.empty()) return;
            line("{");
            depth_++;
            scopes_.push_back({});
            pushAssignCounts(n->list);
            genStatements(n->list);
            popAssignCounts();
            scopes_.pop_back();
            depth_--;
            line("}");
            return;
        }
        case NK::ExprStmt: {
            if (!n->a || n->a->kind == NK::Empty) return;
            if (isStatementNode(n->a)) {
                // A macro slot substituted a statement where an expression was
                // written; emit it as a statement.
                genStatement(n->a);
                return;
            }
            std::string e = genExpr(n->a, 0);
            if (e.empty()) return;
            line(e + ";");
            return;
        }
        case NK::Decl: {
            if (!n->list.empty()) {
                for (const auto& d : n->list) genStatement(d);
                return;
            }
            if (n->names.empty()) return;
            const std::string& name = n->names[0];
            std::string init = n->a ? genExpr(n->a, 0) : std::string();
            if (init.empty()) init = "undefined";
            std::string kw;
            int cnt = assignCountFor(name);
            if (n->text == "let") kw = "let";
            else if (n->text == "const") {
                kw = "const";
                if (cnt > 1) {
                    bag_.error(n->pos, "CGN006",
                               "`const " + name + "` 在其他地方被重新赋值（共 " +
                                   std::to_string(cnt) + " 次赋值）");
                }
            } else {
                kw = (cnt > 1) ? "let" : "const";
            }
            declareName(name);
            std::string prefix = pendingExport_ ? "export " : "";
            pendingExport_ = false;
            line(prefix + kw + " " + name + " = " + init + ";");
            return;
        }
        case NK::Assign: {
            if (!n->text.empty() && n->targets.size() == 1 && n->values.size() == 1) {
                // Compound assignment: `x += 1` -> `x += 1;`, `x ^= 2` -> `x **= 2;`
                std::string jsop = (n->text == "^") ? "**" : n->text;
                line(genExpr(n->targets[0], 11) + " " + jsop + "= " +
                     genExpr(n->values[0], 0) + ";");
                return;
            }
            // A single target that is a brand-new identifier becomes a declaration.
            if (n->targets.size() == n->values.size()) {
                bool allNew = true;
                for (const auto& t : n->targets) {
                    if (t->kind != NK::Ident || t->flag || isDeclared(t->text)) {
                        allNew = false;
                        break;
                    }
                }
                if (allNew && !n->targets.empty()) {
                    for (std::size_t i = 0; i < n->targets.size(); ++i) {
                        const std::string& name = n->targets[i]->text;
                        int cnt = assignCountFor(name);
                        std::string kw = (cnt > 1) ? "let" : "const";
                        declareName(name);
                        line(kw + " " + name + " = " + genExpr(n->values[i], 0) + ";");
                    }
                    return;
                }
            }
            if (n->targets.size() > 1 && n->values.size() == n->targets.size()) {
                std::string lhs = "[";
                for (std::size_t i = 0; i < n->targets.size(); ++i) {
                    if (i) lhs += ", ";
                    lhs += genExpr(n->targets[i], 0);
                }
                lhs += "]";
                std::string rhs = "[";
                for (std::size_t i = 0; i < n->values.size(); ++i) {
                    if (i) rhs += ", ";
                    rhs += genExpr(n->values[i], 0);
                }
                rhs += "]";
                line(lhs + " = " + rhs + ";");
                return;
            }
            if (n->targets.empty()) return;
            std::string lhs = genExpr(n->targets[0], 11);
            std::string rhs = n->values.empty() ? std::string("undefined")
                                                : genExpr(n->values[0], 0);
            line(lhs + " = " + rhs + ";");
            return;
        }
        case NK::IncDec: {
            line(genExpr(n->a, 11) + n->text + ";");
            return;
        }
        case NK::If: {
            genIfChain(n);
            return;
        }
        case NK::Case: {
            if (n->a) {
                line("switch (" + genExpr(n->a, 0) + ") {");
                depth_++;
                for (const auto& arm : n->list) {
                    if (arm->flag) {
                        line("default:");
                    } else {
                        for (const auto& p : arm->patterns) line("case " + genExpr(p, 0) + ":");
                    }
                    depth_++;
                    if (arm->flag2) {
                        line("// fallthrough");
                    } else {
                        scopes_.push_back({});
                        for (const auto& s : arm->list) genStatement(s);
                        scopes_.pop_back();
                        line("break;");
                    }
                    depth_--;
                }
                depth_--;
                line("}");
                return;
            }
            // Subject-less case: an if / else-if chain.
            bool first = true;
            bool hasDefault = false;
            for (const auto& arm : n->list) {
                if (arm->flag) {
                    hasDefault = true;
                    if (first) {
                        line("{");
                        depth_++;
                        scopes_.push_back({});
                    } else {
                        line("else {");
                        depth_++;
                        scopes_.push_back({});
                    }
                    for (const auto& s : arm->list) genStatement(s);
                    scopes_.pop_back();
                    depth_--;
                    line("}");
                    continue;
                }
                std::string cond;
                for (std::size_t i = 0; i < arm->patterns.size(); ++i) {
                    if (i) cond += " || ";
                    cond += genExpr(arm->patterns[i], 3);
                }
                if (first) {
                    line("if (" + cond + ") {");
                } else {
                    line("else if (" + cond + ") {");
                }
                depth_++;
                scopes_.push_back({});
                for (const auto& s : arm->list) genStatement(s);
                scopes_.pop_back();
                depth_--;
                line("}");
                first = false;
            }
            (void)hasDefault;
            return;
        }
        case NK::ForWhile: {
            line("while (" + genExpr(n->a, 0) + ") {");
            depth_++;
            scopes_.push_back({});
            if (n->b && n->b->kind == NK::Block) {
                pushAssignCounts(n->b->list);
                genStatements(n->b->list);
                popAssignCounts();
            } else {
                genStatement(n->b);
            }
            scopes_.pop_back();
            depth_--;
            line("}");
            return;
        }
        case NK::ForOf: {
            scopes_.push_back({});
            std::string head;
            if (n->names.size() >= 2) {
                head = "[" + n->names[0] + ", " + n->names[1] + "]";
            } else if (!n->names.empty()) {
                head = n->names[0];
            } else {
                head = "__shya_unused";
            }
            if (n->names.size() >= 2) {
                declareName(n->names[0]);
                declareName(n->names[1]);
            } else if (!n->names.empty()) {
                declareName(n->names[0]);
            }
            line("for (const " + head + " of " + genExpr(n->a, 0) + ") {");
            depth_++;
            if (n->b && n->b->kind == NK::Block) genStatements(n->b->list);
            else genStatement(n->b);
            depth_--;
            line("}");
            scopes_.pop_back();
            return;
        }
        case NK::ForRange: {
            std::string start = genExpr(n->a, 0);
            std::string end = genExpr(n->b, 0);
            double stepVal = 1;
            bool stepKnown = true;
            if (n->c && n->c->kind == NK::Num) stepVal = n->c->num;
            else if (n->c && n->c->kind == NK::Unary && n->c->text == "-" && n->c->a &&
                     n->c->a->kind == NK::Num)
                stepVal = -n->c->a->num;
            else if (n->c)
                stepKnown = false;
            std::string step = n->c ? genExpr(n->c, 0) : std::string("1");
            std::string compare = (!stepKnown || stepVal >= 0) ? " < " : " > ";
            std::string var = n->text.empty() ? "__shya_i" : n->text;
            if (n->d) {
                line("{");
                depth_++;
                line("const __shya_seq = Array.from(" + genExpr(n->d, 0) + ");");
                line("for (let __shya_i = " + start + "; __shya_i" + compare + end +
                     "; __shya_i += " + step + ") {");
                depth_++;
                scopes_.push_back({});
                declareName(var);
                line("const " + var + " = __shya_seq[__shya_i];");
                if (!n->list.empty()) {
                    if (n->list.size() == 1 && n->list[0]->kind == NK::Block) {
                        genStatements(n->list[0]->list);
                    } else {
                        genStatements(n->list);
                    }
                }
                scopes_.pop_back();
                depth_--;
                line("}");
                depth_--;
                line("}");
                return;
            }
            scopes_.push_back({});
            declareName(var);
            line("for (let " + var + " = " + start + "; " + var + compare + end + "; " + var +
                 " += " + step + ") {");
            depth_++;
            if (!n->list.empty()) {
                if (n->list.size() == 1 && n->list[0]->kind == NK::Block) {
                    genStatements(n->list[0]->list);
                } else {
                    genStatements(n->list);
                }
            }
            depth_--;
            line("}");
            scopes_.pop_back();
            return;
        }
        case NK::FnDecl: {
            std::string prefix = (pendingExport_ || n->flag2) ? "export " : "";
            pendingExport_ = false;
            std::string head = prefix;
            if (n->flag) head += "async ";
            head += "function";
            if (!n->text.empty()) head += " " + n->text;
            head += "(" + genParams(n) + ") {";
            line(head);
            depth_++;
            scopes_.push_back({});
            for (const auto& pn : n->names) declareName(pn);
            pushAssignCounts(n->list);
            genStatements(n->list);
            popAssignCounts();
            scopes_.pop_back();
            depth_--;
            line("}");
            return;
        }
        case NK::Return: {
            if (!n->a) line("return;");
            else line("return " + genExpr(n->a, 0) + ";");
            return;
        }
        case NK::Throw: {
            line("throw " + genExpr(n->a, 0) + ";");
            return;
        }
        case NK::Try: {
            line("try {");
            depth_++;
            if (n->a) {
                if (n->a->kind == NK::Block) genStatements(n->a->list);
                else genStatement(n->a);
            }
            depth_--;
            if (n->b) {
                line("} catch (" + (n->text.empty() ? std::string("__shya_error") : n->text) +
                     ") {");
                depth_++;
                if (n->b->kind == NK::Block) genStatements(n->b->list);
                else genStatement(n->b);
                depth_--;
            }
            if (n->c) {
                line("} finally {");
                depth_++;
                if (n->c->kind == NK::Block) genStatements(n->c->list);
                else genStatement(n->c);
                depth_--;
            }
            line("}");
            return;
        }
        case NK::Break:
        case NK::Continue: {
            line(std::string(n->kind == NK::Break ? "break" : "continue") +
                 (n->text.empty() ? "" : " " + n->text) + ";");
            return;
        }
        case NK::Import: {
            if (n->flag2) {
                // A .shya macro import should have been consumed by the module
                // loader; never emit it into JavaScript.
                bag_.warning(n->pos, "CGN007",
                             "`.shya` 宏导入没有被解析（编译器内部问题），已忽略该 import");
                return;
            }
            line("import " + trimTrailingNewline(n->raw) + ";");
            return;
        }
        case NK::Export: {
            if (n->flag) {
                line("export default " + genExpr(n->a, 0) + ";");
                return;
            }
            if (!n->raw.empty()) {
                line("export " + trimTrailingNewline(n->raw) + ";");
                return;
            }
            if (n->a) {
                pendingExport_ = true;
                genStatement(n->a);
                pendingExport_ = false;
                return;
            }
            return;
        }
        case NK::TsRaw: {
            std::string raw = genTsRaw(n);
            if (raw.empty()) return;
            std::istringstream is(raw);
            std::string l;
            while (std::getline(is, l)) line(l);
            return;
        }
        case NK::MacroDecl:
        case NK::Define:
        case NK::Declare:
            // Compile-time only: macros, `define` constants and host
            // declarations never reach the output.
            return;
        default: {
            std::string e = genExpr(n, 0);
            if (!e.empty()) line(e + ";");
            return;
        }
    }
}

// ------------------------------------------------------------------ driver ---

std::string Codegen::generate(const NodePtr& program, const std::string& sourceFile) {
    file_ = sourceFile;
    out_.clear();
    depth_ = 0;
    scopes_.clear();
    assignCounts_.clear();

    std::string banner = "// Generated by the shya compiler from " + sourceFile + ".\n"
                         "// Target: ES2026. Do not edit by hand.\n\n";
    scopes_.push_back({});
    pushAssignCounts(program->list);
    genStatements(program->list);
    popAssignCounts();
    scopes_.pop_back();

    std::string body = out_;
    // Collapse runs of blank lines.
    std::string collapsed;
    int blank = 0;
    std::istringstream is(body);
    std::string l;
    while (std::getline(is, l)) {
        bool empty = l.find_first_not_of(" \t\r") == std::string::npos;
        if (empty) {
            ++blank;
            if (blank > 1) continue;
        } else {
            blank = 0;
        }
        collapsed += l;
        collapsed += '\n';
    }
    return banner + collapsed;
}

}  // namespace shya
