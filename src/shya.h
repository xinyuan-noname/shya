#pragma once
// ============================================================================
//  shya - a small strongly-typed DSL that compiles to ES2026 JavaScript.
//
//  Pipeline:  lexer -> parser (AST) -> macro expansion / desugaring (core AST)
//             -> type checking -> ES2026 code generation
//
//  One umbrella header so every translation unit shares a single AST type.
// ============================================================================

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace shya {

// Forward declaration so the macro layer can talk about types.
struct Type;
using TypePtr = std::shared_ptr<Type>;

// ============================================================== diagnostics ==

struct Pos {
    int line = 1;
    int col = 1;
    int offset = 0;
};

enum class Severity { Error, Warning, Note };

struct Diagnostic {
    Severity severity = Severity::Error;
    Pos pos;
    std::string code;
    std::string message;
};

class DiagBag {
public:
    void error(Pos p, std::string code, std::string msg) {
        items_.push_back({Severity::Error, p, std::move(code), std::move(msg)});
    }
    void warning(Pos p, std::string code, std::string msg) {
        items_.push_back({Severity::Warning, p, std::move(code), std::move(msg)});
    }
    void note(Pos p, std::string code, std::string msg) {
        items_.push_back({Severity::Note, p, std::move(code), std::move(msg)});
    }
    bool hasError() const {
        for (const auto& d : items_)
            if (d.severity == Severity::Error) return true;
        return false;
    }
    std::size_t errorCount() const {
        std::size_t n = 0;
        for (const auto& d : items_)
            if (d.severity == Severity::Error) ++n;
        return n;
    }
    std::size_t warningCount() const {
        std::size_t n = 0;
        for (const auto& d : items_)
            if (d.severity == Severity::Warning) ++n;
        return n;
    }
    const std::vector<Diagnostic>& items() const { return items_; }
    void append(const DiagBag& other) {
        for (const auto& d : other.items_) items_.push_back(d);
    }
    void clear() { items_.clear(); }

private:
    std::vector<Diagnostic> items_;
};

// Renders diagnostics with a source excerpt (gcc/clang-ish layout).
std::string renderDiagnostics(const DiagBag& bag, const std::string& source,
                              const std::string& filename);

// ==================================================================== lexer ==

enum class Tok {
    End,
    Identifier,
    Number,
    String,
    TemplateString,
    Punct,
    Keyword,
    At,       // '@'
    Hash,     // '#'
    Under,    // '_'
    MathLit,  // '~pi' / '~ln4' / '~deg~e'
    TsBlock,  // verbatim payload of '@ts{ ... }'
};

struct Token {
    Tok kind = Tok::End;
    std::string text;   // raw lexeme as written
    std::string value;  // cooked value (identifier text, string body, math source)
    std::string raw;    // verbatim payload (@ts block body)
    double num = 0.0;   // Number / MathLit value
    bool newlineBefore = false;  // a line break separates this token from the previous one
    Pos pos;
    Pos end;

    bool isPunct(const char* p) const { return kind == Tok::Punct && text == p; }
    bool isKeyword(const char* k) const { return kind == Tok::Keyword && text == k; }
    bool isIdent(const char* n) const { return kind == Tok::Identifier && text == n; }
};

class Lexer {
public:
    Lexer(std::string source, std::string filename);
    std::vector<Token> tokenize(DiagBag& bag);

private:
    std::string src_;
    std::string file_;
    std::size_t i_ = 0;
    int line_ = 1;
    int col_ = 1;

    bool eof() const { return i_ >= src_.size(); }
    char peek(std::size_t k = 0) const { return i_ + k < src_.size() ? src_[i_ + k] : '\0'; }
    char advance();
    Pos here() const;
    void skipTrivia(DiagBag& bag);
    Token lexNumber(DiagBag& bag);
    Token lexString(DiagBag& bag);
    Token lexTemplate(DiagBag& bag);
    Token lexMath(DiagBag& bag);
    Token lexTsBlock(DiagBag& bag);
};

// ====================================================================== AST ==

enum class NK {
    // --- expressions
    Num, Str, Tpl, Bool, Void, Ident,
    MathConst,   // folded '~...' numeric literal (num holds the value)
    ArrayLit, ObjectLit, Prop,
    Unary, Binary, Compare, Ternary,
    Call, Member, Index, Spread, Await,
    MacroApply,  // '@name' with optional target + arguments
    SlotRef,     // '#name' inside a macro template
    SlotList,    // '...#name' variadic slot
    RangeExpr,   // start:end,step
    TsRaw,       // '@ts{ ... }' verbatim text
    Optionalize, // '?' prefix used inside macro templates
    Assign,      // x, y = 1, 2  (expression form kept for 'for' heads)
    TypeRef,     // a type as written in an annotation

    // --- statements
    Program, Block, ExprStmt, Empty,
    IncDec, Decl, Define, Declare, If, Case, CaseArm,
    ForWhile, ForOf, ForEach, ForRange, FnDecl, Return, Throw, Try,
    Break, Continue, Import, Export, MacroDecl,
    When, WhenArm, Each,
};

struct Node;
using NodePtr = std::shared_ptr<Node>;

// A single generic node. Explicit fields (rather than a variant) keep the
// parser, macro expander, checker and code generator readable.
struct Node {
    NK kind = NK::Empty;
    Pos pos;
    std::string text;   // name / operator / keyword / raw payload
    std::string raw;    // verbatim source (template literals, @ts blocks)
    std::string spec;   // import specifier
    double num = 0.0;   // numeric value
    bool flag = false;  // primary boolean attribute of the node
    bool flag2 = false; // secondary boolean attribute
    std::string typeAnn;   // declared type (decl / param / fn return)
    std::string typeAnn2;  // secondary declared type (fn return)

    NodePtr a, b, c, d;
    std::vector<NodePtr> list;      // statement lists, elements, args, params, arms
    std::vector<NodePtr> targets;   // assignment targets
    std::vector<NodePtr> values;    // assignment values
    std::vector<NodePtr> patterns;  // case arm patterns
    std::vector<std::string> names; // member chain / param names / labels
    std::vector<std::string> typeAnns;  // per-parameter type annotations
    std::vector<bool> flags;        // per-arm flags (fallthrough), per-param (optional)
    std::vector<NodePtr> defaults;  // parameter default values

    // ---- resolution results filled in by later phases ----
    std::string resolved;  // resolved name / mangled identifier
    bool constant = false; // proven compile-time constant
};

inline NodePtr mk(NK kind, Pos p) {
    auto n = std::make_shared<Node>();
    n->kind = kind;
    n->pos = p;
    return n;
}

const char* nodeKindName(NK k);
std::string dumpAst(const NodePtr& root);

// =================================================================== parser ==

struct ParserOptions {
    bool allowTopLevelStatements = true;
};

class Parser {
public:
    Parser(std::vector<Token> tokens, DiagBag& bag, ParserOptions opt = {});
    // Parses a whole file. On syntax errors a partial tree is still returned.
    NodePtr parseProgram();
    // Parses a standalone macro-template body (used by @ts-free stdlib parsing).
    NodePtr parseMacroBody();

private:
    std::vector<Token> t_;
    std::size_t i_ = 0;
    DiagBag& bag_;
    ParserOptions opt_;
    int macroDepth_ = 0;

    const Token& cur() const { return t_[i_ < t_.size() ? i_ : t_.size() - 1]; }
    const Token& peek(std::size_t k = 1) const {
        std::size_t j = i_ + k;
        return t_[j < t_.size() ? j : t_.size() - 1];
    }
    bool atEnd() const { return cur().kind == Tok::End; }
    Token take() { return t_[i_ < t_.size() ? i_++ : i_]; }
    bool check(Tok k) const { return cur().kind == k; }
    bool checkPunct(const char* p) const {
        if (cur().kind == Tok::Punct) return cur().text == p;
        if (cur().kind == Tok::At) return std::strcmp(p, "@") == 0;
        if (cur().kind == Tok::Hash) return std::strcmp(p, "#") == 0;
        return false;
    }
    bool checkKeyword(const char* k) const { return cur().isKeyword(k); }
    bool acceptPunct(const char* p) {
        if (checkPunct(p)) { ++i_; return true; }
        return false;
    }
    bool acceptKeyword(const char* k) {
        if (checkKeyword(k)) { ++i_; return true; }
        return false;
    }
    // A slot name (`#name`) may be spelled with a keyword: `#from`, `#default`.
    bool checkName() const {
        return cur().kind == Tok::Identifier || cur().kind == Tok::Keyword;
    }
    bool checkNameAt(std::size_t k) const {
        const Token& t = peek(k);
        return t.kind == Tok::Identifier || t.kind == Tok::Keyword;
    }
    std::string takeName() { return take().text; }
    bool expectPunct(const char* p, const char* ctx);
    bool expectKeyword(const char* k, const char* ctx);
    void errorHere(const std::string& code, const std::string& msg);
    std::string parseParamAnnotation();

    // statements
    NodePtr parseStatement();
    NodePtr parseParenExpr(const char* ctx);
    NodePtr parseBlock();
    NodePtr parseIf();
    NodePtr parseCase();
    NodePtr parseFor();
    NodePtr parseFnDecl(bool isAsync, bool isExport);
    NodePtr parseMacroDecl();
    NodePtr parseImport();
    NodePtr parseTry();
    NodePtr parseVarDecl(const std::string& kw);
    NodePtr parseDefine();
    NodePtr parseDeclare();
    void parseMacroTemplateBody(NodePtr body);

    // expressions
    NodePtr parseExpression();
    NodePtr parseAssignment();
    NodePtr parseTernary();
    NodePtr parseLogicalOr();
    NodePtr parseLogicalAnd();
    NodePtr parseEquality();
    NodePtr parseRelational();
    NodePtr parseTypeNameRHS();
    NodePtr parseAdditive();
    NodePtr parseMultiplicative();
    NodePtr parsePower();
    NodePtr parseUnary();
    NodePtr parsePostfix();
    NodePtr parsePrimary();
    NodePtr parseCallArgs(NodePtr callee, bool safe);
    NodePtr parseArrayLit();
    NodePtr parseObjectLit();
    NodePtr parseRangeOrExpr(bool allowRange);
    NodePtr parseMacroApply(const Token& atToken, bool prefix);
    NodePtr parseSlotFragment();
    NodePtr parseMacroArgList();
    NodePtr parseTemplateEach();
    NodePtr parseTemplateWhen();
    bool isWhenArmStart() const;
    bool isNamedSlotStart() const;
    bool isNamedSlotBlock() const;
    void parseWhenArmBody(const NodePtr& arm);

    std::string parseTypeAnnotation();
    bool isTypeStart(const Token& t) const;
    bool startsStatement() const;
};

// ============================================================ macro expander =

enum class SlotType { Expr, Stmt, Type, ExprList, CallExpr, SafeCallExpr, RangeExpr, Unknown };

struct MacroDef {
    std::string name;
    std::vector<std::string> params;          // without '#'
    std::vector<SlotType> paramTypes;
    std::vector<std::string> paramTypeNames;  // as written, for diagnostics
    std::vector<std::vector<NK>> paramKinds;  // exact AST kinds required (empty = any)
    std::vector<bool> variadic;
    std::vector<bool> optional;  // `#name: Type?` - the slot may be omitted
    std::vector<NodePtr> body;  // template statements
    bool system = false;
    Pos pos;
};

// Maps a slot-type name to the exact AST node kinds it accepts.
// Returns an empty vector for names that are not AST node kinds.
std::vector<NK> astKindsForTypeName(const std::string& name);
// Human-readable list of every AST slot-type name (for docs / diagnostics).
const std::vector<std::string>& astSlotTypeNames();

struct Binding {
    NodePtr node;
    SlotType slot = SlotType::Expr;
    TypePtr type;
    bool nil = false;
};

using Bindings = std::unordered_map<std::string, Binding>;

class MacroExpander {
public:
    MacroExpander(DiagBag& bag);
    void installStdlib();
    // Expands macros / inlines `define` constants; returns the desugared core AST.
    NodePtr expand(const NodePtr& program);

    const std::unordered_map<std::string, MacroDef>& macros() const { return macros_; }

    // Registers one `macro` declaration (used by the module loader as well).
    void registerMacroFromDecl(const NodePtr& decl);

    // Used by the template instantiation engine (same translation unit).
    NodePtr cloneNodePublic(const NodePtr& n) const;
    SlotType inferSlotTypePublic(const NodePtr& n) const;
    SlotType inferSlotType(const NodePtr& n) const;
    std::string slotTypeName(SlotType t) const;
    bool evalWhen(const NodePtr& cond,
                  const std::unordered_map<std::string, NodePtr>& binds) const;

private:
    DiagBag& bag_;
    std::unordered_map<std::string, MacroDef> macros_;
    std::unordered_map<std::string, TypePtr> env_;
    std::unordered_map<std::string, TypePtr> declaredTypes_;
    std::unordered_map<std::string, NodePtr> defines_;
    bool stdlibInstalled_ = false;

    void registerMacro(const MacroDef& def);
    void collectMacroDecls(const NodePtr& n);
    void collectDefines(const NodePtr& n);
    NodePtr substituteDefines(const NodePtr& n);
    NodePtr foldConstant(const NodePtr& n) const;

    NodePtr expandNode(const NodePtr& n);
    std::vector<NodePtr> expandStmts(const std::vector<NodePtr>& list);
    void expandList(const std::vector<NodePtr>& list, std::vector<NodePtr>& out);
    NodePtr expandMacroApply(const NodePtr& call, int depth);
    std::vector<NodePtr> expandMacroStatements(const NodePtr& call, int depth);
    const MacroDef* lookupMacro(const NodePtr& call) const;
    bool bindArguments(const MacroDef& def, const NodePtr& call, Bindings& binds);
    NodePtr cloneNode(const NodePtr& n) const;
    bool typeMatches(SlotType want, SlotType got) const;
};

// ==================================================================== types ==

enum class TK {
    Any, Unknown, Void, Null, Bool, Num, Str, Array, Object, Map, Set, Fn,
    Union, TypeParam, Class, Literal, Range, Never,
};

struct Type;
using TypePtr = std::shared_ptr<Type>;

struct Type {
    TK kind = TK::Unknown;
    std::string name;            // class / type-parameter / literal display name
    TypePtr elem;                // array element / set element / map value
    TypePtr key;                 // map key
    std::vector<TypePtr> parts;  // union members / function params
    TypePtr ret;                 // function return
    bool optional = false;
    int required = -1;           // function: number of required parameters (-1 unknown)
    bool variadic = false;       // function: accepts a rest parameter
    std::string literal;         // literal type payload
    double numLit = 0.0;
    // Structural members contributed by a `declare` block (host objects).
    std::unordered_map<std::string, TypePtr> fields;
    std::unordered_map<std::string, TypePtr> methods;

    std::string str() const;
};

TypePtr tAny();
TypePtr tUnknown();
TypePtr tVoid();
TypePtr tBool();
TypePtr tNum();
TypePtr tStr();
TypePtr tArray(TypePtr e);
TypePtr tMap(TypePtr k, TypePtr v);
TypePtr tSet(TypePtr e);
TypePtr tObject();
TypePtr tFn(std::vector<TypePtr> ps, TypePtr r);
TypePtr tUnion(std::vector<TypePtr> parts);
TypePtr tNamed(const std::string& n);
TypePtr tRange();
TypePtr tRangeOf(const TypePtr& elem);
bool typeEquals(const TypePtr& a, const TypePtr& b);
bool isAssignable(const TypePtr& to, const TypePtr& from);
// Parses a normalised annotation string (as produced by the parser).
TypePtr parseTypeString(const std::string& ann);
// Builds the type described by a `declare` statement: a structural class for the
// block form, a function type for `declare fn`. Returns nullptr otherwise.
TypePtr buildDeclaredType(const NodePtr& declareNode);

class TypeChecker {
public:
    TypeChecker(DiagBag& bag);
    void check(const NodePtr& program);
    // Types recorded for identifiers; used by the code generator for `@len`-like
    // decisions and by the docs/debug dumps.
    const std::unordered_map<const Node*, TypePtr>& exprTypes() const { return exprTypes_; }
    // Member accesses that are calls *without* parentheses.
    //
    // `x y` is a property read by default; it is a call only when the resolved member
    // is a function with no required parameters (`p judge` -> `p.judge()`). Writing the
    // parentheses yourself always makes it a call, with no type information needed.
    const std::unordered_set<const Node*>& implicitCallMembers() const { return implicitCallMembers_; }

private:
    DiagBag& bag_;
    std::unordered_map<const Node*, TypePtr> exprTypes_;
    std::unordered_map<std::string, TypePtr> declaredTypes_;
    std::unordered_set<const Node*> implicitCallMembers_;
    struct Scope;
    std::vector<std::shared_ptr<Scope>> scopes_;

    void pushScope();
    void popScope();
    void declare(const std::string& name, const TypePtr& t, const Pos& p);
    TypePtr lookup(const std::string& name) const;
    bool lookupConst(const std::string& name) const;
    void markConst(const std::string& name);
    void collectDeclares(const NodePtr& n);
    TypePtr memberType(const TypePtr& obj, const std::string& member) const;

    TypePtr checkNode(const NodePtr& n);
    TypePtr checkStatements(const std::vector<NodePtr>& stmts);
    TypePtr resolveAnnotation(const std::string& ann, const Pos& p);
    bool checkAssignable(const TypePtr& to, const TypePtr& from, const Pos& p,
                         const std::string& what);
    void hoistFunctions(const std::vector<NodePtr>& stmts);
};

// ================================================================ codegen ===

struct CodegenOptions {
    int indentWidth = 2;
    bool emitDebugComments = false;
    bool stripTypes = true;
};

// Marks the start of each physical line of a verbatim `@ts{ … }` payload inside generated
// code. `formatJavaScript` strips it and leaves the marked lines exactly as written.
inline constexpr char kRawMark = '\x02';

// The formatting stage that runs over the generated JavaScript (src/format.cpp).
//
// It only ever changes whitespace, and it never touches a line carrying `kRawMark`, so a
// `@ts{ … }` payload reaches the output byte for byte. With `tidy` off it still strips the
// marks and normalises line endings, but leaves the layout alone.
std::string formatJavaScript(const std::string& src, int indentWidth, bool tidy);

class Codegen {
public:
    Codegen(DiagBag& bag, CodegenOptions opt = {});
    std::string generate(const NodePtr& program, const std::string& sourceFile);
    // Members that carry no parentheses but must still be emitted as calls.
    void setImplicitCallMembers(const std::unordered_set<const Node*>* members) {
        implicitCallMembers_ = members;
    }

private:
    struct Rendered {
        std::string text;
        int prec = 11;
    };

    DiagBag& bag_;
    CodegenOptions opt_;
    std::string out_;
    int depth_ = 0;
    std::string file_;
    bool pendingExport_ = false;
    const std::unordered_set<const Node*>* implicitCallMembers_ = nullptr;
    std::vector<std::unordered_set<std::string>> scopes_;
    std::vector<std::unordered_map<std::string, int>> assignCounts_;

    void line(const std::string& s);
    std::string pad() const;

    void genStatements(const std::vector<NodePtr>& stmts);
    void genStatement(const NodePtr& n);
    void genIfChain(const NodePtr& n);
    std::string captureStatement(const NodePtr& n);
    Rendered genExprP(const NodePtr& n);
    std::string genExpr(const NodePtr& n, int parentPrec = 0);
    std::string genFragment(const NodePtr& n, bool optional);
    std::string genArguments(const std::vector<NodePtr>& args);
    std::string genBlockOf(const std::vector<NodePtr>& stmts);
    void genBranchBody(const NodePtr& body);
    std::string genParams(const NodePtr& n);
    std::string genTsRaw(const NodePtr& n);
    std::string genCompare(const NodePtr& n);

    void countAssignments(const std::vector<NodePtr>& stmts,
                          std::unordered_map<std::string, int>& out);
    void scanAssignments(const NodePtr& n, std::unordered_map<std::string, int>& out);
    int assignCountFor(const std::string& name) const;
    bool isDeclared(const std::string& name) const;
    void declareName(const std::string& name);
    void pushAssignCounts(const std::vector<NodePtr>& stmts);
    void popAssignCounts();
    static bool hasSideEffects(const NodePtr& n);

    static int precedence(const std::string& op);
    static std::string escapeJsString(const std::string& s);
    static std::string formatNumber(double d);
};

// ================================================================= pipeline ==

struct CompileOptions {
    bool dumpTokens = false;
    bool dumpAst = false;
    bool dumpCoreAst = false;
    bool noTypecheck = false;
    bool warningsAsErrors = false;
    bool noFormat = false;
    std::vector<std::string> includePaths;  // extra search roots for `import "*.shya"`
};

struct CompileResult {
    bool ok = false;
    std::string code;
    std::string tokensDump;
    std::string astDump;
    std::string coreAstDump;
    DiagBag diags;
};

// Compiles shya source into ES2026 JavaScript. Never throws for user errors.
CompileResult compileSource(const std::string& source, const std::string& filename,
                            const CompileOptions& opt = {});

// Resolves `import ... from "*.shya"` statements: loads each macro file, registers
// its macros (transitively, honouring an explicit `{ @name }` list) and splices in
// its `define` / `declare` statements. Returns the program without those imports.
NodePtr loadShyaModules(const NodePtr& program, const std::string& sourcePath,
                        const std::vector<std::string>& includePaths,
                        MacroExpander& expander, DiagBag& bag);

// The embedded standard library (shya source for the macro layer).
extern const char* kStdlibSource;

}  // namespace shya
