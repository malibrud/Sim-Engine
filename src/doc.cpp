#include "doc.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <sstream>

#include "ast.hpp"
#include "parser.hpp"

namespace se {
namespace docgen {

namespace fs = std::filesystem;

// §6.14 — the sections a node or type doc may carry, in the order they must
// appear. Anything else after a `# ` is an unknown heading.
const char* const kSections[] = {"Conventions", "Initial conditions", "Accuracy",
                                 "Limitations", "Design notes",       "See also"};
constexpr int kSectionCount = sizeof(kSections) / sizeof(kSections[0]);

// ─── Spec index ──────────────────────────────────────────────────────────────

struct SpecIndex {
    bool loaded = false;
    std::string path;                          // as given, for links
    std::map<std::string, std::string> anchor; // "6.4a" -> "64a-array-shaped-states"
    std::set<std::string> codes;               // "SE0410"
};

// GitHub's heading slug: lowercase, keep ASCII letters, digits, `-` and `_`,
// spaces become `-`, everything else (punctuation, non-ASCII) is dropped.
std::string slug(const std::string& heading) {
    std::string s;
    for (unsigned char c : heading) {
        if (std::isalnum(c) && c < 0x80) s += static_cast<char>(std::tolower(c));
        else if (c == ' ') s += '-';
        else if (c == '-' || c == '_') s += static_cast<char>(c);
    }
    return s;
}

SpecIndex load_spec(const std::string& path) {
    SpecIndex ix;
    Source src;
    std::string error;
    if (path.empty() || !Source::load(path, src, error)) return ix;
    ix.loaded = true;
    ix.path = path;
    std::map<std::string, int> seen;
    for (std::uint32_t n = 1; n <= src.line_count(); ++n) {
        const std::string line(src.line(n));
        if (line.rfind("| `SE", 0) == 0 && line.size() >= 10)
            ix.codes.insert(line.substr(3, 6));
        if (line.empty() || line[0] != '#') continue;
        std::size_t i = 0;
        while (i < line.size() && line[i] == '#') ++i;
        if (i >= line.size() || line[i] != ' ') continue;
        const std::string text = line.substr(i + 1);
        std::string a = slug(text);
        const int k = seen[a]++;
        if (k > 0) a += "-" + std::to_string(k);
        // The section number: `4.`, `4.5`, `6.2a`, the leading token only.
        std::size_t j = 0;
        while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) ||
                                   text[j] == '.' ||
                                   (j > 0 && std::islower(static_cast<unsigned char>(text[j])) &&
                                    std::isdigit(static_cast<unsigned char>(text[j - 1])))))
            ++j;
        if (j == 0 || j >= text.size() || text[j] != ' ') continue;
        std::string key = text.substr(0, j);
        if (key.back() == '.') key.pop_back();
        ix.anchor.emplace(key, a);
    }
    return ix;
}

// ─── Source slicing ──────────────────────────────────────────────────────────
//  Types, defaults and test cells are shown exactly as written, so a reader
//  sees `math.Vec3(U/s^2)` and `{ 0.0, 0.0, 0.0 }` rather than a re-rendering.

// From `from` to the `stop` character at bracket depth 0, skipping strings and
// comments, with runs of whitespace collapsed to one space.
std::string slice_to(const std::string& t, std::size_t from, char stop) {
    std::string out;
    int depth = 0;
    bool space = false;
    for (std::size_t i = from; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') {
            while (i < t.size() && t[i] != '\n') ++i;
            space = true;
            continue;
        }
        if (c == '/' && i + 1 < t.size() && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < t.size() && !(t[i] == '*' && t[i + 1] == '/')) ++i;
            ++i;
            space = true;
            continue;
        }
        if (c == '"') {
            if (space && !out.empty()) out += ' ';
            space = false;
            out += c;
            for (++i; i < t.size() && t[i] != '"'; ++i) out += t[i];
            out += '"';
            continue;
        }
        if (depth == 0 && c == stop) break;
        if (c == '(' || c == '{' || c == '[') ++depth;
        if (c == ')' || c == '}' || c == ']') --depth;
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

// Split at `sep` on bracket depth 0, outside strings; each part trimmed.
std::vector<std::string> split_top(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    bool str = false;
    for (char c : s) {
        if (str) {
            cur += c;
            if (c == '"') str = false;
            continue;
        }
        if (c == '"') str = true;
        if (c == '(' || c == '{' || c == '[') ++depth;
        if (c == ')' || c == '}' || c == ']') --depth;
        if (c == sep && depth == 0) {
            parts.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    parts.push_back(cur);
    for (std::string& p : parts) {
        while (!p.empty() && p.front() == ' ') p.erase(p.begin());
        while (!p.empty() && p.back() == ' ') p.pop_back();
    }
    return parts;
}

// `name: TYPE = DEFAULT` -> {TYPE, DEFAULT}; the default may be absent.
std::pair<std::string, std::string> type_and_value(const std::string& decl) {
    const std::size_t colon = decl.find(':');
    if (colon == std::string::npos) return {"", ""};
    const std::vector<std::string> p = split_top(decl.substr(colon + 1), '=');
    return {p[0], p.size() > 1 ? p[1] : std::string()};
}

// ─── Attachment ──────────────────────────────────────────────────────────────

struct Doc {
    std::vector<std::string> lines;
    std::vector<Loc> locs;   // one per line, for diagnostics
    bool present() const { return !lines.empty(); }
};

struct Decl {
    std::uint32_t line = 0;
    const void* key = nullptr;
    Loc loc;
    std::string what;        // "node `X`", "input `a`"
    bool required = false;
    bool is_definition = false;
};

// The `///` marker, for a comment that attaches to nothing.
Loc doc_loc(const DocLine& d) {
    Loc l;
    l.line = d.line;
    l.col = d.col;
    l.offset = d.offset;
    l.length = 3;
    return l;
}

// The text after the marker, for a complaint about what the comment says.
Loc text_loc(const DocLine& d) {
    Loc l;
    l.line = d.line;
    l.col = d.text_col;
    l.offset = d.offset + (d.text_col - d.col);
    l.length = d.text.empty() ? 1 : static_cast<std::uint32_t>(d.text.size());
    return l;
}

// `len` bytes starting `at` bytes into a doc line's text.
Loc within(Loc l, std::size_t at, std::size_t len) {
    l.col += static_cast<std::uint32_t>(at);
    l.offset += static_cast<std::uint32_t>(at);
    l.length = static_cast<std::uint32_t>(len);
    return l;
}

struct FileDocs {
    const ast::ModelFile* file = nullptr;
    const Source* src = nullptr;
    std::string path;
    std::string stem;
    std::map<const void*, Doc> docs;
};

void collect_decls(const ast::ModelFile& f, const std::string& stem, std::vector<Decl>& out) {
    auto add = [&](std::uint32_t line, const void* key, Loc loc, std::string what, bool req,
                   bool def = false) {
        Decl d;
        d.line = line;
        d.key = key;
        d.loc = loc;
        d.what = std::move(what);
        d.required = req;
        d.is_definition = def;
        out.push_back(std::move(d));
    };
    for (const ast::TypeDef& t : f.types) {
        const bool pub = t.name == stem;
        add(t.loc.line, &t, t.loc, "type `" + t.name + "`", pub, true);
        for (const ast::UnitParamDecl& u : t.unit_params)
            add(u.loc.line, &u, u.loc, "unit parameter `" + u.name + "`", false);
        for (const ast::FieldDecl& x : t.fields)
            add(x.loc.line, &x, x.loc, "field `" + x.name + "`", pub);
    }
    for (const ast::NodeDef& n : f.nodes) {
        const bool pub = n.name == stem;
        add(n.loc.line, &n, n.loc, "node `" + n.name + "`", pub, true);
        for (const ast::UnitParamDecl& u : n.unit_params)
            add(u.loc.line, &u, u.loc, "unit parameter `" + u.name + "`", false);
        for (const ast::SettingDecl& s : n.settings)
            add(s.loc.line, &s, s.loc, "setting `" + s.name + "`", pub);
        for (const ast::FieldDecl& x : n.inputs)
            add(x.loc.line, &x, x.loc, "input `" + x.name + "`", pub);
        for (const ast::FieldDecl& x : n.outputs)
            add(x.loc.line, &x, x.loc, "output `" + x.name + "`", pub);
        for (const ast::StateDecl& s : n.states) {
            const Loc at = s.kind_loc.valid() ? s.kind_loc : s.loc;
            add(at.line, &s, at, "state `" + s.name + "`", pub);
        }
        for (const ast::FieldDecl& x : n.vars)
            add(x.loc.line, &x, x.loc, "var `" + x.name + "`", false);
        for (const ast::NativeDecl& x : n.natives)
            add(x.loc.line, &x, x.loc, "native `" + x.name + "`", false);
        // §6.9.3 — documentable so `///` above one is not SE0701, but never
        // required and never on a reference page: a group is internal layout,
        // and its comment is for the topology diagram.
        for (const ast::Group& g : n.structure.groups)
            add(g.loc.line, &g, g.loc, "group \"" + g.label + "\"", false);
    }
}

// Attach every `///` line to its declaration, and report the ones that have
// none (SE0701) or more than one candidate (SE0703).
void attach(FileDocs& fd, Diagnostics& diag) {
    std::vector<Decl> decls;
    collect_decls(*fd.file, fd.stem, decls);
    std::map<std::uint32_t, std::vector<const Decl*>> by_line;
    for (const Decl& d : decls) by_line[d.line].push_back(&d);

    const std::vector<DocLine>& lines = fd.file->doc_lines;
    auto target = [&](std::uint32_t line, const DocLine& first, bool trailing) -> const Decl* {
        const auto it = by_line.find(line);
        if (it == by_line.end()) {
            diag.error("SE0701", *fd.src, doc_loc(first), "doc comment documents nothing",
                       trailing ? "no declaration starts on this line"
                                : "no declaration on the line below",
                       {note("a `///` block documents the declaration directly below it; a "
                             "trailing `///` documents the declaration on its own line "
                             "(\xc2\xa7""3.2)"),
                        help("use `//` for a comment that is not documentation")});
            return nullptr;
        }
        if (it->second.size() > 1) {
            diag.error("SE0703", *fd.src, doc_loc(first),
                       "doc comment is ambiguous: " + std::to_string(it->second.size()) +
                           " declarations start on that line",
                       "", {help("put each documented declaration on a line of its own")});
            return nullptr;
        }
        return it->second.front();
    };

    for (std::size_t i = 0; i < lines.size();) {
        const DocLine& d = lines[i];
        if (d.trailing) {
            if (const Decl* t = target(d.line, d, true)) {
                Doc& doc = fd.docs[t->key];
                doc.lines.push_back(d.text);
                doc.locs.push_back(text_loc(d));
            }
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        while (j < lines.size() && !lines[j].trailing && lines[j].line == lines[j - 1].line + 1)
            ++j;
        if (const Decl* t = target(lines[j - 1].line + 1, d, false)) {
            Doc& doc = fd.docs[t->key];
            // A leading block comes before any trailing text on the same item.
            std::vector<std::string> l;
            std::vector<Loc> c;
            for (std::size_t k = i; k < j; ++k) {
                l.push_back(lines[k].text);
                c.push_back(text_loc(lines[k]));
            }
            l.insert(l.end(), doc.lines.begin(), doc.lines.end());
            c.insert(c.end(), doc.locs.begin(), doc.locs.end());
            doc.lines = std::move(l);
            doc.locs = std::move(c);
        }
        i = j;
    }

    for (const Decl& d : decls) {
        if (!d.required || fd.docs.count(d.key)) continue;
        Loc l = d.loc;
        if (l.length == 0) l.length = 1;
        diag.warning("SE0702", *fd.src, l, "public " + d.what + " has no doc comment", "",
                     {help(d.is_definition
                               ? "add a `///` block directly above, starting with a "
                                 "one-sentence summary (\xc2\xa7""6.14)"
                               : "add a trailing `/// …` fragment on this line (\xc2\xa7""6.14)")});
    }
}

// ─── Checks on the prose ─────────────────────────────────────────────────────

bool is_heading(const std::string& s) { return s.size() > 2 && s[0] == '#' && s[1] == ' '; }

// Each `§n.n` and `SEnnnn` must resolve against the spec (SE0707, SE0708).
void check_citations(const Doc& doc, const SpecIndex& spec, const FileDocs& fd,
                     Diagnostics& diag) {
    if (!spec.loaded) return;
    for (std::size_t n = 0; n < doc.lines.size(); ++n) {
        const std::string& s = doc.lines[n];
        for (std::size_t i = 0; i + 1 < s.size(); ++i) {
            if (static_cast<unsigned char>(s[i]) == 0xc2 &&
                static_cast<unsigned char>(s[i + 1]) == 0xa7) {
                std::size_t j = i + 2;
                if (j < s.size() && s[j] == ' ') ++j;
                std::string key;
                while (j < s.size()) {
                    const char c = s[j];
                    if (std::isdigit(static_cast<unsigned char>(c))) key += c;
                    else if (c == '.' && j + 1 < s.size() &&
                             std::isdigit(static_cast<unsigned char>(s[j + 1])))
                        key += c;
                    else if (std::islower(static_cast<unsigned char>(c)) && !key.empty() &&
                             std::isdigit(static_cast<unsigned char>(key.back())))
                        key += c;
                    else break;
                    ++j;
                }
                if (!key.empty() && !spec.anchor.count(key)) {
                    diag.error("SE0707", *fd.src, within(doc.locs[n], i, j - i),
                               "`\xc2\xa7" + key + "` names no section of the specification",
                               "", {note("checked against " + spec.path)});
                }
            }
            if (s.compare(i, 2, "SE") == 0 && i + 6 <= s.size() &&
                std::all_of(s.begin() + i + 2, s.begin() + i + 6,
                            [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }) &&
                (i == 0 || !std::isalnum(static_cast<unsigned char>(s[i - 1])))) {
                const std::string code = s.substr(i, 6);
                if (!spec.codes.count(code))
                    diag.error("SE0708", *fd.src, within(doc.locs[n], i, 6),
                               "`" + code + "` is not in the diagnostic catalogue", "",
                               {note("checked against " + spec.path + " \xc2\xa7""16.4")});
            }
        }
    }
}

// A definition's doc: a summary paragraph ending in a period (SE0704), and
// only the §6.14 section headings, in order (SE0705, SE0706).
void check_definition(const Doc& doc, const FileDocs& fd, Diagnostics& diag) {
    std::size_t end = 0;
    while (end < doc.lines.size() && !doc.lines[end].empty() && !is_heading(doc.lines[end]))
        ++end;
    if (end == 0 || doc.lines[end - 1].back() != '.') {
        diag.error("SE0704", *fd.src, doc.locs[end == 0 ? 0 : end - 1],
                   end == 0 ? "doc comment does not open with a summary"
                            : "the summary does not end with a period",
                   "", {note("the first paragraph is the summary: one sentence, shown in "
                             "the package index (\xc2\xa7""6.14)")});
    }
    int last = -1;
    for (std::size_t n = 0; n < doc.lines.size(); ++n) {
        if (!is_heading(doc.lines[n])) continue;
        const std::string name = doc.lines[n].substr(2);
        int k = -1;
        for (int s = 0; s < kSectionCount; ++s)
            if (name == kSections[s]) k = s;
        std::string list;
        for (int s = 0; s < kSectionCount; ++s) list += (s ? ", " : "") + std::string(kSections[s]);
        if (k < 0) {
            diag.error("SE0705", *fd.src, doc.locs[n], "unknown doc section `" + name + "`", "",
                       {note("the sections are, in order: " + list + " (\xc2\xa7""6.14)")});
        } else if (k <= last) {
            diag.error("SE0706", *fd.src, doc.locs[n],
                       "doc section `" + name + "` is out of order or repeated", "",
                       {note("the sections are, in order: " + list + " (\xc2\xa7""6.14)")});
        } else {
            last = k;
        }
    }
}

// ─── Markdown ────────────────────────────────────────────────────────────────

std::string cell(std::string s) {
    std::string out;
    for (char c : s) {
        if (c == '|') out += "\\|";
        else out += c;
    }
    return out;
}

std::string code(const std::string& s) { return s.empty() ? "" : "`" + cell(s) + "`"; }

// Prose with `§n.n` turned into a link to the spec heading. Code spans and
// indented code blocks are left alone.
std::string linkify(const std::string& line, const SpecIndex& spec, const std::string& spec_rel) {
    if (!spec.loaded || line.rfind("    ", 0) == 0) return line;
    std::string out;
    bool in_code = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '`') in_code = !in_code;
        if (!in_code && i + 1 < line.size() && static_cast<unsigned char>(c) == 0xc2 &&
            static_cast<unsigned char>(line[i + 1]) == 0xa7) {
            std::size_t j = i + 2;
            const bool sp = j < line.size() && line[j] == ' ';
            if (sp) ++j;
            std::string key;
            while (j < line.size()) {
                const char d = line[j];
                if (std::isdigit(static_cast<unsigned char>(d))) key += d;
                else if (d == '.' && j + 1 < line.size() &&
                         std::isdigit(static_cast<unsigned char>(line[j + 1])))
                    key += d;
                else if (std::islower(static_cast<unsigned char>(d)) && !key.empty() &&
                         std::isdigit(static_cast<unsigned char>(key.back())))
                    key += d;
                else break;
                ++j;
            }
            const auto a = spec.anchor.find(key);
            if (!key.empty() && a != spec.anchor.end()) {
                out += "[\xc2\xa7" + key + "](" + spec_rel + "#" + a->second + ")";
                i = j - 1;
                continue;
            }
        }
        out += c;
    }
    return out;
}

// The prose of a definition, in two parts: the summary and body, which open
// the entry, and the `# X` sections, which follow the signature tables as
// `### X`. Blank lines at either end of a part are dropped.
void write_prose(std::ostream& o, const std::map<const void*, Doc>& docs, const void* key,
                 bool sections, const SpecIndex& spec, const std::string& spec_rel) {
    const auto it = docs.find(key);
    if (it == docs.end()) return;
    const std::vector<std::string>& lines = it->second.lines;
    std::size_t first = 0;
    while (first < lines.size() && !is_heading(lines[first])) ++first;
    std::size_t b = sections ? first : 0;
    std::size_t e = sections ? lines.size() : first;
    while (b < e && lines[b].empty()) ++b;
    while (e > b && lines[e - 1].empty()) --e;
    if (b == e) return;
    bool blank = true;
    for (std::size_t i = b; i < e; ++i) {
        const std::string& l = lines[i];
        if (is_heading(l)) {
            if (!blank) o << "\n";
            o << "### " << l.substr(2) << "\n\n";
            blank = true;
            while (i + 1 < e && lines[i + 1].empty()) ++i;
            continue;
        }
        o << linkify(l, spec, spec_rel) << "\n";
        blank = l.empty();
    }
    o << "\n";
}

std::string fragment(const std::map<const void*, Doc>& docs, const void* key,
                     const SpecIndex& spec, const std::string& spec_rel) {
    const auto it = docs.find(key);
    if (it == docs.end()) return "";
    std::string s;
    for (const std::string& l : it->second.lines) {
        if (!s.empty()) s += ' ';
        s += l;
    }
    return cell(linkify(s, spec, spec_rel));
}

std::string summary(const std::map<const void*, Doc>& docs, const void* key) {
    const auto it = docs.find(key);
    if (it == docs.end()) return "";
    std::string s;
    for (const std::string& l : it->second.lines) {
        if (l.empty() || is_heading(l)) break;
        if (!s.empty()) s += ' ';
        s += l;
    }
    return cell(s);
}

std::string anchor_of(const std::string& name) { return slug(name); }

void write_tests(std::ostream& o, const ast::NodeDef& n, const std::string& t) {
    const ast::TestsSection& ts = n.tests;
    if (ts.sets.empty()) return;
    o << "### Examples\n\nFrom the node's `tests` section";
    if (!ts.units.empty()) {
        o << ", with ";
        for (std::size_t i = 0; i < ts.units.size(); ++i)
            o << (i ? ", " : "") << code(slice_to(t, ts.units[i].loc.offset, ';'));
    }
    o << ". Each row is one evaluation; the last column names the case.\n\n";
    for (const ast::TestSet& s : ts.sets) {
        o << "**" << cell(s.claim) << "**";
        if (s.approx) {
            const std::string head = slice_to(t, s.op_loc.offset, '{');
            const std::size_t w = head.find("within ");
            if (w != std::string::npos) o << " (to within " << code(head.substr(w + 7)) << ")";
        } else {
            o << " (exactly)";
        }
        o << "\n\n|";
        for (const ast::TestColumn& c : s.given) o << " " << code(c.str()) << " |";
        for (const ast::TestColumn& c : s.expected) o << " \xe2\x86\x92 " << code(c.str()) << " |";
        o << " Case |\n|";
        for (std::size_t i = 0; i < s.given.size() + s.expected.size() + 1; ++i) o << "---|";
        o << "\n";
        for (const ast::TestRow& r : s.rows) {
            std::vector<std::string> cells = split_top(slice_to(t, r.loc.offset, ';'), ',');
            o << "|";
            const std::size_t ncol = s.given.size() + s.expected.size();
            for (std::size_t i = 0; i < ncol; ++i)
                o << " " << (i < cells.size() ? code(cells[i]) : std::string()) << " |";
            o << " " << cell(r.label) << " |\n";
        }
        o << "\n";
    }
}

void write_node(std::ostream& o, const FileDocs& fd, const ast::NodeDef& n, const std::string& pkg,
                const SpecIndex& spec, const std::string& spec_rel, const std::string& src_rel) {
    const std::string& t = fd.src->text();
    const auto& docs = fd.docs;
    auto frag = [&](const void* k) { return fragment(docs, k, spec, spec_rel); };

    o << "## " << n.name << "\n\n";
    o << "`node " << pkg << "." << n.name;
    if (!n.unit_params.empty()) {
        o << " (";
        for (std::size_t i = 0; i < n.unit_params.size(); ++i)
            o << (i ? ", " : "") << n.unit_params[i].name;
        o << ")";
    }
    o << "` \xc2\xb7 " << (n.is_composite() ? "composite" : "leaf") << " \xc2\xb7 [source]("
      << src_rel << ")\n\n";

    write_prose(o, docs, &n, false, spec, spec_rel);

    if (!n.unit_params.empty()) {
        o << "### Unit parameters\n\n| Name | Description |\n|---|---|\n";
        for (const ast::UnitParamDecl& u : n.unit_params)
            o << "| " << code(u.name) << " | " << frag(&u) << " |\n";
        o << "\n";
    }
    if (!n.settings.empty()) {
        o << "### Settings\n\n| Name | Type | Default | Description |\n|---|---|---|---|\n";
        for (const ast::SettingDecl& s : n.settings) {
            const auto tv = type_and_value(slice_to(t, s.loc.offset, ';'));
            o << "| " << code(s.name) << " | " << code(tv.first) << " | "
              << (tv.second.empty() ? "*required*" : code(tv.second)) << " | " << frag(&s) << " |\n";
        }
        o << "\n";
    }
    auto ports = [&](const char* title, const std::vector<ast::FieldDecl>& v) {
        if (v.empty()) return;
        o << "### " << title << "\n\n| Name | Type | Description |\n|---|---|---|\n";
        for (const ast::FieldDecl& x : v) {
            const auto tv = type_and_value(slice_to(t, x.loc.offset, ';'));
            o << "| " << code(x.name) << " | " << code(tv.first) << " | " << frag(&x) << " |\n";
        }
        o << "\n";
    };
    ports("Inputs", n.inputs);
    ports("Outputs", n.outputs);
    if (!n.states.empty()) {
        o << "### States\n\n| Name | Kind | Type | Initial | Description |\n|---|---|---|---|---|\n";
        for (const ast::StateDecl& s : n.states) {
            const auto tv = type_and_value(slice_to(t, s.loc.offset, ';'));
            o << "| " << code(s.name) << " | " << (s.is_continuous ? "continuous" : "discrete")
              << " | " << code(tv.first) << " | " << code(tv.second) << " | " << frag(&s) << " |\n";
        }
        o << "\n";
    }
    if (!n.is_composite()) {
        for (const ast::Method& m : n.methods) {
            if (m.which != ast::Method::Which::Output) continue;
            o << "**Direct feedthrough:** ";
            if (m.params.empty()) o << "none; `output()` reads no inputs.";
            for (std::size_t i = 0; i < m.params.size(); ++i)
                o << (i ? ", " : "") << code(m.params[i]);
            o << "\n\n";
        }
    }
    write_prose(o, docs, &n, true, spec, spec_rel);
    write_tests(o, n, t);
}

void write_type(std::ostream& o, const FileDocs& fd, const ast::TypeDef& ty, const std::string& pkg,
                const SpecIndex& spec, const std::string& spec_rel, const std::string& src_rel) {
    const std::string& t = fd.src->text();
    o << "## " << ty.name << "\n\n";
    o << "`type " << pkg << "." << ty.name;
    if (!ty.unit_params.empty()) {
        o << " (";
        for (std::size_t i = 0; i < ty.unit_params.size(); ++i)
            o << (i ? ", " : "") << ty.unit_params[i].name;
        o << ")";
    }
    o << "` \xc2\xb7 record \xc2\xb7 [source](" << src_rel << ")\n\n";
    write_prose(o, fd.docs, &ty, false, spec, spec_rel);
    if (!ty.unit_params.empty()) {
        o << "### Unit parameters\n\n| Name | Description |\n|---|---|\n";
        for (const ast::UnitParamDecl& u : ty.unit_params)
            o << "| " << code(u.name) << " | " << fragment(fd.docs, &u, spec, spec_rel) << " |\n";
        o << "\n";
    }
    o << "### Fields\n\n| Name | Type | Description |\n|---|---|---|\n";
    for (const ast::FieldDecl& x : ty.fields) {
        const auto tv = type_and_value(slice_to(t, x.loc.offset, ';'));
        o << "| " << code(x.name) << " | " << code(tv.first) << " | "
          << fragment(fd.docs, &x, spec, spec_rel) << " |\n";
    }
    o << "\n";
    write_prose(o, fd.docs, &ty, true, spec, spec_rel);
}

const char* const kBanner =
    "<!-- Generated by `sec --doc` from the `///` comments in the source. Do not edit. -->\n\n";

bool write_text(const std::string& path, const std::string& text, bool quiet) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "sec: cannot write `%s`\n", path.c_str());
        return false;
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    if (!quiet) std::printf("  wrote %s\n", path.c_str());
    return true;
}

std::string rel(const std::string& target, const std::string& from_dir) {
    std::error_code ec;
    const fs::path r = fs::relative(fs::absolute(target, ec), fs::absolute(from_dir, ec), ec);
    return ec ? fs::path(target).generic_string() : r.generic_string();
}

}  // namespace docgen

int generate_docs(const std::vector<std::string>& inputs, Diagnostics& diag,
                  const DocOptions& opt) {
    namespace fs = std::filesystem;
    using namespace docgen;

    // Every `.se` named or beneath a named directory, sorted so the output
    // does not depend on directory enumeration order.
    std::vector<std::string> paths;
    for (const std::string& in : inputs) {
        std::error_code ec;
        if (fs::is_directory(in, ec)) {
            for (const auto& e : fs::recursive_directory_iterator(in, ec))
                if (e.is_regular_file() && e.path().extension() == ".se")
                    paths.push_back(e.path().generic_string());
        } else if (fs::is_regular_file(in, ec)) {
            paths.push_back(fs::path(in).generic_string());
        } else {
            std::fprintf(stderr, "sec: --doc: `%s` is not a file or directory\n", in.c_str());
            return 2;
        }
    }
    std::sort(paths.begin(), paths.end());

    std::string spec_path = opt.spec_path;
    if (spec_path.empty() && !inputs.empty()) {
        std::error_code ec;
        const fs::path beside = fs::absolute(inputs.front(), ec).parent_path() / "SPECIFICATION.md";
        if (fs::is_regular_file(beside, ec)) spec_path = beside.string();
        else if (fs::is_regular_file("SPECIFICATION.md", ec)) spec_path = "SPECIFICATION.md";
    }
    const SpecIndex spec = load_spec(spec_path);
    if (!opt.spec_path.empty() && !spec.loaded) {
        std::fprintf(stderr, "sec: --doc: cannot read `%s`\n", opt.spec_path.c_str());
        return 2;
    }

    std::vector<std::unique_ptr<Source>> sources;
    std::vector<std::unique_ptr<ast::ModelFile>> files;
    std::vector<std::unique_ptr<FileDocs>> fds;
    for (const std::string& p : paths) {
        auto src = std::make_unique<Source>();
        std::string error;
        if (!Source::load(p, *src, error)) {
            std::fprintf(stderr, "sec: %s: %s\n", p.c_str(), error.c_str());
            return 2;
        }
        Parser parser(*src, diag);
        auto file = parser.parse_model_file();
        if (file->nodes.empty() && file->types.empty()) continue;   // a package manifest
        auto fd = std::make_unique<FileDocs>();
        fd->file = file.get();
        fd->src = src.get();
        fd->path = p;
        fd->stem = fs::path(p).stem().string();
        attach(*fd, diag);
        for (const auto& [key, doc] : fd->docs) check_citations(doc, spec, *fd, diag);
        for (const ast::NodeDef& n : file->nodes)
            if (const auto it = fd->docs.find(&n); it != fd->docs.end())
                check_definition(it->second, *fd, diag);
        for (const ast::TypeDef& t : file->types)
            if (const auto it = fd->docs.find(&t); it != fd->docs.end())
                check_definition(it->second, *fd, diag);
        sources.push_back(std::move(src));
        files.push_back(std::move(file));
        fds.push_back(std::move(fd));
    }
    if (diag.error_count() > 0) return 1;

    // One page per package; only a file's public definition is documented.
    std::map<std::string, std::vector<const FileDocs*>> pages;
    for (const auto& fd : fds) pages[fd->file->package.str()].push_back(fd.get());

    std::error_code ec;
    fs::create_directories(opt.out_dir, ec);
    if (ec && !fs::is_directory(opt.out_dir)) {
        std::fprintf(stderr, "sec: cannot create `%s`\n", opt.out_dir.c_str());
        return 2;
    }
    const std::string spec_rel = spec.loaded ? rel(spec.path, opt.out_dir) : "";

    std::ostringstream index;
    index << "# Standard library reference\n\n" << kBanner;
    bool ok = true;
    for (const auto& [pkg, list] : pages) {
        std::ostringstream o;
        o << "# " << pkg << "\n\n" << kBanner << "[All packages](index.md)\n\n";
        std::ostringstream table;
        table << "| Name | Summary |\n|---|---|\n";
        std::ostringstream body;
        for (const FileDocs* fd : list) {
            const std::string src_rel = rel(fd->path, opt.out_dir);
            for (const ast::TypeDef& t : fd->file->types) {
                if (t.name != fd->stem) continue;
                table << "| [`" << t.name << "`](#" << anchor_of(t.name) << ") | "
                      << summary(fd->docs, &t) << " |\n";
                body << "---\n\n";
                write_type(body, *fd, t, pkg, spec, spec_rel, src_rel);
            }
            for (const ast::NodeDef& n : fd->file->nodes) {
                if (n.name != fd->stem) continue;
                table << "| [`" << n.name << "`](#" << anchor_of(n.name) << ") | "
                      << summary(fd->docs, &n) << " |\n";
                body << "---\n\n";
                write_node(body, *fd, n, pkg, spec, spec_rel, src_rel);
            }
        }
        o << table.str() << "\n" << body.str();
        index << "## [" << pkg << "](" << pkg << ".md)\n\n";
        std::string t = table.str();
        // The index links into the package page rather than to local anchors.
        for (std::size_t at = t.find("](#"); at != std::string::npos; at = t.find("](#", at))
            t.replace(at, 3, "](" + pkg + ".md#");
        index << t << "\n";
        std::string page = o.str();
        while (page.size() > 1 && page[page.size() - 1] == '\n' && page[page.size() - 2] == '\n')
            page.pop_back();
        ok = write_text((fs::path(opt.out_dir) / (pkg + ".md")).generic_string(), page, opt.quiet) && ok;
    }
    std::string ix = index.str();
    while (ix.size() > 1 && ix[ix.size() - 1] == '\n' && ix[ix.size() - 2] == '\n') ix.pop_back();
    ok = write_text((fs::path(opt.out_dir) / "index.md").generic_string(), ix, opt.quiet) && ok;
    return ok ? 0 : 2;
}

std::string doc_comment_at(const std::vector<DocLine>& lines, std::uint32_t line) {
    std::string out;
    auto add = [&](const std::string& t) {
        if (!out.empty()) out += '\n';
        out += t;
    };
    // The leading run: consecutive non-trailing lines, the last directly above.
    std::size_t end = 0;
    while (end < lines.size() && lines[end].line < line) ++end;
    std::size_t begin = end;
    while (begin > 0 && !lines[begin - 1].trailing &&
           lines[begin - 1].line + (end - begin) + 1 == line)
        --begin;
    for (std::size_t k = begin; k < end; ++k) add(lines[k].text);
    for (std::size_t k = end; k < lines.size() && lines[k].line == line; ++k)
        if (lines[k].trailing) add(lines[k].text);
    return out;
}

}  // namespace se
