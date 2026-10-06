// shya - module loading for macro files.
//
// `import ... from "./x.shya"` is resolved at compile time: the file is parsed,
// its macros are registered (transitively, honouring an explicit name list) and
// its `define` / `declare` statements are spliced into the importing program.
// Nothing is emitted to JavaScript for these imports.
#include "shya.h"

#include <fstream>
#include <set>

namespace shya {

namespace {

std::string readText(const std::string& path, bool& ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    ok = true;
    std::string s = ss.str();
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

std::string dirName(const std::string& p) {
    std::size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? std::string(".") : p.substr(0, k);
}

// Canonical form used for the load cache and the cycle guard: lowercase,
// forward slashes, and `.` / `..` segments collapsed. Without the collapsing,
// `./a.shya` and `././a.shya` would look like two different files and the
// cycle guard would never fire.
std::string normalisePath(const std::string& p) {
    std::string s = p;
    for (char& c : s) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    bool absolute = !s.empty() && s[0] == '/';
    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '/') {
            if (cur.empty() || cur == ".") {
                // nothing
            } else if (cur == "..") {
                if (!parts.empty() && parts.back() != "..") parts.pop_back();
                else if (!absolute) parts.push_back("..");
            } else {
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur.push_back(s[i]);
        }
    }
    std::string out = absolute ? "/" : "";
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '/';
        out += parts[i];
    }
    return out.empty() ? std::string(".") : out;
}

bool pathExists(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return static_cast<bool>(in);
}

bool isAbsolute(const std::string& p) {
    return p.size() > 1 && (p[0] == '/' || p[0] == '\\' || (p.size() > 2 && p[1] == ':'));
}

bool isRelative(const std::string& p) {
    return p.rfind("./", 0) == 0 || p.rfind("../", 0) == 0 || p.rfind(".\\", 0) == 0 ||
           p.rfind("..\\", 0) == 0;
}

// Collects every `macro` / `define` / `declare` reachable from a statement list.
void collectDecls(const NodePtr& n, std::unordered_map<std::string, NodePtr>& macros,
                  std::vector<NodePtr>& others) {
    if (!n) return;
    switch (n->kind) {
        case NK::MacroDecl:
            macros[n->text] = n;
            return;
        case NK::Define:
        case NK::Declare:
            others.push_back(n);
            return;
        case NK::FnDecl:
            return;
        default:
            break;
    }
    for (const auto& k : n->list) collectDecls(k, macros, others);
}

// Every macro name a template body applies with `@name`.
void collectMacroUses(const NodePtr& n, std::set<std::string>& out) {
    if (!n) return;
    if (n->kind == NK::MacroApply && !n->text.empty()) out.insert(n->text);
    for (const auto& k : {n->a, n->b, n->c, n->d}) collectMacroUses(k, out);
    for (const auto& k : n->list) collectMacroUses(k, out);
    for (const auto& k : n->targets) collectMacroUses(k, out);
    for (const auto& k : n->values) collectMacroUses(k, out);
    for (const auto& k : n->patterns) collectMacroUses(k, out);
}

struct ModuleSet {
    std::unordered_map<std::string, NodePtr> macros;  // name -> MacroDecl
    std::vector<NodePtr> others;                      // Define / Declare, in load order
};

class Loader {
public:
    Loader(const std::vector<std::string>& includePaths, DiagBag& bag)
        : includePaths_(includePaths), bag_(bag) {}

    // Loads `spec` as seen from `fromDir`. Returns false when it cannot be found.
    bool load(const std::string& spec, const std::string& fromDir, const Pos& at,
              ModuleSet& into) {
        std::string path = resolve(spec, fromDir);
        if (path.empty()) {
            bag_.error(at, "MOD001", "找不到宏文件 `" + spec + "`");
            return false;
        }
        if (depth_ > 64) {
            bag_.error(at, "MOD002", "宏文件导入层数过深（可能存在循环导入）：`" + spec + "`");
            return false;
        }
        std::string key = normalisePath(path);
        auto cached = cache_.find(key);
        if (cached != cache_.end()) {
            // Already loaded by an earlier import: reuse the same declarations.
            for (const auto& kv : cached->second.macros) into.macros[kv.first] = kv.second;
            for (const auto& o : cached->second.others) into.others.push_back(o);
            return true;
        }
        if (loading_.count(key)) {
            bag_.error(at, "MOD002", "宏文件循环导入：`" + spec + "`");
            return false;
        }
        loading_.insert(key);

        bool ok = false;
        std::string text = readText(path, ok);
        if (!ok) {
            loading_.erase(key);
            bag_.error(at, "MOD001", "无法读取宏文件 `" + spec + "`");
            return false;
        }

        DiagBag sub;
        Lexer lexer(text, path);
        auto tokens = lexer.tokenize(sub);
        Parser parser(tokens, sub);
        NodePtr prog = parser.parseProgram();
        bool bad = sub.hasError();
        int shown = 0;
        for (const auto& d : sub.items()) {
            if (d.severity != Severity::Error) continue;
            if (++shown > 6) break;
            bag_.error(at, "MOD003",
                       "宏文件 " + path + ":" + std::to_string(d.pos.line) + ":" +
                           std::to_string(d.pos.col) + "：" + d.message);
        }
        if (bad) {
            loading_.erase(key);
            return false;
        }

        // Depth first: the file's own .shya imports first.
        std::string dir = dirName(path);
        depth_++;
        for (const auto& st : prog->list) {
            if (!st || st->kind != NK::Import) continue;
            if (!st->flag2) {
                bag_.error(at, "MOD004",
                           "宏文件 " + path + " 里只能导入 .shya 宏文件（发现 `" + st->spec +
                               "`）");
                continue;
            }
            load(st->spec, dir, at, into);
        }
        depth_--;

        std::unordered_map<std::string, NodePtr> local;
        std::vector<NodePtr> localOther;
        collectDecls(prog, local, localOther);
        for (auto& kv : local) into.macros[kv.first] = kv.second;
        for (auto& o : localOther) into.others.push_back(o);

        loading_.erase(key);
        cache_[key] = into;
        return true;
    }

private:
    std::string resolve(const std::string& spec, const std::string& fromDir) const {
        if (isAbsolute(spec)) return pathExists(spec) ? normalisePath(spec) : std::string();
        if (isRelative(spec)) {
            std::string candidate = fromDir + "/" + spec;
            return pathExists(candidate) ? normalisePath(candidate) : std::string();
        }
        std::string local = fromDir + "/" + spec;
        if (pathExists(local)) return normalisePath(local);
        for (const auto& inc : includePaths_) {
            std::string candidate = inc + "/" + spec;
            if (pathExists(candidate)) return normalisePath(candidate);
        }
        return std::string();
    }

    const std::vector<std::string>& includePaths_;
    DiagBag& bag_;
    std::set<std::string> loading_;
    std::unordered_map<std::string, ModuleSet> cache_;
    int depth_ = 0;
};

NodePtr cloneShallow(const NodePtr& n) {
    if (!n) return nullptr;
    auto c = std::make_shared<Node>(*n);
    c->a = cloneShallow(n->a);
    c->b = cloneShallow(n->b);
    c->c = cloneShallow(n->c);
    c->d = cloneShallow(n->d);
    c->list.clear();
    for (const auto& k : n->list) c->list.push_back(cloneShallow(k));
    return c;
}

}  // namespace

NodePtr loadShyaModules(const NodePtr& program, const std::string& sourcePath,
                        const std::vector<std::string>& includePaths,
                        MacroExpander& expander, DiagBag& bag) {
    Loader loader(includePaths, bag);
    std::string baseDir = sourcePath.empty() ? std::string(".") : dirName(sourcePath);

    auto out = mk(NK::Program, program->pos);
    std::set<std::string> registered;

    std::function<void(const std::vector<NodePtr>&, std::vector<NodePtr>&)> handleList =
        [&](const std::vector<NodePtr>& in, std::vector<NodePtr>& outList) {
            for (const auto& st : in) {
                if (!st) continue;
                if (st->kind == NK::Import && st->flag2) {
                    ModuleSet set;
                    if (!loader.load(st->spec, baseDir, st->pos, set)) continue;

                    // Choose which macros to register.
                    std::vector<std::string> wanted;
                    if (!st->names.empty() && !st->flag) {
                        for (const auto& n : st->names) wanted.push_back(n);
                    } else if (st->flag) {
                        for (const auto& n : st->names) {
                            if (!n.empty() && n[0] == '@') wanted.push_back(n.substr(1));
                        }
                    }
                    std::set<std::string> closure;
                    if (wanted.empty()) {
                        for (const auto& kv : set.macros) closure.insert(kv.first);
                    } else {
                        std::vector<std::string> queue = wanted;
                        while (!queue.empty()) {
                            std::string name = queue.back();
                            queue.pop_back();
                            if (closure.count(name)) continue;
                            auto it = set.macros.find(name);
                            if (it == set.macros.end()) {
                                bag.error(st->pos, "MOD005",
                                          "宏文件 `" + st->spec + "` 里没有宏 `@" + name + "`");
                                continue;
                            }
                            closure.insert(name);
                            std::set<std::string> uses;
                            collectMacroUses(it->second, uses);
                            for (const auto& u : uses)
                                if (!closure.count(u)) queue.push_back(u);
                        }
                    }
                    for (const auto& name : closure) {
                        auto it = set.macros.find(name);
                        if (it != set.macros.end() && !registered.count(name)) {
                            expander.registerMacroFromDecl(it->second);
                            registered.insert(name);
                        }
                    }
                    // `define` / `declare` from the imported file(s).
                    for (const auto& o : set.others) outList.push_back(cloneShallow(o));
                    continue;
                }
                if (st->kind == NK::Import && !st->flag2 && st->spec.size() > 5 &&
                    st->spec.compare(st->spec.size() - 5, 5, ".shya") == 0) {
                    // .shya without the flag should not happen; be safe.
                    continue;
                }
                if (st->kind == NK::Block || st->kind == NK::FnDecl || st->kind == NK::Program) {
                    auto c = cloneShallow(st);
                    std::vector<NodePtr> inner;
                    handleList(st->list, inner);
                    c->list = inner;
                    outList.push_back(c);
                    continue;
                }
                outList.push_back(st);
            }
        };

    handleList(program->list, out->list);
    return out;
}

}  // namespace shya
