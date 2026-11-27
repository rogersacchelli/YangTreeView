// yang_core.cpp - port of yang2html.py (parser, module loader, grouping
// resolution, JSON payload, HTML page). Portable C++17, no dependencies.
#include "yang_core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "template_html.h"
#include "version.h"

namespace yang2html {

const char* version() { return YTV_VERSION_STR; }

// ------------------------------------------------------------------------- //
// Data model
// ------------------------------------------------------------------------- //

struct Stmt {
    std::string k, a;
    bool hasA = false;
    int l = 0;
    std::vector<Stmt*> c;
    std::string g, f, r;   // grouping id, source name, resolved grouping ref
};

struct YangError : std::runtime_error {
    int line;
    YangError(int ln, const std::string& m)
        : std::runtime_error(ln > 0 ? "line " + std::to_string(ln) + ": " + m : m), line(ln) {}
};

// ------------------------------------------------------------------------- //
// Tokenizer
// ------------------------------------------------------------------------- //

enum TokT { T_WORD, T_STR, T_LBRACE, T_RBRACE, T_SEMI };
struct Tok {
    TokT t;
    std::string v;
    int line;
};

static bool isWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

static std::string rstripWs(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (isWs(s[e - 1]) || s[e - 1] == '\n')) --e;
    return s.substr(0, e);
}

// Escapes + RFC 7950 6.1.3 indentation stripping for "..." strings.
static std::string dequoteDouble(const std::string& raw, size_t col) {
    std::string s;
    s.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            char n = raw[i + 1];
            switch (n) {
                case 'n': s += '\n'; break;
                case 't': s += '\t'; break;
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                default: s += '\\'; s += n; break;
            }
            i += 2;
            continue;
        }
        s += c;
        ++i;
    }
    if (s.find('\n') == std::string::npos) return s;
    std::string res;
    size_t start = 0;
    bool first = true;
    while (true) {
        size_t nl = s.find('\n', start);
        std::string ln = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (first) {
            res = rstripWs(ln);
            first = false;
        } else {
            size_t k = 0;
            while (k < ln.size() && k <= col && (ln[k] == ' ' || ln[k] == '\t')) ++k;
            res += '\n';
            res += rstripWs(ln.substr(k));
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return res;
}

static std::vector<Tok> tokenize(const std::string& text) {
    std::vector<Tok> toks;
    size_t i = 0, n = text.size(), lineStart = 0;
    int line = 1;
    auto countNl = [](const std::string& s, size_t a, size_t b, size_t& lastNl) {
        int cnt = 0;
        for (size_t x = a; x < b; ++x)
            if (s[x] == '\n') { ++cnt; lastNl = x; }
        return cnt;
    };
    while (i < n) {
        char c = text[i];
        if (c == '\n') { ++line; ++i; lineStart = i; continue; }
        if (isWs(c)) { ++i; continue; }
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            size_t j = text.find('\n', i);
            i = (j == std::string::npos) ? n : j;
            continue;
        }
        if (c == '/' && i + 1 < n && text[i + 1] == '*') {
            size_t j = text.find("*/", i + 2);
            if (j == std::string::npos) throw YangError(line, "unterminated block comment");
            size_t last = 0;
            int nl = countNl(text, i, j + 2, last);
            if (nl) { line += nl; lineStart = last + 1; }
            i = j + 2;
            continue;
        }
        if (c == '{') { toks.push_back({T_LBRACE, "{", line}); ++i; continue; }
        if (c == '}') { toks.push_back({T_RBRACE, "}", line}); ++i; continue; }
        if (c == ';') { toks.push_back({T_SEMI, ";", line}); ++i; continue; }
        if (c == '"') {
            size_t col = i - lineStart;
            size_t j = i + 1;
            while (j < n && text[j] != '"') j += (text[j] == '\\') ? 2 : 1;
            if (j >= n) throw YangError(line, "unterminated double-quoted string");
            std::string raw = text.substr(i + 1, j - i - 1);
            toks.push_back({T_STR, dequoteDouble(raw, col), line});
            size_t last = 0;
            int nl = countNl(text, i + 1, j, last);
            if (nl) { line += nl; lineStart = last + 1; }
            i = j + 1;
            continue;
        }
        if (c == '\'') {
            size_t j = text.find('\'', i + 1);
            if (j == std::string::npos) throw YangError(line, "unterminated single-quoted string");
            toks.push_back({T_STR, text.substr(i + 1, j - i - 1), line});
            size_t last = 0;
            int nl = countNl(text, i + 1, j, last);
            if (nl) { line += nl; lineStart = last + 1; }
            i = j + 1;
            continue;
        }
        size_t j = i;
        while (j < n) {
            char d = text[j];
            if (d == ' ' || d == '\t' || d == '\r' || d == '\n' || d == '\f' || d == '\v' || d == ';' ||
                d == '{' || d == '}' || d == '"' || d == '\'')
                break;
            ++j;
        }
        toks.push_back({T_WORD, text.substr(i, j - i), line});
        i = j;
    }
    return toks;
}

// ------------------------------------------------------------------------- //
// Parser
// ------------------------------------------------------------------------- //

class Parser {
   public:
    Parser(std::deque<Stmt>& arena, std::vector<Tok> toks) : arena_(arena), toks_(std::move(toks)) {}

    Stmt* parseModule() {
        if (toks_.empty()) throw YangError(0, "empty file");
        Stmt* root = statement();
        if (pos_ < toks_.size()) throw YangError(toks_[pos_].line, "unexpected content after module end");
        return root;
    }

   private:
    std::deque<Stmt>& arena_;
    std::vector<Tok> toks_;
    size_t pos_ = 0;

    bool atEnd() const { return pos_ >= toks_.size(); }
    int lastLine() const { return toks_.empty() ? 0 : toks_.back().line; }

    Stmt* statement() {
        if (atEnd()) throw YangError(lastLine(), "expected keyword, got end of file");
        const Tok& kt = toks_[pos_];
        if (kt.t != T_WORD) throw YangError(kt.line, "expected keyword, got '" + kt.v + "'");
        ++pos_;
        arena_.emplace_back();
        Stmt* st = &arena_.back();
        st->k = kt.v;
        st->l = kt.line;
        int kwLine = kt.line;
        if (!atEnd() && (toks_[pos_].t == T_WORD || toks_[pos_].t == T_STR)) {
            st->a = toks_[pos_].v;
            st->hasA = true;
            ++pos_;
            while (pos_ + 1 < toks_.size() && toks_[pos_].t == T_WORD && toks_[pos_].v == "+" &&
                   toks_[pos_ + 1].t == T_STR) {
                st->a += toks_[pos_ + 1].v;
                pos_ += 2;
            }
        }
        if (atEnd()) throw YangError(lastLine(), "expected ';' or '{' after '" + st->k + "', got end of file");
        const Tok& t = toks_[pos_];
        if (t.t == T_SEMI) {
            ++pos_;
        } else if (t.t == T_LBRACE) {
            ++pos_;
            while (true) {
                if (atEnd()) throw YangError(kwLine, "missing '}' for '" + st->k + "'");
                if (toks_[pos_].t == T_RBRACE) { ++pos_; break; }
                st->c.push_back(statement());
            }
        } else {
            throw YangError(t.line, "expected ';' or '{' after '" + st->k + "', got '" + t.v + "'");
        }
        return st;
    }
};

// ------------------------------------------------------------------------- //
// Helpers
// ------------------------------------------------------------------------- //

static Stmt* sub(Stmt* st, const char* kw) {
    if (!st) return nullptr;
    for (Stmt* c : st->c)
        if (c->k == kw) return c;
    return nullptr;
}

static std::vector<Stmt*> subs(Stmt* st, const char* kw) {
    std::vector<Stmt*> v;
    for (Stmt* c : st->c)
        if (c->k == kw) v.push_back(c);
    return v;
}

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + p.u8string());
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s.erase(0, 3);
    return s;
}

static std::string baseName(const fs::path& p) { return p.filename().u8string(); }

static const std::set<std::string> kHeaderKw = {"yang-version", "namespace", "prefix", "belongs-to",
                                                "import", "include", "organization", "contact",
                                                "description", "reference", "revision"};

// ------------------------------------------------------------------------- //
// Loader
// ------------------------------------------------------------------------- //

struct Source {
    std::string name;
    fs::path path;
    Stmt* root;
    std::map<std::string, std::string> prefixes;
};

struct Module {
    std::string name, prefix;
    std::vector<Source> sources;
    std::map<std::string, std::string> top;  // grouping name -> gid
};

struct Group {
    std::string m;
    Stmt* s;
};

class Loader {
   public:
    std::deque<Stmt> arena;
    std::map<std::string, std::pair<std::string, fs::path>> index;
    std::map<std::string, Module*> modules;  // nullptr = missing / failed
    std::deque<Module> store;
    std::map<std::string, Group> groups;
    std::set<std::string> missing;
    std::vector<std::string> warnings;

    explicit Loader(const std::vector<SearchDir>& dirs) {
        for (const auto& d : dirs) scan(d.dir, d.maxDepth);
    }

    Stmt* parseText(const std::string& text) {
        Parser p(arena, tokenize(text));
        return p.parseModule();
    }

    Module* loadByName(const std::string& name) {
        auto it = modules.find(name);
        if (it != modules.end()) return it->second;
        auto ix = index.find(name);
        if (ix == index.end()) {
            modules[name] = nullptr;
            missing.insert(name);
            return nullptr;
        }
        try {
            Module* m = load(readFile(ix->second.second), ix->second.second);
            modules[name] = m;
            return m;
        } catch (const std::exception& e) {
            modules[name] = nullptr;
            warnings.push_back(baseName(ix->second.second) + ": " + e.what());
            return nullptr;
        }
    }

    Module* load(const std::string& text, const fs::path& path) {
        Stmt* root = parseText(text);
        if (root->k != "module")
            throw YangError(0, "'" + root->a + "' is a " + root->k + "; open the main module instead");
        store.emplace_back();
        Module* mod = &store.back();
        mod->name = root->hasA ? root->a : "?";
        modules[mod->name] = mod;
        if (Stmt* p = sub(root, "prefix")) mod->prefix = p->a;
        mod->sources.push_back({mod->name, path, root, {}});

        // submodules (transitively)
        std::vector<Stmt*> pending{root};
        std::set<std::string> seen{mod->name};
        while (!pending.empty()) {
            Stmt* st = pending.back();
            pending.pop_back();
            for (Stmt* inc : subs(st, "include")) {
                const std::string& sname = inc->a;
                if (seen.count(sname)) continue;
                seen.insert(sname);
                auto ix = index.find(sname);
                if (ix == index.end()) { missing.insert(sname); continue; }
                Stmt* sroot = nullptr;
                try {
                    sroot = parseText(readFile(ix->second.second));
                } catch (const std::exception& e) {
                    warnings.push_back(baseName(ix->second.second) + ": " + e.what());
                    continue;
                }
                mod->sources.push_back({sname, ix->second.second, sroot, {}});
                pending.push_back(sroot);
            }
        }

        // prefix maps
        for (Source& src : mod->sources) {
            Stmt* r = src.root;
            if (r->k == "module") {
                if (!mod->prefix.empty()) src.prefixes[mod->prefix] = mod->name;
            } else {
                Stmt* bp = sub(sub(r, "belongs-to"), "prefix");
                if (bp) src.prefixes[bp->a] = mod->name;
            }
            for (Stmt* imp : subs(r, "import"))
                if (Stmt* ip = sub(imp, "prefix")) src.prefixes[ip->a] = imp->a;
        }

        // pass 1: module-wide top-level groupings
        for (Source& src : mod->sources)
            for (Stmt* st : subs(src.root, "grouping")) mod->top[st->a] = newGid(mod->name, st, src.name);

        // pass 2: resolve uses
        // (index-based: loading imports may grow `store`, but deque keeps pointers stable)
        for (size_t i = 0; i < mod->sources.size(); ++i) {
            Source& src = mod->sources[i];
            std::vector<const std::map<std::string, std::string>*> scopes;
            for (Stmt* st : src.root->c) walk(st, mod, src, scopes);
        }
        return mod;
    }

   private:
    int gidCounter_ = 0;

    void scan(const fs::path& d, int maxDepth) {
        std::error_code ec;
        if (d.empty() || !fs::is_directory(d, ec)) return;
        fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const fs::directory_entry& e = *it;
            std::string fname = e.path().filename().u8string();
            std::error_code ec2;
            if (e.is_directory(ec2)) {
                if (!fname.empty() && fname[0] == '.') { it.disable_recursion_pending(); continue; }
                if (it.depth() + 1 > maxDepth) it.disable_recursion_pending();
                continue;
            }
            if (fname.size() <= 5) continue;
            std::string ext = fname.substr(fname.size() - 5);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".yang") continue;
            std::string stem = fname.substr(0, fname.size() - 5);
            size_t at = stem.find('@');
            std::string name = stem.substr(0, at);
            std::string rev = at == std::string::npos ? "" : stem.substr(at + 1);
            auto cur = index.find(name);
            if (cur == index.end() || rev > cur->second.first) index[name] = {rev, e.path()};
        }
    }

    std::string newGid(const std::string& modname, Stmt* st, const std::string& srcname) {
        std::string gid = std::to_string(++gidCounter_);
        st->g = gid;
        st->f = srcname;
        groups[gid] = {modname, st};
        return gid;
    }

    void walk(Stmt* st, Module* mod, Source& src, std::vector<const std::map<std::string, std::string>*>& scopes) {
        if (st->c.empty()) {
            if (st->k == "uses") resolve(st, mod, src, scopes);
            return;
        }
        std::map<std::string, std::string> local;
        for (Stmt* c : st->c)
            if (c->k == "grouping" && c->g.empty()) local[c->a] = newGid(mod->name, c, src.name);
        if (st->k == "uses") resolve(st, mod, src, scopes);
        bool pushed = !local.empty();
        if (pushed) scopes.push_back(&local);
        for (Stmt* c : st->c) walk(c, mod, src, scopes);
        if (pushed) scopes.pop_back();
    }

    void resolve(Stmt* st, Module* mod, Source& src,
                 const std::vector<const std::map<std::string, std::string>*>& scopes) {
        const std::string& ref = st->a;
        std::string gid;
        size_t colon = ref.find(':');
        if (colon != std::string::npos) {
            std::string pfx = ref.substr(0, colon), name = ref.substr(colon + 1);
            auto t = src.prefixes.find(pfx);
            if (t == src.prefixes.end()) return;
            std::string target = t->second;  // copy: loading may touch maps
            if (target == mod->name) {
                auto g = mod->top.find(name);
                if (g != mod->top.end()) gid = g->second;
            } else if (Module* m = loadByName(target)) {
                auto g = m->top.find(name);
                if (g != m->top.end()) gid = g->second;
            }
        } else {
            for (auto it = scopes.rbegin(); it != scopes.rend() && gid.empty(); ++it) {
                auto g = (*it)->find(ref);
                if (g != (*it)->end()) gid = g->second;
            }
            if (gid.empty()) {
                auto g = mod->top.find(ref);
                if (g != mod->top.end()) gid = g->second;
            }
        }
        if (!gid.empty()) st->r = gid;
    }
};

// ------------------------------------------------------------------------- //
// JSON
// ------------------------------------------------------------------------- //

static void jstr(std::string& o, const std::string& s) {
    o += '"';
    char prev = 0;
    for (char ch : s) {
        unsigned char u = (unsigned char)ch;
        switch (ch) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            case '\b': o += "\\b"; break;
            case '\f': o += "\\f"; break;
            default:
                if (u < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", u);
                    o += buf;
                } else if (ch == '/' && prev == '<') {
                    o += "\\/";   // never emit "</" inside <script>
                } else {
                    o += ch;
                }
        }
        prev = ch;
    }
    o += '"';
}

static void jstmt(std::string& o, const Stmt* s, const std::vector<Stmt*>* kids = nullptr) {
    o += "{\"k\":";
    jstr(o, s->k);
    o += ",\"l\":" + std::to_string(s->l);
    if (s->hasA) { o += ",\"a\":"; jstr(o, s->a); }
    const std::vector<Stmt*>& c = kids ? *kids : s->c;
    if (!c.empty() || kids) {
        o += ",\"c\":[";
        for (size_t i = 0; i < c.size(); ++i) {
            if (i) o += ',';
            jstmt(o, c[i]);
        }
        o += ']';
    }
    if (!s->g.empty()) { o += ",\"g\":"; jstr(o, s->g); }
    if (!s->f.empty()) { o += ",\"f\":"; jstr(o, s->f); }
    if (!s->r.empty()) { o += ",\"r\":"; jstr(o, s->r); }
    o += '}';
}

template <class C>
static void jlist(std::string& o, const C& items) {
    o += '[';
    bool first = true;
    for (const auto& x : items) {
        if (!first) o += ',';
        first = false;
        jstr(o, x);
    }
    o += ']';
}

// ------------------------------------------------------------------------- //
// NETCONF support data ("nc")
// ------------------------------------------------------------------------- //

static const std::set<std::string> kSchemaKw = {"container", "list",   "leaf",   "leaf-list",    "choice",
                                                "case",      "anydata", "anyxml", "rpc",          "action",
                                                "notification", "input", "output"};

struct NodeAt {
    Stmt* s = nullptr;
    std::string src;  // source (module or submodule) where the statement is written
};

// Children of a schema node as seen in the data tree: 'uses' replaced by the grouping.
static void schemaChildren(Loader& L, const NodeAt& n, std::vector<NodeAt>& out, int depth = 0) {
    if (!n.s || depth > 40) return;
    for (Stmt* c : n.s->c) {
        if (c->k == "uses") {
            auto g = L.groups.find(c->r);
            if (g != L.groups.end()) schemaChildren(L, {g->second.s, g->second.s->f}, out, depth + 1);
        } else if (kSchemaKw.count(c->k)) {
            out.push_back({c, n.src});
        }
    }
}

// Key leaves of a list (they may come from a grouping or sit under nothing else).
static void listKeys(Loader& L, const NodeAt& list, std::string& o) {
    o += ",\"keys\":[";
    Stmt* key = sub(list.s, "key");
    bool first = true;
    if (key) {
        std::vector<NodeAt> kids;
        schemaChildren(L, list, kids);
        std::string names = key->a;
        for (size_t p = 0; p < names.size();) {
            size_t q = names.find_first_of(" \t\n", p);
            if (q == std::string::npos) q = names.size();
            std::string kn = names.substr(p, q - p);
            p = q + 1;
            if (kn.empty()) continue;
            size_t colon = kn.find(':');
            if (colon != std::string::npos) kn = kn.substr(colon + 1);
            for (auto& k : kids)
                if (k.s->k == "leaf" && k.s->a == kn) {
                    if (!first) o += ',';
                    first = false;
                    o += "{\"n\":";
                    jstr(o, kn);
                    o += ",\"f\":";
                    jstr(o, k.src);
                    o += ",\"s\":";
                    jstmt(o, k.s);
                    o += '}';
                    break;
                }
        }
    }
    o += ']';
}

static void ncPayload(Loader& L, Module* mod, const std::vector<Stmt*>& kids, std::string& o) {
    // Imports are normally loaded lazily (only for groupings); types, identities and
    // augment targets need all of them, transitively.
    {
        std::vector<std::string> todo;
        std::set<std::string> seen;
        auto addImports = [&](Module* m) {
            for (Source& s : m->sources)
                for (auto& p : s.prefixes)
                    if (seen.insert(p.second).second) todo.push_back(p.second);
        };
        seen.insert(mod->name);
        addImports(mod);
        while (!todo.empty()) {
            std::string name = todo.back();
            todo.pop_back();
            if (Module* m = L.loadByName(name)) addImports(m);
        }
    }
    o += ",\"nc\":{\"mods\":{";
    bool first = true;
    for (auto& m : L.modules) {
        if (!m.second) continue;
        Stmt* ns = sub(m.second->sources[0].root, "namespace");
        if (!first) o += ',';
        first = false;
        jstr(o, m.first);
        o += ":{\"ns\":";
        jstr(o, ns ? ns->a : "");
        o += ",\"prefix\":";
        jstr(o, m.second->prefix);
        o += '}';
    }
    // source -> owning module and prefix map (to resolve "pfx:name" where it is written)
    o += "},\"src\":{";
    first = true;
    for (auto& m : L.modules) {
        if (!m.second) continue;
        for (Source& s : m.second->sources) {
            if (!first) o += ',';
            first = false;
            jstr(o, s.name);
            o += ":{\"m\":";
            jstr(o, m.first);
            o += ",\"p\":{";
            bool f2 = true;
            for (auto& p : s.prefixes) {
                if (!f2) o += ',';
                f2 = false;
                jstr(o, p.first);
                o += ':';
                jstr(o, p.second);
            }
            o += "}}";
        }
    }
    // top-level typedefs of every loaded module: "module:name" -> {f, s}
    o += "},\"typedefs\":{";
    first = true;
    for (auto& m : L.modules) {
        if (!m.second) continue;
        for (Source& s : m.second->sources)
            for (Stmt* t : subs(s.root, "typedef")) {
                if (!first) o += ',';
                first = false;
                jstr(o, m.first + ":" + t->a);
                o += ":{\"f\":";
                jstr(o, s.name);
                o += ",\"s\":";
                jstmt(o, t);
                o += '}';
            }
    }
    // identities with resolved bases ("module:name")
    o += "},\"ids\":[";
    first = true;
    for (auto& m : L.modules) {
        if (!m.second) continue;
        for (Source& s : m.second->sources)
            for (Stmt* id : subs(s.root, "identity")) {
                if (!first) o += ',';
                first = false;
                o += "{\"m\":";
                jstr(o, m.first);
                o += ",\"a\":";
                jstr(o, id->a);
                o += ",\"b\":[";
                bool f2 = true;
                for (Stmt* b : subs(id, "base")) {
                    std::string bm = m.first, bn = b->a;
                    size_t c = bn.find(':');
                    if (c != std::string::npos) {
                        auto p = s.prefixes.find(bn.substr(0, c));
                        if (p != s.prefixes.end()) bm = p->second;
                        bn = bn.substr(c + 1);
                    }
                    if (!f2) o += ',';
                    f2 = false;
                    jstr(o, bm + ":" + bn);
                }
                o += "]}";
            }
    }
    // augment targets: each step's module, kind, config false, list keys
    o += "],\"aug\":{";
    first = true;
    std::set<std::string> done;
    for (Stmt* a : kids) {
        if (a->k != "augment" || !done.insert(a->a).second) continue;
        std::string srcName = a->f.empty() ? mod->name : a->f;
        const std::map<std::string, std::string>* prefixes = nullptr;
        for (Source& s : mod->sources)
            if (s.name == srcName) prefixes = &s.prefixes;
        if (!first) o += ',';
        first = false;
        jstr(o, a->a);
        o += ":[";
        std::vector<NodeAt> level;  // candidates at the current depth
        bool firstStep = true, lost = false;
        std::string path = a->a;
        for (size_t p = 0; p < path.size() && !lost;) {
            while (p < path.size() && (path[p] == '/' || path[p] == ' ')) ++p;
            size_t q = path.find('/', p);
            if (q == std::string::npos) q = path.size();
            std::string step = path.substr(p, q - p);
            p = q;
            while (!step.empty() && step.back() == ' ') step.pop_back();
            if (step.empty()) continue;
            std::string sm = mod->name, sn = step;
            size_t c = step.find(':');
            if (c != std::string::npos) {
                if (prefixes) {
                    auto it = prefixes->find(step.substr(0, c));
                    if (it != prefixes->end()) sm = it->second;
                }
                sn = step.substr(c + 1);
            }
            if (firstStep) {  // top level of the target module (all its sources)
                auto mit = L.modules.find(sm);
                if (mit != L.modules.end() && mit->second)
                    for (Source& s : mit->second->sources) schemaChildren(L, {s.root, s.name}, level);
            }
            NodeAt hit;
            for (auto& n : level)
                if (n.s->a == sn) { hit = n; break; }
            if (!firstStep) o += ',';
            firstStep = false;
            o += "{\"n\":";
            jstr(o, sn);
            o += ",\"m\":";
            jstr(o, sm);
            if (!hit.s) {
                o += ",\"missing\":true}";
                lost = true;
                break;
            }
            o += ",\"k\":";
            jstr(o, hit.s->k);
            Stmt* cf = sub(hit.s, "config");
            if (cf && cf->a == "false") o += ",\"ro\":true";
            if (hit.s->k == "list") listKeys(L, hit, o);
            o += '}';
            level.clear();
            schemaChildren(L, hit, level);
        }
        o += ']';
    }
    o += "}}";
}

static std::string buildPayload(Loader& L, Module* mod, const fs::path& input, bool includePaths,
                                bool includeNetconf) {
    Source& main = mod->sources[0];
    std::vector<Stmt*> kids = main.root->c;
    std::map<std::string, std::string> files;
    std::map<std::string, fs::path> paths;  // absolute paths, only with includePaths
    files[mod->name] = baseName(main.path.empty() ? input : main.path);
    paths[mod->name] = main.path.empty() ? input : main.path;
    for (size_t i = 1; i < mod->sources.size(); ++i) {
        Source& src = mod->sources[i];
        files[src.name] = baseName(src.path);
        paths[src.name] = src.path;
        for (Stmt* st : src.root->c) {
            if (kHeaderKw.count(st->k)) continue;
            st->f = src.name;
            kids.push_back(st);
        }
    }

    std::set<std::string> localGids;
    std::function<void(const Stmt*, std::set<std::string>&)> collect = [&](const Stmt* st, std::set<std::string>& acc) {
        if (!st->g.empty()) localGids.insert(st->g);
        if (!st->r.empty()) acc.insert(st->r);
        for (const Stmt* c : st->c) collect(c, acc);
    };
    std::set<std::string> refs;
    for (Stmt* k : kids) collect(k, refs);
    collect(main.root, refs);  // header statements (none hold uses, harmless)

    std::map<std::string, Group> outGroups;
    std::vector<std::string> todo;
    for (const auto& g : refs)
        if (!localGids.count(g)) todo.push_back(g);
    while (!todo.empty()) {
        std::string gid = todo.back();
        todo.pop_back();
        if (outGroups.count(gid) || localGids.count(gid)) continue;
        auto git = L.groups.find(gid);
        if (git == L.groups.end()) continue;
        outGroups[gid] = git->second;
        std::set<std::string> more;
        collect(git->second.s, more);
        for (const auto& g : more)
            if (!outGroups.count(g) && !localGids.count(g)) todo.push_back(g);
        auto mit = L.modules.find(git->second.m);
        if (mit != L.modules.end() && mit->second)
            for (Source& s : mit->second->sources) {
                files.emplace(s.name, baseName(s.path));
                paths.emplace(s.name, s.path);
            }
    }

    std::set<std::string> unresolved;
    std::function<void(const Stmt*)> findUnres = [&](const Stmt* st) {
        if (st->k == "uses" && st->r.empty() && !st->a.empty()) unresolved.insert(st->a);
        for (const Stmt* c : st->c) findUnres(c);
    };
    for (Stmt* k : kids) findUnres(k);
    for (auto& g : outGroups) findUnres(g.second.s);

    std::string o;
    o.reserve(1 << 20);
    o += "{\"module\":";
    jstr(o, mod->name);
    o += ",\"local\":[";
    for (size_t i = 0; i < mod->sources.size(); ++i) {
        if (i) o += ',';
        jstr(o, mod->sources[i].name);
    }
    o += "],\"prefix\":";
    if (mod->prefix.empty()) o += "null"; else jstr(o, mod->prefix);
    o += ",\"root\":";
    jstmt(o, main.root, &kids);
    o += ",\"groups\":{";
    bool first = true;
    for (auto& g : outGroups) {
        if (!first) o += ',';
        first = false;
        jstr(o, g.first);
        o += ":{\"m\":";
        jstr(o, g.second.m);
        o += ",\"s\":";
        jstmt(o, g.second.s);
        o += '}';
    }
    o += "},\"files\":{";
    first = true;
    for (auto& f : files) {
        if (!first) o += ',';
        first = false;
        jstr(o, f.first);
        o += ':';
        jstr(o, f.second);
    }
    o += '}';
    if (includePaths) {
        o += ",\"paths\":{";
        first = true;
        for (auto& p : paths) {
            if (!p.second.is_absolute()) continue;  // e.g. an unsaved "new 1" tab
            if (!first) o += ',';
            first = false;
            jstr(o, p.first);
            o += ':';
            jstr(o, p.second.lexically_normal().u8string());
        }
        o += '}';
    }
    if (includeNetconf) ncPayload(L, mod, kids, o);
    o += ",\"missing\":";
    jlist(o, L.missing);
    o += ",\"unresolved\":";
    jlist(o, unresolved);
    o += ",\"warnings\":";
    jlist(o, L.warnings);
    o += '}';
    return o;
}

// ------------------------------------------------------------------------- //
// HTML
// ------------------------------------------------------------------------- //

static std::string htmlEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#x27;"; break;
            default: o += c;
        }
    }
    return o;
}

static std::string templateText() {
    std::string t;
    for (const char* part : kTemplateParts) t += part;
    return t;
}

static void replaceFirst(std::string& s, const std::string& from, const std::string& to) {
    size_t p = s.find(from);
    if (p != std::string::npos) s.replace(p, from.size(), to);
}

std::string errorHtml(const std::string& msg) {
    return "<!DOCTYPE html><meta charset='utf-8'><title>yang2html error</title>"
           "<body style='font:14px Segoe UI,sans-serif;padding:24px'>"
           "<h2 style='color:#c00'>yang2html could not render this file</h2>"
           "<pre style='white-space:pre-wrap;background:#f5f5f5;padding:12px'>" +
           htmlEscape(msg) + "</pre>";
}

RenderResult render(const std::string& textIn, const fs::path& sourcePath, const std::vector<SearchDir>& dirs,
                    const RenderOptions& opts) {
    RenderResult res;
    try {
        std::string text = textIn;
        if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
            (unsigned char)text[2] == 0xBF)
            text.erase(0, 3);
        Loader L(dirs);
        Module* mod = L.load(text, sourcePath);
        res.json = buildPayload(L, mod, sourcePath, opts.includePaths, opts.includeNetconf);
        res.module = mod->name;
        std::string page = templateText();
        // DATA is inserted last so payload text can never be mistaken for a marker
        size_t dpos = page.find("{{DATA}}");
        std::string head = page.substr(0, dpos), tail = dpos == std::string::npos ? "" : page.substr(dpos + 8);
        replaceFirst(head, "{{TITLE}}", htmlEscape(mod->name + " - YANG tree"));
        res.html = head + res.json + tail;
        res.ok = true;
    } catch (const YangError& e) {
        res.error = e.what();
        res.errorLine = e.line;
    } catch (const std::exception& e) {
        res.error = e.what();
    }
    return res;
}

}  // namespace yang2html
