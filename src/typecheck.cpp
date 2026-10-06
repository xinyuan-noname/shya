// shya - basic type system and the (progressive) type checker.
#include "shya.h"

namespace shya {

// ============================================================ type objects ==

std::string Type::str() const {
    switch (kind) {
        case TK::Any: return "any";
        case TK::Unknown: return "unknown";
        case TK::Never: return "never";
        case TK::Void: return "void";
        case TK::Null: return "void";
        case TK::Bool: return "boolean";
        case TK::Num: return "number";
        case TK::Str: return "string";
        case TK::Object: return "object";
        case TK::Array:
            return "array<" + (elem ? elem->str() : std::string("unknown")) + ">";
        case TK::Set: return "set<" + (elem ? elem->str() : std::string("unknown")) + ">";
        case TK::Map:
            return "map<" + (key ? key->str() : std::string("unknown")) + "," +
                   (elem ? elem->str() : std::string("unknown")) + ">";
        case TK::Fn: {
            std::string s = "fn(";
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i) s += ",";
                s += parts[i]->str();
            }
            s += ")->" + (ret ? ret->str() : std::string("void"));
            return s;
        }
        case TK::Union: {
            std::string s;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i) s += "|";
                s += parts[i]->str();
            }
            return s;
        }
        case TK::Range: return "range<" + (elem ? elem->str() : std::string("unknown")) + ">";
        case TK::TypeParam:
        case TK::Class: return name;
        case TK::Literal: return literal;
    }
    return "unknown";
}

static TypePtr makeType(TK k) {
    auto t = std::make_shared<Type>();
    t->kind = k;
    return t;
}

TypePtr tAny() {
    static TypePtr v = makeType(TK::Any);
    return v;
}
TypePtr tUnknown() {
    static TypePtr v = makeType(TK::Unknown);
    return v;
}
TypePtr tVoid() {
    static TypePtr v = makeType(TK::Void);
    return v;
}
TypePtr tBool() {
    static TypePtr v = makeType(TK::Bool);
    return v;
}
TypePtr tNum() {
    static TypePtr v = makeType(TK::Num);
    return v;
}
TypePtr tStr() {
    static TypePtr v = makeType(TK::Str);
    return v;
}
TypePtr tObject() {
    static TypePtr v = makeType(TK::Object);
    return v;
}
TypePtr tArray(TypePtr e) {
    auto t = makeType(TK::Array);
    t->elem = std::move(e);
    return t;
}
TypePtr tSet(TypePtr e) {
    auto t = makeType(TK::Set);
    t->elem = std::move(e);
    return t;
}
TypePtr tMap(TypePtr k, TypePtr v) {
    auto t = makeType(TK::Map);
    t->key = std::move(k);
    t->elem = std::move(v);
    return t;
}
TypePtr tFn(std::vector<TypePtr> ps, TypePtr r) {
    auto t = makeType(TK::Fn);
    t->parts = std::move(ps);
    t->ret = std::move(r);
    return t;
}
TypePtr tUnion(std::vector<TypePtr> parts) {
    std::vector<TypePtr> flat;
    for (auto& p : parts) {
        if (!p) continue;
        if (p->kind == TK::Union) {
            for (auto& q : p->parts) flat.push_back(q);
        } else {
            flat.push_back(p);
        }
    }
    // dedupe
    std::vector<TypePtr> uniq;
    for (auto& p : flat) {
        bool seen = false;
        for (auto& q : uniq)
            if (typeEquals(p, q)) seen = true;
        if (!seen) uniq.push_back(p);
    }
    if (uniq.size() == 1) return uniq[0];
    auto t = makeType(TK::Union);
    t->parts = std::move(uniq);
    return t;
}
TypePtr tNamed(const std::string& n) {
    auto t = makeType(TK::Class);
    t->name = n;
    return t;
}
TypePtr tRange() {
    static TypePtr v = makeType(TK::Range);
    return v;
}
TypePtr tRangeOf(const TypePtr& elem) {
    auto t = makeType(TK::Range);
    t->elem = elem;
    return t;
}

bool typeEquals(const TypePtr& a, const TypePtr& b) {
    if (!a || !b) return a == b;
    if (a.get() == b.get()) return true;
    if (a->kind != b->kind) {
        if ((a->kind == TK::Any || a->kind == TK::Unknown) ||
            (b->kind == TK::Any || b->kind == TK::Unknown))
            return false;
        return false;
    }
    switch (a->kind) {
        case TK::Array:
        case TK::Set:
            return typeEquals(a->elem, b->elem);
        case TK::Map:
            return typeEquals(a->key, b->key) && typeEquals(a->elem, b->elem);
        case TK::Fn: {
            if (a->parts.size() != b->parts.size()) return false;
            for (std::size_t i = 0; i < a->parts.size(); ++i)
                if (!typeEquals(a->parts[i], b->parts[i])) return false;
            return typeEquals(a->ret, b->ret);
        }
        case TK::Union: {
            if (a->parts.size() != b->parts.size()) return false;
            for (auto& p : a->parts) {
                bool found = false;
                for (auto& q : b->parts)
                    if (typeEquals(p, q)) found = true;
                if (!found) return false;
            }
            return true;
        }
        case TK::Class:
        case TK::TypeParam:
            return a->name == b->name;
        default:
            return true;
    }
}

bool isAssignable(const TypePtr& to, const TypePtr& from) {
    if (!to || !from) return true;
    if (to->kind == TK::Any || from->kind == TK::Any) return true;
    if (to->kind == TK::Unknown || from->kind == TK::Unknown) return true;
    if (to->kind == TK::Never) return true;
    if (typeEquals(to, from)) return true;
    if (to->kind == TK::Union) {
        for (auto& p : to->parts)
            if (isAssignable(p, from)) return true;
        return false;
    }
    if (from->kind == TK::Union) {
        for (auto& p : from->parts)
            if (!isAssignable(to, p)) return false;
        return true;
    }
    if (to->kind == TK::Array && from->kind == TK::Array) return isAssignable(to->elem, from->elem);
    if (to->kind == TK::Set && from->kind == TK::Set) return isAssignable(to->elem, from->elem);
    if (to->kind == TK::Map && from->kind == TK::Map)
        return isAssignable(to->key, from->key) && isAssignable(to->elem, from->elem);
    if (to->kind == TK::Class && from->kind == TK::Class) return false;
    if (to->kind == TK::Object && (from->kind == TK::Class)) return true;
    return false;
}

// ==================================================== annotation parsing ====

namespace {

struct TypeParser {
    const std::string& s;
    std::size_t i = 0;
    bool bad = false;

    explicit TypeParser(const std::string& str) : s(str) {}

    void skip() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    }
    bool at(char c) {
        skip();
        return i < s.size() && s[i] == c;
    }
    bool eat(char c) {
        if (at(c)) {
            ++i;
            return true;
        }
        return false;
    }
    std::string ident() {
        skip();
        std::string r;
        while (i < s.size()) {
            char c = s[i];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
                static_cast<unsigned char>(c) >= 0x80) {
                r.push_back(c);
                ++i;
            } else {
                break;
            }
        }
        return r;
    }

    TypePtr parseUnion() {
        TypePtr first = parsePostfix();
        std::vector<TypePtr> parts{first};
        while (eat('|')) {
            eat('|');  // in case of '||'
            parts.push_back(parsePostfix());
        }
        if (parts.size() == 1) return first;
        return tUnion(parts);
    }

    TypePtr parsePostfix() {
        TypePtr base = parsePrimary();
        for (;;) {
            if (at('[')) {
                eat('[');
                eat(']');
                base = tArray(base);
                continue;
            }
            if (at('?')) {
                eat('?');
                if (base) base->optional = true;
                continue;
            }
            break;
        }
        return base;
    }

    TypePtr parsePrimary() {
        skip();
        if (i >= s.size()) return tUnknown();
        char c = s[i];
        if (c == '(') {
            eat('(');
            TypePtr t = parseUnion();
            eat(')');
            return t;
        }
        if (c == '"' || c == '\'') {
            std::string lit;
            ++i;
            while (i < s.size() && s[i] != c) lit.push_back(s[i++]);
            eat(c);
            auto t = makeType(TK::Literal);
            t->literal = "\"" + lit + "\"";
            t->name = lit;
            return t;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::string lit;
            while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.'))
                lit.push_back(s[i++]);
            auto t = makeType(TK::Literal);
            t->literal = lit;
            return t;
        }
        std::string name = ident();
        if (name.empty()) {
            bad = true;
            ++i;
            return tUnknown();
        }
        std::vector<TypePtr> args;
        if (at('<')) {
            eat('<');
            if (!at('>')) {
                for (;;) {
                    args.push_back(parseUnion());
                    if (eat(',')) continue;
                    break;
                }
            }
            eat('>');
        }
        if (name == "number" || name == "int" || name == "float" || name == "double")
            return tNum();
        if (name == "string") return tStr();
        if (name == "boolean" || name == "bool") return tBool();
        if (name == "void" || name == "null" || name == "undefined" || name == "nil")
            return tVoid();
        if (name == "any") return tAny();
        if (name == "unknown") return tUnknown();
        if (name == "never") return makeType(TK::Never);
        if (name == "object" || name == "obj") return tObject();
        if (name == "array" || name == "list")
            return tArray(args.empty() ? tUnknown() : args[0]);
        if (name == "set") return tSet(args.empty() ? tUnknown() : args[0]);
        if (name == "map" || name == "record")
            return tMap(args.size() > 0 ? args[0] : tUnknown(),
                        args.size() > 1 ? args[1] : tUnknown());
        if (name == "fn" || name == "function") return tFn({}, tUnknown());
        if (name == "rangeExpr" || name == "range") return tRange();
        if (name == "stmt" || name == "expr" || name == "type" || name == "callExpr" ||
            name == "safeCallExpr" || name == "expr[]")
            return tNamed(name);
        if (!args.empty()) {
            auto t = makeType(TK::Class);
            t->name = name;
            t->parts = args;
            return t;
        }
        return tNamed(name);
    }
};

}  // namespace

TypePtr parseTypeString(const std::string& ann) {
    if (ann.empty()) return tUnknown();
    TypeParser p(ann);
    TypePtr t = p.parseUnion();
    return t ? t : tUnknown();
}

// ========================================================== type checker ====

struct TypeChecker::Scope {
    std::unordered_map<std::string, TypePtr> vars;
    std::unordered_set<std::string> consts;
};

namespace {
const std::unordered_set<std::string>& builtinGlobals() {    static const std::unordered_set<std::string> g = {
        "Math",     "Object",   "Array",   "JSON",     "console",  "String",  "Number",
        "Boolean",  "Map",      "Set",     "WeakMap",  "WeakSet",  "Promise", "Date",
        "RegExp",   "Error",    "TypeError", "Symbol", "BigInt",   "globalThis",
        "undefined", "NaN",     "Infinity", "parseInt", "parseFloat", "isNaN",
        "structuredClone", "queueMicrotask", "setTimeout", "clearTimeout",
        "setInterval", "clearInterval", "fetch", "this", "arguments",
    };
    return g;
}

// Type names accepted on the right hand side of `is`.
bool knownTypeName(const std::string& n) {
    static const std::unordered_set<std::string> t = {
        "array", "list", "string", "number", "int", "float", "boolean", "bool",
        "object", "obj", "map", "set", "fn", "function", "void", "null",
        "undefined", "any", "unknown", "never", "range", "rangeExpr", "expr",
        "stmt", "type", "callExpr", "safeCallExpr",
    };
    return t.count(n) > 0;
}
}  // namespace

TypeChecker::TypeChecker(DiagBag& bag) : bag_(bag) {}

void TypeChecker::pushScope() { scopes_.push_back(std::make_shared<Scope>()); }
void TypeChecker::popScope() {
    if (!scopes_.empty()) scopes_.pop_back();
}

void TypeChecker::declare(const std::string& name, const TypePtr& t, const Pos& p) {
    if (scopes_.empty()) pushScope();
    auto& s = *scopes_.back();
    if (s.vars.count(name)) {
        bag_.warning(p, "TC001", "变量 `" + name + "` 在同一作用域内重复声明，后一次声明覆盖前一次");
    }
    s.vars[name] = t;
}

TypePtr TypeChecker::lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = (*it)->vars.find(name);
        if (f != (*it)->vars.end()) return f->second;
    }
    return nullptr;
}

bool TypeChecker::lookupConst(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        if ((*it)->vars.count(name)) return (*it)->consts.count(name) > 0;
    }
    return false;
}

void TypeChecker::markConst(const std::string& name) {
    if (scopes_.empty()) return;
    scopes_.back()->consts.insert(name);
}

TypePtr TypeChecker::resolveAnnotation(const std::string& ann, const Pos& p) {
    if (ann.empty()) return tUnknown();
    TypePtr t = parseTypeString(ann);
    if (!t) {
        bag_.error(p, "TC002", "无法解析的类型标注 `" + ann + "`");
        return tUnknown();
    }
    return t;
}

bool TypeChecker::checkAssignable(const TypePtr& to, const TypePtr& from, const Pos& p,
                                  const std::string& what) {
    if (!to || !from) return true;
    if (isAssignable(to, from)) return true;
    bag_.error(p, "TC003", what + "：期望 " + to->str() + "，实际是 " + from->str());
    return false;
}

void TypeChecker::hoistFunctions(const std::vector<NodePtr>& stmts) {
    for (const auto& st : stmts) {
        if (!st) continue;
        if (st->kind == NK::FnDecl) {
            std::vector<TypePtr> params;
            int required = 0;
            bool variadic = false;
            for (std::size_t i = 0; i < st->typeAnns.size(); ++i) {
                std::string ann = st->typeAnns[i];
                if (ann.rfind("...", 0) == 0) {
                    variadic = true;
                    ann = ann.substr(3);
                }
                params.push_back(ann.empty() ? tUnknown() : parseTypeString(ann));
                bool hasDefault = i < st->defaults.size() && st->defaults[i] != nullptr;
                if (!variadic && !hasDefault) ++required;
            }
            TypePtr ret = st->typeAnn2.empty() ? tUnknown() : parseTypeString(st->typeAnn2);
            TypePtr ft = tFn(params, ret);
            ft->required = required;
            ft->variadic = variadic;
            declare(st->text, ft, st->pos);
        }
        if (st->kind == NK::Export && st->a) hoistFunctions({st->a});
    }
}

namespace {
TypePtr makeDeclaredFn(const std::vector<std::string>& typeAnns,
                       const std::vector<NodePtr>& defaults, const std::string& retAnn) {
    std::vector<TypePtr> params;
    int required = 0;
    bool variadic = false;
    for (std::size_t i = 0; i < typeAnns.size(); ++i) {
        std::string ann = typeAnns[i];
        if (ann.rfind("...", 0) == 0) {
            variadic = true;
            ann = ann.substr(3);
        }
        params.push_back(ann.empty() ? tUnknown() : parseTypeString(ann));
        bool hasDefault = i < defaults.size() && defaults[i] != nullptr;
        if (!variadic && !hasDefault) ++required;
    }
    TypePtr ft = tFn(params, retAnn.empty() ? tVoid() : parseTypeString(retAnn));
    ft->required = required;
    ft->variadic = variadic;
    return ft;
}
}  // namespace

TypePtr buildDeclaredType(const NodePtr& n) {
    if (!n || n->kind != NK::Declare) return nullptr;
    if (n->flag) return makeDeclaredFn(n->typeAnns, n->defaults, n->typeAnn2);
    auto t = tNamed(n->text);
    for (const auto& m : n->list) {
        if (!m || m->text.empty()) continue;
        if (m->flag) t->methods[m->text] = makeDeclaredFn(m->typeAnns, m->defaults, m->typeAnn);
        else t->fields[m->text] = m->typeAnn.empty() ? tUnknown() : parseTypeString(m->typeAnn);
    }
    return t;
}

void TypeChecker::collectDeclares(const NodePtr& n) {
    if (!n) return;
    if (n->kind == NK::Declare) {
        TypePtr t = buildDeclaredType(n);
        if (!t) return;
        if (n->flag) {
            declaredTypes_[n->text] = t;
            declare(n->text, t, n->pos);
            return;
        }
        // Merge into an existing declaration of the same name, if any.
        auto it = declaredTypes_.find(n->text);
        if (it != declaredTypes_.end() && it->second->kind == TK::Class) {
            for (const auto& kv : t->fields) it->second->fields[kv.first] = kv.second;
            for (const auto& kv : t->methods) it->second->methods[kv.first] = kv.second;
            return;
        }
        declaredTypes_[n->text] = t;
        return;
    }
    for (const auto& k : {n->a, n->b, n->c, n->d}) collectDeclares(k);
    for (const auto& k : n->list) collectDeclares(k);
    for (const auto& k : n->values) collectDeclares(k);
    for (const auto& k : n->defaults) collectDeclares(k);
}

// Looks up a member on a declared host type. Returns nullptr when unknown.
TypePtr TypeChecker::memberType(const TypePtr& obj, const std::string& member) const {
    if (!obj || obj->kind != TK::Class) return nullptr;
    auto it = declaredTypes_.find(obj->name);
    if (it == declaredTypes_.end()) return nullptr;
    const TypePtr& t = it->second;
    auto f = t->fields.find(member);
    if (f != t->fields.end()) return f->second;
    auto m = t->methods.find(member);
    if (m != t->methods.end()) return m->second;
    return nullptr;
}

void TypeChecker::check(const NodePtr& program) {
    pushScope();
    collectDeclares(program);
    checkStatements(program->list);
    popScope();
    if (bag_.hasError()) return;
}

TypePtr TypeChecker::checkStatements(const std::vector<NodePtr>& stmts) {
    hoistFunctions(stmts);
    TypePtr last = tVoid();
    for (const auto& st : stmts) last = checkNode(st);
    return last;
}

TypePtr TypeChecker::checkNode(const NodePtr& n) {
    if (!n) return tVoid();
    TypePtr result = tVoid();
    switch (n->kind) {
        case NK::Program:
        case NK::Block: {
            pushScope();
            result = checkStatements(n->list);
            popScope();
            return result;
        }
        case NK::Empty:
            return tVoid();
        case NK::Num:
        case NK::MathConst: {
            result = tNum();
            break;
        }
        case NK::Str:
        case NK::Tpl:
            result = tStr();
            break;
        case NK::Bool:
            result = tBool();
            break;
        case NK::Void:
            result = tVoid();
            break;
        case NK::Ident: {
            if (n->flag && n->text == "_") {
                result = tVoid();
                break;
            }
            if (n->flag2) {
                // A chain fragment (`getHp` in `x @safe getHp`): member name.
                result = tUnknown();
                break;
            }
            TypePtr t = lookup(n->text);
            if (!t) {
                if (!builtinGlobals().count(n->text)) {
                    bag_.warning(n->pos, "TC010", "使用了未声明的标识符 `" + n->text + "`");
                }
                result = tUnknown();
            } else {
                result = t;
            }
            break;
        }
        case NK::ArrayLit: {
            if (n->flag) {
                TypePtr last = tVoid();
                for (const auto& e : n->list) last = checkNode(e);
                result = last;
                break;
            }
            TypePtr elem = tUnknown();
            if (!n->list.empty()) elem = checkNode(n->list[0]);
            for (std::size_t i = 1; i < n->list.size(); ++i) checkNode(n->list[i]);
            result = tArray(elem);
            break;
        }
        case NK::ObjectLit: {
            for (const auto& p : n->list) {
                if (p->a) checkNode(p->a);
            }
            result = tObject();
            break;
        }
        case NK::Prop: {
            result = n->a ? checkNode(n->a) : tUnknown();
            break;
        }
        case NK::Unary: {
            TypePtr a = checkNode(n->a);
            if (n->text == "!" || n->text == "typeof") result = n->text == "!" ? tBool() : tStr();
            else if (n->text == "-" || n->text == "+") {
                checkAssignable(tNum(), a, n->pos, "一元运算符 `" + n->text + "` 的操作数");
                result = tNum();
            } else {
                result = a;
            }
            break;
        }
        case NK::Binary: {
            if (n->text == "concat") {
                TypePtr l = checkNode(n->a);
                checkNode(n->b);
                result = l;
                break;
            }
            TypePtr l = checkNode(n->a);
            TypePtr r = checkNode(n->b);
            if (n->text == "&&" || n->text == "||") {
                result = tUnion({l, r});
            } else if (n->text == "+") {
                if (l->kind == TK::Str || r->kind == TK::Str) {
                    if (l->kind != TK::Str && l->kind != TK::Unknown && l->kind != TK::Any)
                        bag_.warning(n->pos, "TC004",
                                     "`+` 的一侧是字符串，另一侧是 " + l->str() +
                                         "，会得到字符串拼接");
                    result = tStr();
                } else {
                    checkAssignable(tNum(), l, n->pos, "`+` 的左操作数");
                    checkAssignable(tNum(), r, n->pos, "`+` 的右操作数");
                    result = tNum();
                }
            } else {
                checkAssignable(tNum(), l, n->pos, "运算符 `" + n->text + "` 的左操作数");
                checkAssignable(tNum(), r, n->pos, "运算符 `" + n->text + "` 的右操作数");
                result = tNum();
            }
            break;
        }
        case NK::Compare: {
            TypePtr l = checkNode(n->a);
            for (std::size_t i = 0; i < n->list.size(); ++i) {
                std::string op = i < n->names.size() ? n->names[i] : n->text;
                if (op == "is" || op == "is not") {
                    // The right hand side names a type, not a value.
                    const NodePtr& r = n->list[i];
                    if (!r || r->kind != NK::Ident) {
                        bag_.error(r ? r->pos : n->pos, "TC011",
                                   "`is` 的右侧必须是类型名，例如 `array`、`string`、`Player`");
                    } else if (!knownTypeName(r->text)) {
                        bag_.warning(r->pos, "TC012",
                                     "`is` 右侧的 `" + r->text +
                                         "` 不是内建类型名，将按宿主类型 `instanceof` 处理");
                    }
                    exprTypes_[r.get()] = tUnknown();
                    continue;
                }
                TypePtr rt = checkNode(n->list[i]);
                if (l && l->kind != TK::Unknown && rt->kind != TK::Unknown &&
                    l->kind != TK::Any && rt->kind != TK::Any && !isAssignable(l, rt) &&
                    !isAssignable(rt, l)) {
                    bag_.warning(n->pos, "TC005",
                                 "比较运算符两侧类型不同：" + l->str() + " 与 " + rt->str());
                }
            }
            result = tBool();
            break;
        }
        case NK::Ternary: {
            checkNode(n->a);
            TypePtr b = checkNode(n->b);
            TypePtr c = checkNode(n->c);
            result = typeEquals(b, c) ? b : tUnion({b, c});
            break;
        }
        case NK::Call: {
            // A chain fragment (`recover(2)` inside a macro argument) names a
            // member of its receiver, so its callee is not a free identifier.
            TypePtr callee = tUnknown();
            if (n->flag2 && n->a && n->a->kind == NK::Ident) {
                callee = tUnknown();
            } else {
                callee = checkNode(n->a);
            }
            std::vector<TypePtr> args;
            for (const auto& a : n->list) args.push_back(checkNode(a));
            if (callee && callee->kind == TK::Fn) {
                int minArgs = callee->required < 0 ? static_cast<int>(callee->parts.size())
                                                   : callee->required;
                int maxArgs = callee->variadic ? -1 : static_cast<int>(callee->parts.size());
                int have = static_cast<int>(args.size());
                if (have < minArgs || (maxArgs >= 0 && have > maxArgs)) {
                    bag_.error(n->pos, "TC006",
                               "调用参数个数不匹配：期望 " +
                                   (maxArgs < 0 ? std::to_string(minArgs) + " 个以上"
                                                : (minArgs == maxArgs
                                                       ? std::to_string(minArgs) + " 个"
                                                       : std::to_string(minArgs) + "~" +
                                                             std::to_string(maxArgs) + " 个")) +
                                   "，实际 " + std::to_string(have) + " 个");
                }
                std::size_t common = std::min(args.size(), callee->parts.size());
                for (std::size_t i = 0; i < common; ++i) {
                    TypePtr expected = callee->parts[i];
                    // For the rest parameter each argument has the element type.
                    if (callee->variadic && i + 1 == callee->parts.size() && expected &&
                        expected->kind == TK::Array && expected->elem)
                        expected = expected->elem;
                    checkAssignable(expected, args[i], n->list[i]->pos,
                                    "第 " + std::to_string(i + 1) + " 个实参");
                }
                result = callee->ret ? callee->ret : tUnknown();
            } else {
                result = tUnknown();
            }
            break;
        }
        case NK::Member: {
            TypePtr obj = checkNode(n->a);
            if (n->b) {
                // `n->b` is a chain-link fragment such as `say("hello")`; its
                // callee names a member of the receiver, not a free variable.
                if (n->b->kind == NK::Call || n->b->kind == NK::Member) {
                    for (const auto& a : n->b->list) checkNode(a);
                    if (n->b->kind == NK::Member && n->b->b) checkNode(n->b->b);
                } else {
                    checkNode(n->b);
                }
            }
            std::vector<TypePtr> args;
            for (const auto& a : n->list) args.push_back(checkNode(a));

            // A member of a `declare`d host type carries a known type.
            result = tUnknown();
            if (!n->b && !n->text.empty() && obj && obj->kind == TK::Class) {
                TypePtr mt = memberType(obj, n->text);
                if (!mt && declaredTypes_.count(obj->name)) {
                    bag_.warning(n->pos, "TC014",
                                 "类型 `" + obj->name + "` 没有声明成员 `" + n->text +
                                     "`（可用 `declare " + obj->name + " { " + n->text +
                                     ": … }` 补充声明）");
                }
                if (mt) {
                    if (mt->kind == TK::Fn) {                        int minArgs = mt->required < 0 ? static_cast<int>(mt->parts.size())
                                                       : mt->required;
                        int maxArgs = mt->variadic ? -1 : static_cast<int>(mt->parts.size());
                        int have = static_cast<int>(args.size());
                        if (have < minArgs || (maxArgs >= 0 && have > maxArgs)) {
                            bag_.error(n->pos, "TC006",
                                       "调用 `" + n->text + "` 的参数个数不匹配：期望 " +
                                           (maxArgs < 0
                                                ? std::to_string(minArgs) + " 个以上"
                                                : (minArgs == maxArgs
                                                       ? std::to_string(minArgs) + " 个"
                                                       : std::to_string(minArgs) + "~" +
                                                             std::to_string(maxArgs) + " 个")) +
                                           "，实际 " + std::to_string(have) + " 个");
                        }
                        std::size_t common = std::min(args.size(), mt->parts.size());
                        for (std::size_t i = 0; i < common; ++i) {
                            TypePtr expected = mt->parts[i];
                            if (mt->variadic && i + 1 == mt->parts.size() && expected &&
                                expected->kind == TK::Array && expected->elem)
                                expected = expected->elem;
                            checkAssignable(expected, args[i], n->list[i]->pos,
                                            "方法 `" + n->text + "` 的第 " +
                                                std::to_string(i + 1) + " 个实参");
                        }
                        result = mt->ret ? mt->ret : tUnknown();
                    } else {
                        // A declared field is a real property, not a call.
                        fieldAccesses_.insert(n.get());
                        result = mt;
                    }
                }
            }
            (void)obj;
            break;
        }
        case NK::Index: {
            TypePtr o = checkNode(n->a);
            TypePtr idx = checkNode(n->b);
            if (o && o->kind == TK::Array) {
                checkAssignable(tNum(), idx, n->b->pos, "数组下标");
                result = o->elem ? o->elem : tUnknown();
            } else if (o && o->kind == TK::Map) {
                result = o->elem ? o->elem : tUnknown();
            } else if (o && o->kind == TK::Str) {
                result = tStr();
            } else if (o && o->kind == TK::Range) {
                result = o->elem ? o->elem : tNum();
            } else {
                result = tUnknown();
            }
            break;
        }
        case NK::Spread:
        case NK::Await: {
            result = checkNode(n->a);
            break;
        }
        case NK::Decl: {
            if (!n->list.empty()) {
                // Multi declaration container.
                for (const auto& d : n->list) checkNode(d);
                result = tVoid();
                break;
            }
            if (n->names.empty()) {
                result = tVoid();
                break;
            }
            const std::string& name = n->names[0];
            TypePtr ann = n->typeAnn.empty() ? nullptr : resolveAnnotation(n->typeAnn, n->pos);
            TypePtr init = n->a ? checkNode(n->a) : tUnknown();
            TypePtr finalType = ann ? ann : init;
            if (ann) checkAssignable(ann, init, n->pos, "变量 `" + name + "` 的初始值");
            if (n->text == "const" && !n->a) {
                bag_.error(n->pos, "TC007", "`const " + name + "` 必须有初始值");
            }
            declare(name, finalType, n->pos);
            if (n->text == "const") markConst(name);
            result = finalType;
            break;
        }
        case NK::Define: {
            // `define` is inlined by the macro phase; anything left is an error.
            bag_.error(n->pos, "TC008", "内部的 `define` 节点没有被消除（编译器错误）");
            result = tUnknown();
            break;
        }
        case NK::Declare:
            // Host declarations carry no runtime behaviour and are collected in
            // a pre-pass; nothing to check inside them.
            result = tVoid();
            break;
        case NK::Assign: {
            std::vector<TypePtr> valueTypes;
            for (const auto& v : n->values) valueTypes.push_back(checkNode(v));
            if (n->targets.size() == n->values.size()) {
                for (std::size_t i = 0; i < n->targets.size(); ++i) {
                    const NodePtr& t = n->targets[i];
                    if (t->kind == NK::Ident) {
                        TypePtr cur = lookup(t->text);
                        if (lookupConst(t->text)) {
                            bag_.error(t->pos, "TC013",
                                       "`" + t->text + "` 是 const 常量，不能被重新赋值");
                        }
                        if (cur) checkAssignable(cur, valueTypes[i], t->pos,
                                                 "对 `" + t->text + "` 的赋值");
                        else declare(t->text, valueTypes[i], t->pos);
                    } else {
                        checkNode(t);
                    }
                    exprTypes_[t.get()] = valueTypes[i];
                }
            } else if (n->values.size() == 1) {
                for (const auto& t : n->targets) {
                    if (t->kind == NK::Ident) {
                        TypePtr cur = lookup(t->text);
                        if (cur) checkAssignable(cur, valueTypes[0], t->pos,
                                                 "对 `" + t->text + "` 的赋值");
                        else declare(t->text, valueTypes[0], t->pos);
                    } else {
                        checkNode(t);
                    }
                }
            }
            result = valueTypes.empty() ? tVoid() : valueTypes.back();
            break;
        }
        case NK::IncDec: {
            TypePtr cur = checkNode(n->a);
            checkAssignable(tNum(), cur, n->pos, "自增/自减的操作数");
            result = cur;
            break;
        }
        case NK::If: {
            checkNode(n->a);
            checkNode(n->b);
            if (n->c) checkNode(n->c);
            result = tVoid();
            break;
        }
        case NK::Case: {
            if (n->a) checkNode(n->a);
            for (const auto& arm : n->list) checkNode(arm);
            result = tVoid();
            break;
        }
        case NK::CaseArm: {
            for (const auto& p : n->patterns) checkNode(p);
            pushScope();
            checkStatements(n->list);
            popScope();
            result = tVoid();
            break;
        }
        case NK::ForWhile: {
            checkNode(n->a);
            pushScope();
            checkNode(n->b);
            popScope();
            result = tVoid();
            break;
        }
        case NK::ForOf: {
            TypePtr iter = checkNode(n->a);
            pushScope();
            TypePtr elem = tUnknown();
            if (iter) {
                if (iter->kind == TK::Array || iter->kind == TK::Set) elem = iter->elem;
                else if (iter->kind == TK::Map) elem = tArray(iter->key);
                else if (iter->kind == TK::Str) elem = tStr();
                else if (iter->kind == TK::Range) elem = iter->elem;
            }
            if (n->names.size() >= 2) {
                declare(n->names[0], elem ? elem : tUnknown(), n->pos);
                declare(n->names[1], tUnknown(), n->pos);
            } else if (!n->names.empty()) {
                declare(n->names[0], elem ? elem : tUnknown(), n->pos);
            }
            checkNode(n->b);
            popScope();
            result = tVoid();
            break;
        }
        case NK::ForRange: {
            checkNode(n->a);
            checkNode(n->b);
            if (n->c) checkNode(n->c);
            TypePtr collType = n->d ? checkNode(n->d) : nullptr;
            pushScope();
            TypePtr elem = tNum();
            if (collType) {
                if (collType->kind == TK::Array || collType->kind == TK::Set) elem = collType->elem;
                else if (collType->kind == TK::Str) elem = tStr();
            }
            if (!n->text.empty()) declare(n->text, elem, n->pos);
            checkStatements(n->list);
            popScope();
            result = tVoid();
            break;
        }
        case NK::FnDecl: {
            pushScope();
            for (std::size_t i = 0; i < n->names.size(); ++i) {
                std::string ann = i < n->typeAnns.size() ? n->typeAnns[i] : std::string();
                if (ann.rfind("...", 0) == 0) ann = ann.substr(3);
                declare(n->names[i], ann.empty() ? tUnknown() : parseTypeString(ann), n->pos);
                if (i < n->defaults.size() && n->defaults[i]) checkNode(n->defaults[i]);
            }
            checkStatements(n->list);
            popScope();
            result = tVoid();
            break;
        }
        case NK::Return: {
            result = n->a ? checkNode(n->a) : tVoid();
            break;
        }
        case NK::Throw: {
            checkNode(n->a);
            result = tVoid();
            break;
        }
        case NK::Try: {
            if (n->a) checkNode(n->a);
            pushScope();
            if (!n->text.empty()) declare(n->text, tUnknown(), n->pos);
            if (n->b) checkNode(n->b);
            popScope();
            if (n->c) checkNode(n->c);
            result = tVoid();
            break;
        }
        case NK::Break:
        case NK::Continue:
        case NK::Import:
            result = tVoid();
            break;
        case NK::Export: {
            result = n->a ? checkNode(n->a) : tVoid();
            break;
        }
        case NK::ExprStmt: {
            result = checkNode(n->a);
            break;
        }
        case NK::TsRaw:
            // Raw JS passthrough: no static information for the payload itself,
            // but the `#slot` substitutions are real shya nodes that get emitted.
            for (const auto& a : n->list) checkNode(a);
            result = tUnknown();
            break;
        case NK::MacroApply:
        case NK::SlotRef:
        case NK::SlotList:
        case NK::When:
        case NK::WhenArm:
        case NK::Each:
        case NK::MacroDecl:
        case NK::TypeRef:
        case NK::Optionalize:
        case NK::RangeExpr:
        case NK::ForEach:
            // Should have been consumed by the macro phase.
            if (n->kind == NK::MacroApply || n->kind == NK::SlotRef || n->kind == NK::When ||
                n->kind == NK::Each || n->kind == NK::MacroDecl) {
                bag_.error(n->pos, "TC009", "宏阶段没有完全展开该节点（编译器错误）");
            }
            if (n->a) checkNode(n->a);
            result = tUnknown();
            break;
        default:
            result = tUnknown();
            break;
    }
    exprTypes_[n.get()] = result;
    return result;
}

}  // namespace shya
