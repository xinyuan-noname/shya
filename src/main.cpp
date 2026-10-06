// shya - command line interface and the compile pipeline.
#include "shya.h"

#include <fstream>

namespace shya {

namespace {

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    ok = true;
    return ss.str();
}

bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

std::string dirOf(const std::string& path) {
    std::size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? std::string(".") : path.substr(0, p);
}

std::string baseName(const std::string& path) {
    std::size_t p = path.find_last_of("/\\");
    std::string f = p == std::string::npos ? path : path.substr(p + 1);
    std::size_t d = f.find_last_of('.');
    return d == std::string::npos ? f : f.substr(0, d);
}

std::string extensionOf(const std::string& path) {
    std::size_t p = path.find_last_of("/\\");
    std::string f = p == std::string::npos ? path : path.substr(p + 1);
    std::size_t d = f.find_last_of('.');
    return d == std::string::npos ? std::string() : f.substr(d + 1);
}

const char* kUsage =
    "shya - a strongly-typed DSL that compiles to ES2026 JavaScript\n"
    "\n"
    "usage:\n"
    "  shya build <file.shya> [-o out.js]   compile to JavaScript\n"
    "  shya check <file.shya>               type-check only, no output\n"
    "  shya tokens <file.shya>              dump the token stream\n"
    "  shya ast <file.shya>                 dump the parsed AST\n"
    "  shya core <file.shya>                dump the desugared core AST\n"
    "  shya run <file.shya> [-o out.mjs]    compile and execute with node\n"
    "  shya version                         print the compiler version\n"
    "  shya help                            print this message\n"
    "\n"
    "options:\n"
    "  -o, --out <path>      output file (default: alongside the source, .mjs)\n"
    "  -I, --include <dir>   extra search root for `import \"*.shya\"` macro files\n"
    "  --no-typecheck        skip the type checker\n"
    "  --keep-types          keep type annotations in the output\n"
    "  --warnings-as-errors  treat warnings as errors\n"
    "  -q, --quiet           only print errors\n";

}  // namespace

CompileResult compileSource(const std::string& source, const std::string& filename,
                            const CompileOptions& opt) {
    CompileResult result;

    // 1. lexer -----------------------------------------------------------
    Lexer lexer(source, filename);
    DiagBag bag;
    auto tokens = lexer.tokenize(bag);
    if (opt.dumpTokens) {
        std::ostringstream os;
        for (const auto& t : tokens) {
            if (t.kind == Tok::End) break;
            os << t.pos.line << ':' << t.pos.col << '\t';
            switch (t.kind) {
                case Tok::Identifier: os << "IDENT"; break;
                case Tok::Number: os << "NUMBER"; break;
                case Tok::String: os << "STRING"; break;
                case Tok::TemplateString: os << "TEMPLATE"; break;
                case Tok::Punct: os << "PUNCT"; break;
                case Tok::Keyword: os << "KEYWORD"; break;
                case Tok::At: os << "AT"; break;
                case Tok::Hash: os << "HASH"; break;
                case Tok::Under: os << "UNDER"; break;
                case Tok::MathLit: os << "MATHLIT"; break;
                case Tok::TsBlock: os << "TSBLOCK"; break;
                default: os << "END"; break;
            }
            os << '\t' << t.text;
            if (t.kind == Tok::Number || t.kind == Tok::MathLit) os << "\t= " << t.num;
            os << '\n';
        }
        result.tokensDump = os.str();
    }
    if (bag.hasError()) {
        result.diags = bag;
        return result;
    }

    // 2. parser ----------------------------------------------------------
    Parser parser(tokens, bag);
    NodePtr program = parser.parseProgram();
    if (opt.dumpAst) result.astDump = dumpAst(program);
    if (bag.hasError()) {
        result.diags = bag;
        return result;
    }

    // 3. resolve `import ... from "*.shya"` macro modules ------------------
    MacroExpander expander(bag);
    expander.installStdlib();
    NodePtr resolved = loadShyaModules(program, filename, opt.includePaths, expander, bag);
    if (bag.hasError()) {
        result.diags = bag;
        return result;
    }

    // 4. macro expansion / desugaring ------------------------------------
    NodePtr core = expander.expand(resolved);
    if (opt.dumpCoreAst) result.coreAstDump = dumpAst(core);
    if (bag.hasError()) {
        result.diags = bag;
        return result;
    }

    // 5. type checking ---------------------------------------------------
    if (!opt.noTypecheck) {
        TypeChecker checker(bag);
        checker.check(core);
        if (bag.hasError()) {
            result.diags = bag;
            return result;
        }
        // 6. code generation ---------------------------------------------
        CodegenOptions copt;
        copt.stripTypes = true;
        Codegen codegen(bag, copt);
        codegen.setFieldAccesses(&checker.fieldAccesses());
        result.code = codegen.generate(core, filename);
    } else {
        CodegenOptions copt;
        copt.stripTypes = true;
        Codegen codegen(bag, copt);
        result.code = codegen.generate(core, filename);
    }

    if (opt.warningsAsErrors) {
        for (const auto& d : bag.items()) {
            if (d.severity == Severity::Warning) {
                bag.error(d.pos, d.code, "（-Werror）" + d.message);
            }
        }
    }

    result.diags = bag;
    result.ok = !bag.hasError();
    return result;
}

namespace {

std::string quoteForShell(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out += "\"";
    return out;
}

}  // namespace

}  // namespace shya

int main(int argc, char** argv) {
    using namespace shya;

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

    if (args.empty()) {
        std::fputs(kUsage, stdout);
        return 1;
    }

    const std::string command = args[0];
    if (command == "help" || command == "--help" || command == "-h") {
        std::fputs(kUsage, stdout);
        return 0;
    }
    if (command == "version" || command == "--version" || command == "-v") {
        std::puts("shya 1.0.0 (ES2026 backend)");
        return 0;
    }

    static const std::set<std::string> commands = {"build", "check", "tokens", "ast",
                                                   "core", "run"};
    if (!commands.count(command)) {
        std::fprintf(stderr, "shya: unknown command `%s`\n\n", command.c_str());
        std::fputs(kUsage, stderr);
        return 1;
    }

    CompileOptions opt;
    std::string input;
    std::string output;
    bool quiet = false;
    bool keepTypes = false;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-o" || a == "--out") {
            if (i + 1 >= args.size()) {
                std::fputs("shya: -o requires a path\n", stderr);
                return 1;
            }
            output = args[++i];
        } else if (a == "-I" || a == "--include") {
            if (i + 1 >= args.size()) {
                std::fputs("shya: -I requires a directory\n", stderr);
                return 1;
            }
            opt.includePaths.push_back(args[++i]);
        } else if (a == "--no-typecheck") {
            opt.noTypecheck = true;
        } else if (a == "--keep-types") {
            keepTypes = true;
        } else if (a == "--warnings-as-errors" || a == "-Werror") {
            opt.warningsAsErrors = true;
        } else if (a == "-q" || a == "--quiet") {
            quiet = true;
        } else if (!a.empty() && a[0] == '-' && a.size() > 1) {
            std::fprintf(stderr, "shya: unknown option `%s`\n", a.c_str());
            return 1;
        } else if (input.empty()) {
            input = a;
        } else {
            std::fprintf(stderr, "shya: unexpected argument `%s`\n", a.c_str());
            return 1;
        }
    }

    if (input.empty()) {
        std::fprintf(stderr, "shya %s: missing input file\n", command.c_str());
        return 1;
    }

    bool ok = false;
    std::string source = readFile(input, ok);
    if (!ok) {
        std::fprintf(stderr, "shya: cannot read `%s`\n", input.c_str());
        return 1;
    }
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
        static_cast<unsigned char>(source[1]) == 0xBB &&
        static_cast<unsigned char>(source[2]) == 0xBF) {
        source.erase(0, 3);
    }

    opt.dumpTokens = (command == "tokens");
    opt.dumpAst = (command == "ast" || command == "core");
    opt.dumpCoreAst = (command == "core");
    (void)keepTypes;

    CompileResult result = compileSource(source, input, opt);

    if (opt.dumpTokens && !result.tokensDump.empty()) std::fputs(result.tokensDump.c_str(), stdout);
    if (opt.dumpAst && !result.astDump.empty()) std::fputs(result.astDump.c_str(), stdout);
    if (opt.dumpCoreAst && !result.coreAstDump.empty())
        std::fputs(result.coreAstDump.c_str(), stdout);

    if (result.diags.errorCount() > 0 || (!quiet && result.diags.warningCount() > 0) ||
        (command != "tokens" && command != "ast" && command != "core" && !quiet)) {
        std::string rendered = renderDiagnostics(result.diags, source, input);
        if (!rendered.empty()) std::fputs(rendered.c_str(), stderr);
    }

    if (!result.ok) {
        if (result.diags.errorCount() == 0) {
            std::fprintf(stderr, "shya: %s failed\n", command.c_str());
        } else {
            std::fprintf(stderr, "shya: %s failed with %zu error(s)\n", command.c_str(),
                         result.diags.errorCount());
        }
        return 1;
    }

    if (command == "tokens" || command == "ast" || command == "core" || command == "check") {
        if (!quiet) std::fprintf(stderr, "shya: %s OK\n", command.c_str());
        return 0;
    }

    if (output.empty()) {
        std::string dir = dirOf(input);
        output = dir + "/" + baseName(input);
        output += (command == "run") ? ".mjs" : ".mjs";
    }

    if (!writeFile(output, result.code)) {
        std::fprintf(stderr, "shya: cannot write `%s`\n", output.c_str());
        return 1;
    }
    if (!quiet) std::fprintf(stderr, "shya: wrote %s (%zu bytes)\n", output.c_str(),
                             result.code.size());

    if (command == "run") {
        std::string cmd = "node " + quoteForShell(output);
        int rc = std::system(cmd.c_str());
        return rc;
    }
    return 0;
}
