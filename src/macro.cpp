// shya - macro expansion / desugaring: template AST -> core AST.
#include "shya.h"

#include <cctype>
#include <cstdlib>

namespace shya {

namespace {

const char kMarker = '\x01';

std::string slotTypeNameImpl(SlotType t) {
    switch (t) {
        case SlotType::Expr: return "expr";
        case SlotType::Stmt: return "stmt";
        case SlotType::Type: return "type";
        case SlotType::ExprList: return "expr[]";
        case SlotType::CallExpr: return "callExpr";
        case SlotType::SafeCallExpr: return "safeCallExpr";
        case SlotType::RangeExpr: return "rangeExpr";
        case SlotType::Unknown: return "unknown";
    }
    return "unknown";
}

SlotType slotTypeFromName(const std::string& s) {
    if (s.empty() || s == "expr") return SlotType::Expr;
    if (s == "stmt") return SlotType::Stmt;
    if (s == "type") return SlotType::Type;
    if (s == "expr[]" || s == "expr...") return SlotType::ExprList;
    if (s == "callExpr") return SlotType::CallExpr;
    if (s == "safeCallExpr") return SlotType::SafeCallExpr;
    if (s == "rangeExpr") return SlotType::RangeExpr;
    return SlotType::Unknown;
}

// ------------------------------------------------- AST node kinds as types ---
//
// Every "structurally safe" AST node kind can be named in a macro parameter
// annotation, so a macro can require e.g. `#cond: binary` or `#body: block`.
// The macro-template internals (program, empty, macroDecl, when, whenArm,
// each, slotList, optionalize, typeRef) are deliberately NOT addressable:
// substituting one of them into a template would rewrite the template itself.

struct AstKindEntry {
    const char* name;
    std::vector<NK> kinds;
};

const std::vector<AstKindEntry>& astKindTable() {
    static const std::vector<AstKindEntry> table = {
        {"numLit", {NK::Num}},
        {"mathLit", {NK::MathConst}},
        {"strLit", {NK::Str}},
        {"tplLit", {NK::Tpl}},
        {"boolLit", {NK::Bool}},
        {"voidLit", {NK::Void}},
        {"ident", {NK::Ident}},
        {"arrayLit", {NK::ArrayLit}},
        {"objectLit", {NK::ObjectLit}},
        {"prop", {NK::Prop}},
        {"unary", {NK::Unary}},
        {"binary", {NK::Binary}},
        {"compare", {NK::Compare}},
        {"ternary", {NK::Ternary}},
        {"call", {NK::Call}},
        {"member", {NK::Member}},
        {"index", {NK::Index}},
        {"spread", {NK::Spread}},
        {"await", {NK::Await}},
        {"macroApply", {NK::MacroApply}},
        {"slotRef", {NK::SlotRef}},
        {"tsRaw", {NK::TsRaw}},
        {"rangeExpr", {NK::RangeExpr}},
        {"assign", {NK::Assign}},
        {"decl", {NK::Decl}},
        {"incDec", {NK::IncDec}},
        {"ifStmt", {NK::If}},
        {"caseStmt", {NK::Case}},
        {"caseArm", {NK::CaseArm}},
        {"whileStmt", {NK::ForWhile}},
        {"forOf", {NK::ForOf}},
        {"forRange", {NK::ForRange}},
        {"fnDecl", {NK::FnDecl}},
        {"returnStmt", {NK::Return}},
        {"throwStmt", {NK::Throw}},
        {"tryStmt", {NK::Try}},
        {"breakStmt", {NK::Break}},
        {"continueStmt", {NK::Continue}},
        {"importStmt", {NK::Import}},
        {"exportStmt", {NK::Export}},
        {"block", {NK::Block}},
        {"declareStmt", {NK::Declare}},
        {"exprStmt", {NK::ExprStmt}},
    };
    return table;
}

}  // namespace

std::vector<NK> astKindsForTypeName(const std::string& name) {
    for (const auto& e : astKindTable()) {
        if (name == e.name) return e.kinds;
    }
    return {};
}

const std::vector<std::string>& astSlotTypeNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v = {"expr", "stmt", "type", "expr[]", "callExpr",
                                      "safeCallExpr"};
        for (const auto& e : astKindTable()) v.push_back(e.name);
        return v;
    }();
    return names;
}

namespace {

// A `_` placeholder or an empty node renders as nothing.
bool isNilNode(const NodePtr& n) {
    return !n || n->kind == NK::Empty || (n->kind == NK::Ident && n->flag && n->text == "_");
}

NodePtr makeNil(const Pos& p) { return mk(NK::Empty, p); }

NodePtr makeConcat(const std::vector<NodePtr>& parts, const Pos& p) {
    std::vector<NodePtr> live;
    for (const auto& x : parts) {
        if (isNilNode(x)) continue;
        if (x->kind == NK::Binary && x->text == "concat") {
            // flatten
            std::function<void(const NodePtr&)> push = [&](const NodePtr& y) {
                if (y->kind == NK::Binary && y->text == "concat") {
                    push(y->a);
                    if (y->b) push(y->b);
                } else if (!isNilNode(y)) {
                    live.push_back(y);
                }
            };
            push(x);
            continue;
        }
        live.push_back(x);
    }
    if (live.empty()) return makeNil(p);
    if (live.size() == 1) return live[0];
    NodePtr acc = live[0];
    for (std::size_t i = 1; i < live.size(); ++i) {
        auto n = mk(NK::Binary, p);
        n->text = "concat";
        n->a = acc;
        n->b = live[i];
        acc = n;
    }
    return acc;
}

}  // namespace

std::string MacroExpander::slotTypeName(SlotType t) const { return slotTypeNameImpl(t); }

// ------------------------------------------------------------------ setup ---

MacroExpander::MacroExpander(DiagBag& bag) : bag_(bag) {}

void MacroExpander::registerMacro(const MacroDef& def) {
    auto it = macros_.find(def.name);
    if (it != macros_.end()) {
        if (it->second.system) {
            // A user macro may extend or replace a standard-library macro.
            it->second = def;
            return;
        }
        bag_.error(def.pos, "MAC001", "宏 `@" + def.name + "` 重复定义");
        return;
    }
    macros_.emplace(def.name, def);
}

// --------------------------------------------------------- define inlining ---

void MacroExpander::collectDefines(const NodePtr& n) {
    if (!n) return;
    if (n->kind == NK::Define) {
        NodePtr init = n->a ? substituteDefines(n->a) : nullptr;
        NodePtr folded = init ? foldConstant(init) : nullptr;
        if (!folded) {
            bag_.error(n->pos, "MAC019",
                       "`define " + n->text +
                           "` 的初始值不是编译期常量（define 只接受字面量、数学字面量、其它 define 与"
                           "它们的常量运算）");
            folded = mk(NK::Void, n->pos);
        }
        defines_[n->text] = folded;
        return;  // the initializer itself is fully folded already
    }
    for (const auto& k : {n->a, n->b, n->c, n->d}) collectDefines(k);
    for (const auto& k : n->targets) collectDefines(k);
    for (const auto& k : n->values) collectDefines(k);
    for (const auto& k : n->patterns) collectDefines(k);
    for (const auto& k : n->list) collectDefines(k);
    for (const auto& k : n->defaults) collectDefines(k);
}

NodePtr MacroExpander::substituteDefines(const NodePtr& n) {
    if (!n) return nullptr;
    if (n->kind == NK::Define) return n;  // dropped by the expansion pass
    if (n->kind == NK::Ident && !n->flag) {
        auto it = defines_.find(n->text);
        if (it != defines_.end()) return cloneNode(it->second);
    }
    auto c = std::make_shared<Node>(*n);
    c->a = substituteDefines(n->a);
    c->b = substituteDefines(n->b);
    c->c = substituteDefines(n->c);
    c->d = substituteDefines(n->d);
    c->targets.clear();
    for (const auto& k : n->targets) c->targets.push_back(substituteDefines(k));
    c->values.clear();
    for (const auto& k : n->values) c->values.push_back(substituteDefines(k));
    c->patterns.clear();
    for (const auto& k : n->patterns) c->patterns.push_back(substituteDefines(k));
    c->defaults.clear();
    for (const auto& k : n->defaults) c->defaults.push_back(substituteDefines(k));
    c->list.clear();
    for (const auto& k : n->list) c->list.push_back(substituteDefines(k));
    return c;
}

NodePtr MacroExpander::foldConstant(const NodePtr& n) const {
    if (!n) return nullptr;
    switch (n->kind) {
        case NK::Num:
        case NK::MathConst: {
            auto r = mk(NK::Num, n->pos);
            r->num = n->num;
            return r;
        }
        case NK::Str:
        case NK::Bool:
        case NK::Void:
            return cloneNodePublic(n);
        case NK::Unary: {
            NodePtr v = foldConstant(n->a);
            if (!v) return nullptr;
            if (n->text == "-" && v->kind == NK::Num) {
                auto r = mk(NK::Num, n->pos);
                r->num = -v->num;
                return r;
            }
            if (n->text == "+" && v->kind == NK::Num) return v;
            if (n->text == "!" && v->kind == NK::Bool) {
                auto r = mk(NK::Bool, n->pos);
                r->flag = !v->flag;
                return r;
            }
            return nullptr;
        }
        case NK::Binary: {
            NodePtr l = foldConstant(n->a);
            NodePtr r = foldConstant(n->b);
            if (!l || !r) return nullptr;
            if (n->text == "+" && l->kind == NK::Str && r->kind == NK::Str) {
                auto s = mk(NK::Str, n->pos);
                s->text = l->text + r->text;
                return s;
            }
            if (l->kind != NK::Num || r->kind != NK::Num) return nullptr;
            double a = l->num, b = r->num, v = 0;
            const std::string& op = n->text;
            if (op == "+") v = a + b;
            else if (op == "-") v = a - b;
            else if (op == "*") v = a * b;
            else if (op == "/") v = a / b;
            else if (op == "~/") v = a / b >= 0 ? std::floor(a / b) : std::ceil(a / b);
            else if (op == "+/") v = std::ceil(a / b);
            else if (op == "-/") v = std::floor(a / b);
            else if (op == "\\/") v = std::round(a / b);
            else if (op == "%") v = std::fmod(a, b);
            else if (op == "^") v = std::pow(a, b);
            else return nullptr;
            auto out = mk(NK::Num, n->pos);
            out->num = v;
            return out;
        }
        default:
            return nullptr;
    }
}

void MacroExpander::installStdlib() {
    if (stdlibInstalled_) return;
    stdlibInstalled_ = true;
    Lexer lex(kStdlibSource, "<stdlib>");
    DiagBag sub;
    auto toks = lex.tokenize(sub);
    Parser p(toks, sub);
    NodePtr prog = p.parseProgram();
    if (sub.hasError()) {
        // The embedded standard library must always parse; surface it loudly.
        // Positions refer to <stdlib>, so they are reported as text, not as a
        // source excerpt of the user's file.
        int n = 0;
        for (const auto& d : sub.items()) {
            if (++n > 8) break;
            bag_.error(Pos{1, 1, 0}, "MAC002",
                       "内建标准库解析失败（<stdlib>:" + std::to_string(d.pos.line) + ":" +
                           std::to_string(d.pos.col) + "）：" + d.message);
        }
        return;
    }
    for (const auto& st : prog->list) {
        if (st->kind != NK::MacroDecl) continue;
        MacroDef def;
        def.name = st->text;
        def.pos = st->pos;
        def.system = true;
        for (std::size_t i = 0; i < st->names.size(); ++i) {
            std::string tn = i < st->typeAnns.size() ? st->typeAnns[i] : "";
            def.params.push_back(st->names[i]);
            def.paramTypes.push_back(slotTypeFromName(tn));
            def.paramTypeNames.push_back(tn);
            def.paramKinds.push_back(astKindsForTypeName(tn));
            def.variadic.push_back(i < st->flags.size() ? st->flags[i] : false);
        }
        def.body = st->list;
        registerMacro(def);
    }
}

void MacroExpander::registerMacroFromDecl(const NodePtr& n) {
    if (!n || n->kind != NK::MacroDecl) return;
    MacroDef def;
    def.name = n->text;
    def.pos = n->pos;
    for (std::size_t i = 0; i < n->names.size(); ++i) {
        std::string tn = i < n->typeAnns.size() ? n->typeAnns[i] : "";
        def.params.push_back(n->names[i]);
        def.paramTypes.push_back(slotTypeFromName(tn));
        def.paramTypeNames.push_back(tn);
        def.paramKinds.push_back(astKindsForTypeName(tn));
        def.variadic.push_back(i < n->flags.size() ? n->flags[i] : false);
    }
    def.body = n->list;
    registerMacro(def);
}

void MacroExpander::collectMacroDecls(const NodePtr& n) {
    if (!n) return;
    if (n->kind == NK::MacroDecl) {
        registerMacroFromDecl(n);
        return;
    }
    for (const auto& k : {n->a, n->b, n->c, n->d}) collectMacroDecls(k);
    for (const auto& k : n->list) collectMacroDecls(k);
    for (const auto& k : n->targets) collectMacroDecls(k);
    for (const auto& k : n->values) collectMacroDecls(k);
    for (const auto& k : n->patterns) collectMacroDecls(k);
}

// -------------------------------------------------------------- utilities ---

SlotType MacroExpander::inferSlotType(const NodePtr& n) const {
    if (isNilNode(n)) return SlotType::Expr;
    switch (n->kind) {
        case NK::Call:
            return n->flag ? SlotType::SafeCallExpr : SlotType::CallExpr;
        case NK::MacroApply:
            // `@safe x y` style fragments are safe calls at heart.
            if (n->text == "safe" || n->text == "safe_share") return SlotType::SafeCallExpr;
            return SlotType::Expr;
        case NK::RangeExpr:
            return SlotType::RangeExpr;
        case NK::TypeRef:
            return SlotType::Type;
        case NK::Block:
        case NK::Program:
            return SlotType::Stmt;
        case NK::ExprStmt:
        case NK::Assign:
        case NK::Decl:
        case NK::Define:
        case NK::If:
        case NK::Case:
        case NK::ForWhile:
        case NK::ForOf:
        case NK::ForRange:
        case NK::FnDecl:
        case NK::Return:
        case NK::Throw:
        case NK::Try:
        case NK::Break:
        case NK::Continue:
        case NK::Import:
        case NK::Export:
        case NK::MacroDecl:
            return SlotType::Stmt;
        case NK::Compare:
            // `#x is type` used as a macro argument is a compile-time test.
            return SlotType::Expr;
        default:
            return SlotType::Expr;
    }
}

bool MacroExpander::typeMatches(SlotType want, SlotType got) const {
    if (want == SlotType::Unknown || want == SlotType::Expr) return true;
    if (want == got) return true;
    switch (want) {
        case SlotType::CallExpr:
            return got == SlotType::SafeCallExpr || got == SlotType::Expr;
        case SlotType::SafeCallExpr:
            return got == SlotType::CallExpr || got == SlotType::Expr;
        case SlotType::ExprList:
            return got == SlotType::ExprList;
        case SlotType::RangeExpr:
            return got == SlotType::RangeExpr;
        case SlotType::Stmt:
            return got == SlotType::Stmt;
        case SlotType::Type:
            return got == SlotType::Type || got == SlotType::Expr;
        default:
            return false;
    }
}

NodePtr MacroExpander::cloneNode(const NodePtr& n) const {
    if (!n) return nullptr;
    auto c = std::make_shared<Node>(*n);
    c->a = cloneNode(n->a);
    c->b = cloneNode(n->b);
    c->c = cloneNode(n->c);
    c->d = cloneNode(n->d);
    c->list.clear();
    for (const auto& k : n->list) c->list.push_back(cloneNode(k));
    c->targets.clear();
    for (const auto& k : n->targets) c->targets.push_back(cloneNode(k));
    c->values.clear();
    for (const auto& k : n->values) c->values.push_back(cloneNode(k));
    c->patterns.clear();
    for (const auto& k : n->patterns) c->patterns.push_back(cloneNode(k));
    c->defaults.clear();
    for (const auto& k : n->defaults) c->defaults.push_back(cloneNode(k));
    return c;
}

// --------------------------------------------------- static type inference ---

namespace {
class StaticTyper {
public:
    explicit StaticTyper(const std::unordered_map<std::string, TypePtr>& env,
                         const std::unordered_map<std::string, TypePtr>* declared = nullptr)
        : env_(env), declared_(declared) {}

    TypePtr type(const NodePtr& n) const {
        if (!n) return tUnknown();
        switch (n->kind) {
            case NK::Num:
            case NK::MathConst:
                return tNum();
            case NK::Str:
            case NK::Tpl:
                return tStr();
            case NK::Bool:
                return tBool();
            case NK::Void:
                return tVoid();
            case NK::ArrayLit: {
                if (n->flag) return tUnknown();  // comma sequence, not an array
                TypePtr elem = n->list.empty() ? tUnknown() : type(n->list[0]);
                return tArray(elem);
            }
            case NK::ObjectLit:
                return tObject();
            case NK::Ident:
                if (n->flag && n->text == "_") return tUnknown();
                {
                    auto it = env_.find(n->text);
                    if (it != env_.end()) return it->second;
                }
                return tUnknown();
            case NK::Unary:
                if (n->text == "!") return tBool();
                if (n->text == "typeof") return tStr();
                if (n->text == "-" || n->text == "+") return tNum();
                if (n->text == "new") {
                    // `new Map()` / `new Set()` / `new Array()` are inferable.
                    std::string name;
                    const NodePtr& inner = n->a;
                    if (inner && inner->kind == NK::Call && inner->a &&
                        inner->a->kind == NK::Ident)
                        name = inner->a->text;
                    else if (inner && inner->kind == NK::Ident)
                        name = inner->text;
                    if (name == "Map") return tMap(tUnknown(), tUnknown());
                    if (name == "Set") return tSet(tUnknown());
                    if (name == "Array") return tArray(tUnknown());
                }
                return tUnknown();
            case NK::Binary: {
                if (n->text == "concat") return tUnknown();
                if (n->text == "&&" || n->text == "||") {
                    TypePtr l = type(n->a), r = type(n->b);
                    if (typeEquals(l, r)) return l;
                    return tUnknown();
                }
                if (n->text == "+") {
                    TypePtr l = type(n->a), r = type(n->b);
                    if (l->kind == TK::Str || r->kind == TK::Str) return tStr();
                    if (l->kind == TK::Num && r->kind == TK::Num) return tNum();
                    return tUnknown();
                }
                TypePtr l = type(n->a), r = type(n->b);
                if (l->kind == TK::Num && r->kind == TK::Num) return tNum();
                return tUnknown();
            }
            case NK::Compare:
                return tBool();
            case NK::Ternary: {
                TypePtr l = type(n->b), r = type(n->c);
                if (typeEquals(l, r)) return l;
                return tUnknown();
            }
            case NK::Index: {
                TypePtr o = type(n->a);
                if (o->kind == TK::Array) return o->elem ? o->elem : tUnknown();
                if (o->kind == TK::Map) return o->elem ? o->elem : tUnknown();
                if (o->kind == TK::Str) return tStr();
                if (o->kind == TK::Range) return o->elem ? o->elem : tUnknown();
                return tUnknown();
            }
            case NK::SlotRef:
                return tUnknown();
            case NK::Member: {
                // `p hp` where `p: Player` and Player is `declare`d.
                if (!declared_) return tUnknown();
                TypePtr obj = type(n->a);
                if (!obj || obj->kind != TK::Class) return tUnknown();
                auto it = declared_->find(obj->name);
                if (it == declared_->end()) return tUnknown();
                const TypePtr& t = it->second;
                if (!n->b && !n->text.empty()) {
                    auto f = t->fields.find(n->text);
                    if (f != t->fields.end()) return f->second;
                    auto m = t->methods.find(n->text);
                    if (m != t->methods.end()) return m->second;
                }
                return tUnknown();
            }
            default:
                return tUnknown();
        }
    }

private:
    const std::unordered_map<std::string, TypePtr>& env_;
    const std::unordered_map<std::string, TypePtr>* declared_ = nullptr;
};
}  // namespace

// --------------------------------------------------------------- when eval ---

namespace {
enum class Tri { True, False, Unknown };

Tri triNot(Tri t) {
    if (t == Tri::True) return Tri::False;
    if (t == Tri::False) return Tri::True;
    return Tri::Unknown;
}
}  // namespace

bool MacroExpander::evalWhen(const NodePtr& cond,
                             const std::unordered_map<std::string, NodePtr>& binds) const {
    std::function<Tri(const NodePtr&)> ev = [&](const NodePtr& n) -> Tri {
        if (!n) return Tri::False;
        if (n->kind == NK::Bool) return n->flag ? Tri::True : Tri::False;
        if (n->kind == NK::Unary && n->text == "!") return triNot(ev(n->a));
        if (n->kind == NK::Binary && n->text == "&&") {
            Tri l = ev(n->a), r = ev(n->b);
            if (l == Tri::False || r == Tri::False) return Tri::False;
            if (l == Tri::True && r == Tri::True) return Tri::True;
            return Tri::Unknown;
        }
        if (n->kind == NK::Binary && n->text == "||") {
            Tri l = ev(n->a), r = ev(n->b);
            if (l == Tri::True || r == Tri::True) return Tri::True;
            if (l == Tri::False && r == Tri::False) return Tri::False;
            return Tri::Unknown;
        }
        if (n->kind == NK::Compare && (n->text == "is" || n->text == "is not")) {
            NodePtr lhs = n->a;
            // A comparison chain keeps its right-hand operands in `list`.
            NodePtr rhs = n->b ? n->b : (n->list.empty() ? nullptr : n->list[0]);
            std::string want = rhs && rhs->kind == NK::Ident ? rhs->text : "";
            bool neg = n->text == "is not";
            Tri res = Tri::Unknown;
            if (lhs && lhs->kind == NK::SlotRef) {
                auto it = binds.find(lhs->text);
                if (it == binds.end()) {
                    return Tri::Unknown;
                }
                const NodePtr& arg = it->second;
                SlotType sk = inferSlotType(arg);
                if (want == "stmt") res = sk == SlotType::Stmt ? Tri::True : Tri::False;
                else if (want == "callExpr") res = sk == SlotType::CallExpr ? Tri::True : Tri::False;
                else if (want == "safeCallExpr")
                    res = sk == SlotType::SafeCallExpr ? Tri::True : Tri::False;
                else if (want == "rangeExpr")
                    res = sk == SlotType::RangeExpr ? Tri::True : Tri::False;
                else if (want == "expr")
                    res = (sk != SlotType::Stmt && sk != SlotType::Type) ? Tri::True : Tri::False;
                else {
                    // shya type test against the argument's static type
                    StaticTyper st(env_, &declaredTypes_);
                    TypePtr t = st.type(arg);
                    res = Tri::Unknown;
                    auto isKind = [&](TK k) {
                        if (t->kind == TK::Unknown) return Tri::Unknown;
                        return t->kind == k ? Tri::True : Tri::False;
                    };
                    if (want == "array") res = isKind(TK::Array);
                    else if (want == "string") res = isKind(TK::Str);
                    else if (want == "number") res = isKind(TK::Num);
                    else if (want == "boolean") res = isKind(TK::Bool);
                    else if (want == "object") res = isKind(TK::Object);
                    else if (want == "map") res = isKind(TK::Map);
                    else if (want == "set") res = isKind(TK::Set);
                    else if (want == "fn") res = isKind(TK::Fn);
                    else if (want == "void") res = isKind(TK::Void);
                    else if (want == "rangeExpr") res = isKind(TK::Range);
                    else if (want == "unknown") res = isKind(TK::Unknown);
                    else if (!astKindsForTypeName(want).empty()) {
                        // A concrete AST node kind: `@when(#x is binary)`.
                        res = Tri::False;
                        for (NK k : astKindsForTypeName(want)) {
                            if (arg->kind == k) res = Tri::True;
                        }
                    } else res = Tri::Unknown;
                    if (std::getenv("SHYA_DEBUG_WHEN")) {
                        std::fprintf(stderr, "[when] want=%s staticType=%s -> %s\n", want.c_str(),
                                     t->str().c_str(),
                                     res == Tri::True ? "true"
                                                      : (res == Tri::False ? "false" : "unknown"));
                    }
                }
            }
            return neg ? triNot(res) : res;
        }
        if (n->kind == NK::SlotRef) {
            // A bare slot in a condition is truthy when it is not the `_` gap.
            auto it = binds.find(n->text);
            if (it == binds.end()) return Tri::Unknown;
            return isNilNode(it->second) ? Tri::False : Tri::True;
        }
        return Tri::Unknown;
    };
    return ev(cond) == Tri::True;
}

// ------------------------------------------------------------- instantiate ---

namespace {
// Recursion depth guard so a recursive macro cannot hang the compiler.
constexpr int kMaxMacroDepth = 64;
}  // namespace

// The instantiation engine lives in MacroExpander but needs bidirectional
// statement/expression traversal; implemented via free functions that call
// back into the expander through a small context object.
namespace {

struct InstCtx {
    MacroExpander* self;
    const Bindings* binds;
    int depth;
    DiagBag* bag;
};

NodePtr tsSubstitute(const NodePtr& n, const InstCtx& ctx);

// Replaces `#name` inside a raw @ts payload with a marker bound to the
// substituted node, so code generation can render it later.
NodePtr makeTsRaw(const NodePtr& tpl, const InstCtx& ctx) {
    auto out = mk(NK::TsRaw, tpl->pos);
    const std::string& src = tpl->raw;
    std::string text;
    text.reserve(src.size());
    for (std::size_t i = 0; i < src.size();) {
        if (src[i] == '#' &&
            (i + 1 < src.size() &&
             (std::isalpha(static_cast<unsigned char>(src[i + 1])) || src[i + 1] == '_'))) {
            std::size_t j = i + 1;
            std::string name;
            while (j < src.size() &&
                   (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '_'))
                name.push_back(src[j++]);
            auto it = ctx.binds->find(name);
            if (it == ctx.binds->end()) {
                ctx.bag->error(tpl->pos, "MAC003",
                               "`@ts` 中引用了未绑定的插槽 `#" + name + "`");
                i = j;
                continue;
            }
            if (it->second.slot == SlotType::ExprList) {
                ctx.bag->error(tpl->pos, "MAC004",
                               "`@ts` 中不能直接使用不定项插槽 `#" + name + "`");
                i = j;
                continue;
            }
            text.push_back(kMarker);
            text += std::to_string(out->list.size());
            text.push_back(kMarker);
            out->list.push_back(it->second.node ? it->second.node : makeNil(tpl->pos));
            out->names.push_back(name);
            i = j;
            continue;
        }
        text.push_back(src[i]);
        ++i;
    }
    out->raw = text;
    return out;
}

NodePtr instExpr(const NodePtr& tpl, const InstCtx& ctx);

void instStmts(const std::vector<NodePtr>& tpl, const InstCtx& ctx, std::vector<NodePtr>& out);

// Chooses the arms of a @when that should be materialised.
std::vector<NodePtr> chooseWhenArms(const NodePtr& when, const InstCtx& ctx) {
    std::vector<NodePtr> chosen;
    std::unordered_map<std::string, NodePtr> plain;
    for (const auto& kv : *ctx.binds) plain[kv.first] = kv.second.node;
    bool cond = ctx.self->evalWhen(when->a, plain);
    bool anyNamed = false;
    for (const auto& arm : when->list) {
        if (arm->kind == NK::WhenArm && !arm->text.empty()) anyNamed = true;
    }
    if (!anyNamed) {
        if (cond && !when->list.empty()) chosen.push_back(when->list[0]);
        return chosen;
    }
    for (const auto& arm : when->list) {
        if (arm->kind != NK::WhenArm) continue;
        bool take = (arm->text == "y" && cond) || (arm->text == "n" && !cond);
        if (take) chosen.push_back(arm);
    }
    return chosen;
}

NodePtr instWhenExpr(const NodePtr& when, const InstCtx& ctx) {
    std::vector<NodePtr> parts;
    for (const auto& arm : chooseWhenArms(when, ctx)) {
        for (const auto& st : arm->list) {
            if (st->kind == NK::ExprStmt) {
                parts.push_back(instExpr(st->a, ctx));
            } else if (st->kind == NK::Empty) {
                continue;
            } else {
                ctx.bag->error(st->pos, "MAC005",
                               "@when 分支在表达式位置只能包含表达式");
            }
        }
    }
    return makeConcat(parts, when->pos);
}

NodePtr instEachExpr(const NodePtr& each, const InstCtx& ctx) {
    std::vector<NodePtr> parts;
    std::string listName = each->names.empty() ? std::string() : each->names[0];
    auto it = ctx.binds->find(listName);
    if (it == ctx.binds->end()) {
        ctx.bag->error(each->pos, "MAC006", "@each 引用了未绑定的插槽 `#" + listName + "`");
        return makeNil(each->pos);
    }
    std::vector<NodePtr> elems;
    if (it->second.node && it->second.node->kind == NK::ArrayLit && it->second.node->flag) {
        elems = it->second.node->list;
    } else if (it->second.node) {
        elems.push_back(it->second.node);
    }
    for (const auto& e : elems) {
        Bindings nested = *ctx.binds;
        Binding b;
        b.node = e;
        b.slot = ctx.self->inferSlotTypePublic(e);
        nested[each->text] = b;
        InstCtx sub{ctx.self, &nested, ctx.depth, ctx.bag};
        std::vector<NodePtr> body;
        instStmts(each->list, sub, body);
        for (const auto& st : body) {
            if (st->kind == NK::ExprStmt) parts.push_back(st->a);
            else if (st->kind == NK::Empty) continue;
            else ctx.bag->error(each->pos, "MAC007", "@each 在表达式位置只能包含表达式");
        }
    }
    return makeConcat(parts, each->pos);
}

void instEachStmts(const NodePtr& each, const InstCtx& ctx, std::vector<NodePtr>& out) {
    std::string listName = each->names.empty() ? std::string() : each->names[0];
    auto it = ctx.binds->find(listName);
    if (it == ctx.binds->end()) {
        ctx.bag->error(each->pos, "MAC006", "@each 引用了未绑定的插槽 `#" + listName + "`");
        return;
    }
    std::vector<NodePtr> elems;
    if (it->second.node && it->second.node->kind == NK::ArrayLit && it->second.node->flag) {
        elems = it->second.node->list;
    } else if (it->second.node) {
        elems.push_back(it->second.node);
    }
    for (const auto& e : elems) {
        Bindings nested = *ctx.binds;
        Binding b;
        b.node = e;
        b.slot = ctx.self->inferSlotTypePublic(e);
        nested[each->text] = b;
        InstCtx sub{ctx.self, &nested, ctx.depth, ctx.bag};
        instStmts(each->list, sub, out);
    }
}

NodePtr instExpr(const NodePtr& tpl, const InstCtx& ctx) {
    if (!tpl) return nullptr;
    switch (tpl->kind) {
        case NK::Empty:
            return makeNil(tpl->pos);
        case NK::SlotRef: {
            auto it = ctx.binds->find(tpl->text);
            if (it == ctx.binds->end()) {
                ctx.bag->error(tpl->pos, "MAC008", "未绑定的插槽 `#" + tpl->text + "`");
                return makeNil(tpl->pos);
            }
            if (it->second.nil || !it->second.node) return makeNil(tpl->pos);
            return ctx.self->cloneNodePublic(it->second.node);
        }
        case NK::TsRaw:
            return makeTsRaw(tpl, ctx);
        case NK::When:
            return instWhenExpr(tpl, ctx);
        case NK::Each:
            return instEachExpr(tpl, ctx);
        case NK::Optionalize: {
            auto n = mk(NK::Optionalize, tpl->pos);
            n->a = instExpr(tpl->a, ctx);
            return n;
        }
        case NK::Binary: {
            if (tpl->text == "concat") {
                std::vector<NodePtr> parts;
                std::function<void(const NodePtr&)> walk = [&](const NodePtr& x) {
                    if (x && x->kind == NK::Binary && x->text == "concat") {
                        walk(x->a);
                        walk(x->b);
                    } else {
                        parts.push_back(instExpr(x, ctx));
                    }
                };
                walk(tpl);
                return makeConcat(parts, tpl->pos);
            }
            auto n = mk(NK::Binary, tpl->pos);
            n->text = tpl->text;
            n->a = instExpr(tpl->a, ctx);
            n->b = instExpr(tpl->b, ctx);
            return n;
        }
        case NK::Unary: {
            auto n = mk(NK::Unary, tpl->pos);
            n->text = tpl->text;
            n->a = instExpr(tpl->a, ctx);
            return n;
        }
        case NK::Compare: {
            auto n = mk(NK::Compare, tpl->pos);
            n->text = tpl->text;
            n->a = instExpr(tpl->a, ctx);
            n->b = instExpr(tpl->b, ctx);
            for (const auto& r : tpl->list) n->list.push_back(instExpr(r, ctx));
            n->names = tpl->names;
            return n;
        }
        case NK::Ternary: {
            auto n = mk(NK::Ternary, tpl->pos);
            n->a = instExpr(tpl->a, ctx);
            n->b = instExpr(tpl->b, ctx);
            n->c = instExpr(tpl->c, ctx);
            return n;
        }
        case NK::Call: {
            auto n = mk(NK::Call, tpl->pos);
            n->flag = tpl->flag;
            n->flag2 = tpl->flag2;
            n->a = instExpr(tpl->a, ctx);
            for (const auto& x : tpl->list) n->list.push_back(instExpr(x, ctx));
            return n;
        }
        case NK::Member: {
            auto n = mk(NK::Member, tpl->pos);
            n->text = tpl->text;
            n->flag = tpl->flag;
            n->flag2 = tpl->flag2;
            n->a = instExpr(tpl->a, ctx);
            n->b = tpl->b ? instExpr(tpl->b, ctx) : nullptr;
            for (const auto& x : tpl->list) n->list.push_back(instExpr(x, ctx));
            return n;
        }
        case NK::Index: {
            auto n = mk(NK::Index, tpl->pos);
            n->a = instExpr(tpl->a, ctx);
            n->b = instExpr(tpl->b, ctx);
            return n;
        }
        case NK::ArrayLit: {
            auto n = mk(NK::ArrayLit, tpl->pos);
            n->flag = tpl->flag;
            for (const auto& x : tpl->list) n->list.push_back(instExpr(x, ctx));
            return n;
        }
        case NK::ObjectLit: {
            auto n = mk(NK::ObjectLit, tpl->pos);
            for (const auto& x : tpl->list) n->list.push_back(instExpr(x, ctx));
            return n;
        }
        case NK::Prop: {
            auto n = mk(NK::Prop, tpl->pos);
            n->text = tpl->text;
            n->a = instExpr(tpl->a, ctx);
            return n;
        }
        case NK::Spread:
        case NK::Await: {
            auto n = mk(tpl->kind, tpl->pos);
            n->text = tpl->text;
            n->a = instExpr(tpl->a, ctx);
            return n;
        }
        case NK::RangeExpr: {
            auto n = mk(NK::RangeExpr, tpl->pos);
            n->a = instExpr(tpl->a, ctx);
            n->b = instExpr(tpl->b, ctx);
            n->c = tpl->c ? instExpr(tpl->c, ctx) : nullptr;
            return n;
        }
        case NK::MacroApply: {
            auto n = mk(NK::MacroApply, tpl->pos);
            n->text = tpl->text;
            n->a = instExpr(tpl->a, ctx);
            for (const auto& x : tpl->list) n->list.push_back(instExpr(x, ctx));
            return n;
        }
        case NK::FnDecl: {
            auto n = mk(NK::FnDecl, tpl->pos);
            n->text = tpl->text;
            n->flag = tpl->flag;
            n->flag2 = tpl->flag2;
            n->names = tpl->names;
            n->typeAnns = tpl->typeAnns;
            n->typeAnn = tpl->typeAnn;
            n->typeAnn2 = tpl->typeAnn2;
            for (const auto& d : tpl->defaults) n->defaults.push_back(instExpr(d, ctx));
            instStmts(tpl->list, ctx, n->list);
            return n;
        }
        case NK::Block:
        case NK::Program: {
            auto n = mk(tpl->kind, tpl->pos);
            instStmts(tpl->list, ctx, n->list);
            return n;
        }
        default: {
            // Statements reaching expression position (or unknown kinds) are
            // cloned with their expression children substituted.
            return ctx.self->cloneNodePublic(tpl);
        }
    }
}

void instStmts(const std::vector<NodePtr>& tpl, const InstCtx& ctx, std::vector<NodePtr>& out) {
    for (const auto& st : tpl) {
        if (!st) continue;
        switch (st->kind) {
            case NK::When: {
                for (const auto& arm : chooseWhenArms(st, ctx)) instStmts(arm->list, ctx, out);
                break;
            }
            case NK::Each:
                instEachStmts(st, ctx, out);
                break;
            case NK::MacroDecl:
                break;  // macros are collected separately
            case NK::ExprStmt: {
                // A statement whose whole expression is a @when / @each splice
                // must be materialised in statement position, so that the arms
                // become statements rather than expression fragments.
                if (st->a && st->a->kind == NK::When) {
                    for (const auto& arm : chooseWhenArms(st->a, ctx))
                        instStmts(arm->list, ctx, out);
                    break;
                }
                if (st->a && st->a->kind == NK::Each) {
                    instEachStmts(st->a, ctx, out);
                    break;
                }
                auto n = mk(NK::ExprStmt, st->pos);
                n->a = instExpr(st->a, ctx);
                out.push_back(n);
                break;
            }
            case NK::Block:
            case NK::Program: {
                auto n = mk(st->kind, st->pos);
                instStmts(st->list, ctx, n->list);
                out.push_back(n);
                break;
            }
            case NK::If: {
                auto n = mk(NK::If, st->pos);
                n->a = instExpr(st->a, ctx);
                n->b = instExpr(st->b, ctx);
                n->c = instExpr(st->c, ctx);
                out.push_back(n);
                break;
            }
            case NK::Case: {
                auto n = mk(NK::Case, st->pos);
                n->a = instExpr(st->a, ctx);
                for (const auto& arm : st->list) {
                    auto a2 = mk(NK::CaseArm, arm->pos);
                    a2->flag = arm->flag;
                    a2->flag2 = arm->flag2;
                    for (const auto& p : arm->patterns) a2->patterns.push_back(instExpr(p, ctx));
                    instStmts(arm->list, ctx, a2->list);
                    n->list.push_back(a2);
                }
                out.push_back(n);
                break;
            }
            case NK::ForWhile: {
                auto n = mk(NK::ForWhile, st->pos);
                n->a = instExpr(st->a, ctx);
                n->b = instExpr(st->b, ctx);
                out.push_back(n);
                break;
            }
            case NK::ForOf:
            case NK::ForRange: {
                auto n = mk(st->kind, st->pos);
                n->names = st->names;
                n->text = st->text;
                n->flag = st->flag;
                n->a = instExpr(st->a, ctx);
                n->b = instExpr(st->b, ctx);
                n->c = instExpr(st->c, ctx);
                n->d = instExpr(st->d, ctx);
                out.push_back(n);
                break;
            }
            case NK::FnDecl: {
                auto n = mk(NK::FnDecl, st->pos);
                n->text = st->text;
                n->flag = st->flag;
                n->flag2 = st->flag2;
                n->names = st->names;
                n->typeAnns = st->typeAnns;
                n->typeAnn = st->typeAnn;
                n->typeAnn2 = st->typeAnn2;
                for (const auto& d : st->defaults) n->defaults.push_back(instExpr(d, ctx));
                instStmts(st->list, ctx, n->list);
                out.push_back(n);
                break;
            }
            case NK::Return:
            case NK::Throw: {
                auto n = mk(st->kind, st->pos);
                n->a = instExpr(st->a, ctx);
                out.push_back(n);
                break;
            }
            case NK::Try: {
                auto n = mk(NK::Try, st->pos);
                n->text = st->text;
                instStmts({st->a}, ctx, n->list);
                instStmts({st->b}, ctx, n->list);
                instStmts({st->c}, ctx, n->list);
                out.push_back(n);
                break;
            }
            case NK::Decl:
            case NK::Define: {
                auto n = mk(st->kind, st->pos);
                n->text = st->text;
                n->typeAnn = st->typeAnn;
                n->names = st->names;
                n->a = instExpr(st->a, ctx);
                for (const auto& d : st->list) {
                    auto c = mk(NK::Decl, d->pos);
                    c->text = d->text;
                    c->typeAnn = d->typeAnn;
                    c->names = d->names;
                    c->a = instExpr(d->a, ctx);
                    n->list.push_back(c);
                }
                out.push_back(n);
                break;
            }
            case NK::Assign: {
                auto n = mk(NK::Assign, st->pos);
                n->text = st->text;  // compound-assignment operator, if any
                for (const auto& t : st->targets) n->targets.push_back(instExpr(t, ctx));
                for (const auto& v : st->values) n->values.push_back(instExpr(v, ctx));
                out.push_back(n);
                break;
            }
            case NK::IncDec: {
                auto n = mk(NK::IncDec, st->pos);
                n->text = st->text;
                n->a = instExpr(st->a, ctx);
                out.push_back(n);
                break;
            }
            case NK::Export: {
                auto n = mk(NK::Export, st->pos);
                n->flag = st->flag;
                n->raw = st->raw;
                if (st->a) {
                    std::vector<NodePtr> inner;
                    instStmts({st->a}, ctx, inner);
                    if (!inner.empty()) n->a = inner[0];
                    if (inner.size() > 1) {
                        auto blk = mk(NK::Block, st->pos);
                        blk->list = inner;
                        n->a = blk;
                    }
                }
                out.push_back(n);
                break;
            }
            case NK::Import:
            case NK::Empty:
            case NK::Break:
            case NK::Continue:
                out.push_back(ctx.self->cloneNodePublic(st));
                break;
            default: {
                auto n = mk(NK::ExprStmt, st->pos);
                n->a = instExpr(st, ctx);
                out.push_back(n);
                break;
            }
        }
    }
}

}  // namespace

// ---------------------------------------------- public expansion interface ---

NodePtr MacroExpander::cloneNodePublic(const NodePtr& n) const { return cloneNode(n); }
SlotType MacroExpander::inferSlotTypePublic(const NodePtr& n) const { return inferSlotType(n); }

// ------------------------------------------------------------------ expand ---

NodePtr MacroExpander::expand(const NodePtr& program) {
    // 1. standards library first, then user macros (user macros may override).
    installStdlib();
    collectMacroDecls(program);

    // 2. inline `define` compile-time constants.
    collectDefines(program);
    NodePtr work = defines_.empty() ? program : substituteDefines(program);

    // 3. build a heuristic static type environment for @when decisions
    std::function<void(const NodePtr&)> scanTypes = [&](const NodePtr& n) {
        if (!n) return;
        if (n->kind == NK::Declare) {
            TypePtr t = buildDeclaredType(n);
            if (t && !n->text.empty()) {
                auto it = declaredTypes_.find(n->text);
                if (it != declaredTypes_.end() && it->second->kind == TK::Class &&
                    t->kind == TK::Class) {
                    for (const auto& kv : t->fields) it->second->fields[kv.first] = kv.second;
                    for (const auto& kv : t->methods) it->second->methods[kv.first] = kv.second;
                } else {
                    declaredTypes_[n->text] = t;
                }
            }
        }
        if (n->kind == NK::Decl) {
            for (const auto& d : n->list) {
                if (d->names.empty()) continue;
                TypePtr t;
                if (!d->typeAnn.empty()) t = parseTypeString(d->typeAnn);
                else if (d->a) {
                    StaticTyper st(env_, &declaredTypes_);
                    t = st.type(d->a);
                }
                if (t && env_.find(d->names[0]) == env_.end()) env_[d->names[0]] = t;
            }
        }
        if (n->kind == NK::FnDecl) {
            for (std::size_t i = 0; i < n->names.size() && i < n->typeAnns.size(); ++i) {
                std::string ann = n->typeAnns[i];
                if (ann.rfind("...", 0) == 0) ann = ann.substr(3);
                if (!ann.empty()) env_[n->names[i]] = parseTypeString(ann);
            }
        }
        for (const auto& k : {n->a, n->b, n->c, n->d}) scanTypes(k);
        for (const auto& k : n->list) scanTypes(k);
        for (const auto& k : n->values) scanTypes(k);
    };
    scanTypes(work);

    // 4. expand macros / desugar
    auto out = mk(NK::Program, program->pos);
    expandList(work->list, out->list);
    return out;
}

void MacroExpander::expandList(const std::vector<NodePtr>& list, std::vector<NodePtr>& out) {
    auto produced = expandStmts(list);
    for (const auto& s : produced) out.push_back(s);
}

NodePtr MacroExpander::expandNode(const NodePtr& n) {
    if (!n) return nullptr;
    switch (n->kind) {
        case NK::MacroApply:
            return expandMacroApply(n, 0);
        case NK::When:
        case NK::Each:
            bag_.error(n->pos, "MAC009",
                       std::string(n->kind == NK::When ? "@when" : "@each") +
                           " 只能出现在宏定义体内");
            return nullptr;
        case NK::ForOf: {
            // Materialise `@range` into a counted loop.
            if (n->a && n->a->kind == NK::MacroApply && n->a->text == "range") {
                NodePtr rng = n->a;
                const NodePtr& coll = rng->a;
                NodePtr rangeNode = !rng->list.empty() ? rng->list[0] : nullptr;
                if (!rangeNode || rangeNode->kind != NK::RangeExpr) {
                    bag_.error(n->pos, "MAC010",
                               "`@range` 需要一个范围表达式，例如 `@range 0:10,2`");
                    return nullptr;
                }
                auto n2 = mk(NK::ForRange, n->pos);
                n2->text = n->names.empty() ? std::string() : n->names[0];
                if (n->names.size() > 1) {
                    bag_.error(n->pos, "MAC011", "`@range` 循环只支持一个循环变量");
                }
                n2->a = expandNode(rangeNode->a);
                n2->b = expandNode(rangeNode->b);
                n2->c = rangeNode->c ? expandNode(rangeNode->c) : nullptr;
                n2->d = coll ? expandNode(coll) : nullptr;
                if (n->b && n->b->kind == NK::Block) n2->list = expandStmts(n->b->list);
                else if (n->b) n2->list.push_back(expandNode(n->b));
                return n2;
            }
            break;
        }
        default:
            break;
    }

    auto c = std::make_shared<Node>(*n);
    c->a = expandNode(n->a);
    c->b = expandNode(n->b);
    c->c = expandNode(n->c);
    c->d = expandNode(n->d);
    c->targets.clear();
    for (const auto& t : n->targets) c->targets.push_back(expandNode(t));
    c->values.clear();
    for (const auto& v : n->values) c->values.push_back(expandNode(v));
    c->patterns.clear();
    for (const auto& p : n->patterns) c->patterns.push_back(expandNode(p));
    c->defaults.clear();
    for (const auto& d : n->defaults) c->defaults.push_back(expandNode(d));

    if (n->kind == NK::Block || n->kind == NK::Program || n->kind == NK::FnDecl ||
        n->kind == NK::Case || n->kind == NK::CaseArm || n->kind == NK::Try) {
        c->list = expandStmts(n->list);
    } else {
        c->list.clear();
        for (const auto& k : n->list) c->list.push_back(expandNode(k));
    }
    return c;
}

std::vector<NodePtr> MacroExpander::expandStmts(const std::vector<NodePtr>& list) {
    std::vector<NodePtr> out;
    for (const auto& st : list) {
        if (!st) continue;
        if (st->kind == NK::MacroDecl || st->kind == NK::Define) continue;
        if (st->kind == NK::When || st->kind == NK::Each) {
            // A splice survived outside a template; expand it with empty bindings
            // is impossible, so this is a user error.
            bag_.error(st->pos, "MAC009",
                       std::string(st->kind == NK::When ? "@when" : "@each") +
                           " 只能出现在宏定义体内");
            continue;
        }
        if (st->kind == NK::ExprStmt && st->a && st->a->kind == NK::MacroApply) {
            auto produced = expandMacroStatements(st->a, 0);
            if (produced.size() == 1 && produced[0]->kind == NK::ExprStmt) {
                out.push_back(produced[0]);
            } else {
                for (const auto& p : produced) out.push_back(p);
            }
            continue;
        }
        NodePtr e = expandNode(st);
        if (!e) continue;
        if (e->kind == NK::Block && e->flag2) {
            for (const auto& s : e->list) out.push_back(s);
        } else {
            out.push_back(e);
        }
    }
    return out;
}

std::vector<NodePtr> MacroExpander::expandMacroStatements(const NodePtr& call, int depth) {
    std::vector<NodePtr> out;
    if (depth > kMaxMacroDepth) {
        bag_.error(call->pos, "MAC012", "宏展开层数过深（可能存在递归宏）");
        return out;
    }
    const MacroDef* def = lookupMacro(call);
    if (!def) return out;

    Bindings binds;
    if (!bindArguments(*def, call, binds)) return out;

    InstCtx ctx{this, &binds, depth, &bag_};
    std::vector<NodePtr> raw;
    instStmts(def->body, ctx, raw);

    std::function<bool(const NodePtr&)> hasWhen = [&](const NodePtr& n) -> bool {
        if (!n) return false;
        if (n->kind == NK::When) return true;
        for (const auto& k : {n->a, n->b, n->c, n->d})
            if (hasWhen(k)) return true;
        for (const auto& k : n->list)
            if (hasWhen(k)) return true;
        return false;
    };
    bool whenDriven = false;
    for (const auto& s : def->body)
        if (hasWhen(s)) whenDriven = true;
    if (raw.empty() && whenDriven) {
        bag_.error(call->pos, "MAC020",
                   "宏 `@" + def->name + "` 的 @when 分支一个都没有匹配：无法静态确定参数类型");
    }

    // Recursively expand what the template produced.
    if (raw.size() == 1) {
        auto e = expandNode(raw[0]);
        if (e) {
            if (e->kind == NK::Block && e->flag2) {
                for (const auto& s : e->list) out.push_back(s);
            } else {
                out.push_back(e);
            }
        }
        return out;
    }
    out = expandStmts(raw);
    return out;
}

NodePtr MacroExpander::expandMacroApply(const NodePtr& call, int depth) {
    if (depth > kMaxMacroDepth) {
        bag_.error(call->pos, "MAC012", "宏展开层数过深（可能存在递归宏）");
        return makeNil(call->pos);
    }
    // Expression-position expansion of @range / @when / @each helpers.
    if (call->text == "range") {
        bag_.error(call->pos, "MAC010", "`@range` 只能用在 `for ... of` 的遍历位置");
        return makeNil(call->pos);
    }
    auto stmts = expandMacroStatements(call, depth);
    if (std::getenv("SHYA_DEBUG_WHEN")) {
        std::fprintf(stderr, "[macro] @%s -> %zu stmt(s)", call->text.c_str(), stmts.size());
        for (const auto& s : stmts) std::fprintf(stderr, " %s", nodeKindName(s->kind));
        std::fprintf(stderr, "\n");
    }
    if (stmts.empty()) return makeNil(call->pos);
    if (stmts.size() == 1 && stmts[0]->kind == NK::ExprStmt) return stmts[0]->a;
    // Collapse a run of expression statements into a concatenation so that
    // chain macros (`@safe`, `@share`) can also be used in expression position.
    std::vector<NodePtr> parts;
    bool allExpr = true;
    for (const auto& s : stmts) {
        if (s->kind == NK::ExprStmt) parts.push_back(s->a);
        else if (s->kind == NK::Empty) continue;
        else {
            allExpr = false;
            break;
        }
    }
    if (allExpr) {
        // Multiple statements without a receiver: wrap them in an arrow-like
        // sequence is impossible, so require statement position.
        if (parts.size() == 1) return parts[0];
        bag_.error(call->pos, "MAC013",
                   "宏 `@" + call->text + "` 展开为多条语句，只能用作语句");
        return makeNil(call->pos);
    }
    bag_.error(call->pos, "MAC013",
               "宏 `@" + call->text + "` 展开为多条语句，只能用作语句");
    return makeNil(call->pos);
}

const MacroDef* MacroExpander::lookupMacro(const NodePtr& call) const {
    auto it = macros_.find(call->text);
    if (it == macros_.end()) {
        bag_.error(call->pos, "MAC014", "未定义的宏 `@" + call->text + "`");
        return nullptr;
    }
    return &it->second;
}

bool MacroExpander::bindArguments(const MacroDef& def, const NodePtr& call, Bindings& binds) {
    std::vector<NodePtr> positional;
    if (call->a) positional.push_back(call->a);  // postfix target
    std::unordered_map<std::string, NodePtr> named;
    for (const auto& arg : call->list) {
        if (arg->kind == NK::Prop) {
            named[arg->text] = arg->a;
        } else {
            positional.push_back(arg);
        }
    }

    // Named arguments win; positional arguments (the postfix target first) then
    // fill the parameters that no named slot claimed, in declaration order.
    std::vector<bool> bound(def.params.size(), false);
    bool ok = true;
    for (std::size_t i = 0; i < def.params.size(); ++i) {
        auto nit = named.find(def.params[i]);
        if (nit == named.end()) continue;
        Binding b;
        b.node = nit->second;
        b.slot = def.paramTypes[i] == SlotType::Unknown ? SlotType::Stmt : def.paramTypes[i];
        binds[def.params[i]] = b;
        bound[i] = true;
    }

    std::size_t pi = 0;
    for (std::size_t i = 0; i < def.params.size(); ++i) {
        if (bound[i]) continue;
        Binding b;
        b.slot = def.paramTypes[i];
        if (def.variadic[i]) {
            auto list = mk(NK::ArrayLit, call->pos);
            list->flag = true;  // variadic slot list
            while (pi < positional.size()) list->list.push_back(positional[pi++]);
            b.node = list;
            b.slot = SlotType::ExprList;
            binds[def.params[i]] = b;
            bound[i] = true;
            continue;
        }
        if (pi < positional.size()) {
            NodePtr arg = positional[pi++];
            if (isNilNode(arg)) {
                b.nil = true;
                b.node = makeNil(call->pos);
            } else {
                SlotType got = inferSlotType(arg);
                if (def.paramTypes[i] == SlotType::Type) got = SlotType::Type;
                b.node = arg;
                b.slot = got;
                if (def.paramTypes[i] == SlotType::Stmt && got != SlotType::Stmt) {
                    // An expression may stand in for a statement slot; wrap it.
                    auto st = mk(NK::ExprStmt, arg->pos);
                    st->a = arg;
                    b.node = st;
                    b.slot = SlotType::Stmt;
                } else if (!def.paramKinds[i].empty()) {
                    // The parameter names a concrete AST node kind.
                    bool kindOk = false;
                    for (NK k : def.paramKinds[i]) {
                        if (arg->kind == k) kindOk = true;
                    }
                    if (!kindOk && def.paramTypes[i] == SlotType::SafeCallExpr &&
                        arg->kind == NK::Call && arg->flag)
                        kindOk = true;
                    if (!kindOk) {
                        bag_.error(arg->pos, "MAC015",
                                   "宏 `@" + def.name + "` 的参数 `#" + def.params[i] +
                                       "` 需要 AST 节点 `" + def.paramTypeNames[i] +
                                       "`，但传入的是 `" + nodeKindName(arg->kind) + "`");
                        ok = false;
                    }
                } else if (!typeMatches(def.paramTypes[i], got)) {
                    bag_.error(arg->pos, "MAC015",
                               "宏 `@" + def.name + "` 的参数 `#" + def.params[i] +
                                   "` 需要 " + slotTypeName(def.paramTypes[i]) + "，但传入的是 " +
                                   slotTypeName(got));
                    ok = false;
                }
            }
            binds[def.params[i]] = b;
            bound[i] = true;
            continue;
        }
        // Missing argument: stmt slots may be omitted (renders as nothing).
        if (def.paramTypes[i] == SlotType::Stmt) {
            b.nil = true;
            b.node = makeNil(call->pos);
            binds[def.params[i]] = b;
            bound[i] = true;
            continue;
        }
        bag_.error(call->pos, "MAC016",
                   "宏 `@" + def.name + "` 缺少参数 `#" + def.params[i] + "`");
        ok = false;
    }
    if (pi < positional.size()) {
        std::string extra = positional[pi] && positional[pi]->kind == NK::Ident
                                ? "`" + positional[pi]->text + "`"
                                : std::string("该实参");
        bag_.error(pi < positional.size() && positional[pi] ? positional[pi]->pos : call->pos,
                   "MAC017",
                   "宏 `@" + def.name + "` 收到过多位置参数：多余 " +
                       std::to_string(positional.size() - pi) + " 个（" + extra +
                       "）——所有参数都已被具名插槽占满，请减少具名插槽或去掉多余的位置参数");
        ok = false;
    }
    for (const auto& kv : named) {
        bool known = false;
        for (const auto& p : def.params)
            if (p == kv.first) known = true;
        if (!known) {
            bag_.error(call->pos, "MAC018",
                       "宏 `@" + def.name + "` 没有名为 `#" + kv.first + "` 的参数");
            ok = false;
        }
    }
    return ok;
}

}  // namespace shya
