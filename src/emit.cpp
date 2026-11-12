// ─────────────────────────────────────────────────────────────────────────────
//  Stage 6 — emission.  SPECIFICATION.md §11, §12.4, §15.
// ─────────────────────────────────────────────────────────────────────────────
#include "emit.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace se {
namespace {

// ─── An output buffer that knows its own line number ─────────────────────────
//  Appendix C item 17: a `#line` block set for one body stays in effect over
//  the GENERATED code that follows it, so a warning about a generated signature
//  gets blamed on the previous method's body. The generator must therefore
//  reset `#line` after each verbatim region, not only before one — and to do
//  that it has to know what line it is on.

class Out {
public:
    void w(const std::string& s) {
        for (char c : s)
            if (c == '\n') ++line_;
        buf_ += s;
    }
    Out& operator<<(const std::string& s) {
        w(s);
        return *this;
    }
    Out& operator<<(const char* s) {
        w(s);
        return *this;
    }
    Out& operator<<(std::size_t n) {
        w(std::to_string(n));
        return *this;
    }
    Out& operator<<(long n) {
        w(std::to_string(n));
        return *this;
    }
    unsigned line() const { return line_; }
    const std::string& str() const { return buf_; }

private:
    std::string buf_;
    unsigned line_ = 1;
};

// ─── Small helpers ───────────────────────────────────────────────────────────

std::string pad(const std::string& s, std::size_t n) {
    std::string r = s;
    while (r.size() < n) r += ' ';
    return r;
}

// `pad` where the padding IS the column separator, so an over-long entry must
// still be held off the next column. `pad` alone runs them together -- a 26-
// character signal path in a 24-wide field emits `...mgy.v.xm/s^2` -- and the
// manifest is a contract a host parses, not just something to look at.
std::string col(const std::string& s, std::size_t n) {
    std::string r = pad(s, n);
    if (s.size() >= n) r += ' ';   // pad() added nothing, so add the gap
    return r;
}

// Round-trippable and still readable: the shortest precision that reads back
// as the same double.
std::string dbl(double v) {
    if (std::isnan(v)) return "std::numeric_limits<double>::quiet_NaN()";
    if (std::isinf(v)) return v > 0 ? "HUGE_VAL" : "-HUGE_VAL";
    char buf[64];
    for (int p = 15; p <= 17; ++p) {
        std::snprintf(buf, sizeof buf, "%.*g", p, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

std::string int_text(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(std::llround(v)));
    return buf;
}

// §5.2a — an enum's value is written by name wherever it has one. Without one
// (a zero that is no enumerator) the cast is the only spelling C++ accepts.
std::string literal(double v, const std::string& scalar, const EnumInfo* enm = nullptr) {
    if (enm) {
        if (const EnumInfo::Value* e = enm->find(std::lround(v)))
            return enm->cpp_name + "::" + e->name;
        return "static_cast<" + enm->cpp_name + ">(" + int_text(v) + ")";
    }
    if (scalar == "bool") return v != 0.0 ? "true" : "false";
    if (scalar == "int") return int_text(v);
    if (scalar == "float") return dbl(v) + "f";
    return dbl(v);
}

// The same value as a person reads it — the manifest, the topology page: an
// enum by its bare enumerator, the way a §14 source writes it (§7.2a).
std::string shown(double v, const std::string& scalar, const EnumInfo* enm) {
    if (enm) {
        if (const EnumInfo::Value* e = enm->find(std::lround(v))) return e->name;
        return int_text(v);
    }
    return literal(v, scalar);
}

// A derived setting's expression, path-addressed rather than C++-addressed:
// what the manifest and the topology page show as "<- 2 * fl.whl.radius".
std::string derived_from(const ExprCode& expr) {
    std::string e;
    for (const ExprTok& tok : expr)
        e += tok.kind == ExprTok::Kind::Param
                 ? (tok.owner.empty() ? tok.text : tok.owner + "." + tok.text)
                 : tok.text;
    return e;
}

std::string cpp_type(const Type& t) {
    return t.is_record ? t.record->cpp_name : t.scalar;
}

// §8.3a — the output group a method is, or null for every method but `output`.
const NodeInfo::OutputGroup* group_of(const NodeInfo& n, const ast::Method& m) {
    for (const NodeInfo::OutputGroup& g : n.out_groups)
        if (g.method == &m) return &g;
    return nullptr;
}

// §15.4 — the struct a group writes through: references to its outputs and
// nothing else. Named for the first output it lists; `Out_default` is the share
// of `output default()`, and cannot collide, no output being nameable `default`
// (SE0332).
std::string out_view(const NodeInfo::OutputGroup& g) {
    return g.method->is_default ? "Out_default" : "Out_" + g.method->writes.front();
}

// §6.4 — the three state kinds, spelled once. A state is in `x`, in `dis`, or
// in `fin`, and every block walk below asks exactly one of these.
template <class S> bool is_cont(const S& s) { return s.kind == ast::StateKind::Continuous; }
template <class S> bool is_disc(const S& s) { return s.kind == ast::StateKind::Discrete; }
template <class S> bool is_fin(const S& s) { return s.kind == ast::StateKind::Finite; }

// A `#line` path: forward slashes everywhere, so the emitted directive is the
// same on every platform and the recorded test output is stable.
std::string line_path(const std::string& p) {
    std::string s;
    for (char c : p) s += (c == '\\') ? '/' : c;
    return s;
}

std::string quote(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        r += c;
    }
    return r + "\"";
}

// A trailing unit comment on every emitted declaration — one half of the
// compensation for erasing units (§4.4); the manifest is the other.
std::string unit_comment(const Unit& u) { return "// " + u.str(); }

// A type as a reader of the public header wants it labelled: a scalar by its
// unit, a record by its name and its BOUND units, `se.math.Vec3(m/s^2)`,
// because a parametric record's own fields say only `U` (§5.2).
std::string type_label(const Type& t) {
    if (!t.is_record) return t.unit.str();
    std::string s = t.record->fq;
    for (std::size_t i = 0; i < t.unit_args.size(); ++i)
        s += (i ? ", " : "(") + t.unit_args[i].str();
    return t.unit_args.empty() ? s : s + ")";
}

// One member of a generated view struct, kept as columns so the emitted code
// lines up. The unit comment is the thing a reader scans, so it is worth the
// bookkeeping to put it in a column.
struct Row {
    std::string type;
    std::string decl;      // `name;` or `name{};`
    std::string comment;   // may be empty
};

void emit_struct(Out& o, const char* name, const std::vector<Row>& rows,
                 const char* empty_note = nullptr) {
    o << "    struct " << name << " {";
    if (rows.empty()) {
        o << "};";
        if (empty_note) o << "   " << empty_note;
        o << "\n";
        return;
    }
    std::size_t wt = 0, wd = 0;
    for (const Row& r : rows) {
        wt = std::max(wt, r.type.size());
        if (!r.comment.empty()) wd = std::max(wd, r.decl.size());
    }
    o << "\n";
    for (const Row& r : rows) {
        o << "        " << pad(r.type, wt) << " ";
        if (r.comment.empty()) o << r.decl << "\n";
        else o << pad(r.decl, wd) << "   " << r.comment << "\n";
    }
    o << "    };\n";
}

// §6.7 / §11.3 — the native type text is scanned to the `;`, so an optional
// initialiser is still attached to it. The member name goes between the two.
// Angle brackets are never tracked, which is what keeps `>>` out of the
// language; the first `=`, `{` or `(` at depth zero is the split.
void split_native(const std::string& text, std::string& type, std::string& init) {
    std::size_t split = std::string::npos;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"' || c == '\'') {
            const char q = c;
            for (++i; i < text.size(); ++i) {
                if (text[i] == '\\') ++i;
                else if (text[i] == q) break;
            }
            continue;
        }
        if (c == '=' || c == '{' || c == '(') {
            split = i;
            break;
        }
    }
    auto trim = [](std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n'))
            s.pop_back();
        std::size_t b = 0;
        while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\n')) ++b;
        return s.substr(b);
    };
    if (split == std::string::npos) {
        type = trim(text);
        init = "{}";
        return;
    }
    type = trim(text.substr(0, split));
    init = trim(text.substr(split));
    if (init[0] == '=') init = " " + init;
}

// ═════════════════════════════════════════════════════════════════════════════
//  The emitter
// ═════════════════════════════════════════════════════════════════════════════

// Which shell a reference build script is written for (§12.4).
enum class Script { Ps1, Sh };

// The signal-flow graph as the model runs, in no notation at all. One walk
// over the flat model builds it; the Mermaid diagram and the interactive page
// are each a printer over it, so they cannot disagree about what is wired to
// what. Leaves are referred to by their index in `Model::leaves`, so a printer
// that wants more than the graph -- settings, states -- reads the leaf itself.
struct Topo {
    // The boxes on the diagram: the composite tree, recovered from the leaf
    // paths, with each composite's §6.9.3 groups nested inside it. [0] is the
    // root composite, and a cluster's leaves are in §8.5 schedule order. A
    // cluster exists only if some leaf sits beneath it, so an empty group
    // (SE0218) draws nothing.
    struct Cluster {
        bool is_group = false;             // a §6.9.3 group, not a composite
        std::string name;                  // composite: last path segment ("" for
                                           // the root); group: its label
        std::string path;                  // composite: root-relative ("fl.whl");
                                           // group: the composite that owns it
        std::size_t parent = 0;            // meaningless for [0]
        std::vector<std::size_t> leaves;
        std::vector<std::size_t> subs;
        // A group only: where it is written, and its `///` text.
        std::string file;
        std::uint32_t line = 0;
        std::string doc;
        // §6.15 — the page opens this box collapsed. Mermaid ignores it: a
        // static diagram cannot be expanded again, so it keeps every leaf.
        bool collapsed = false;
    };
    // A root output the host may read, collapsed from `boundary_out`'s
    // field-by-field rows back onto the port they came from.
    struct Output {
        std::string ident;                 // producer leaf's identifier
        std::size_t leaf = 0;              // index, or npos if it is not a leaf
        std::string port;
        std::string path;                  // "whl.ws"
        std::string type;                  // "?" when the port is not found
        std::vector<std::string> via;      // composite ports on the way out
    };
    // One connected input port.
    struct Edge {
        enum class From { Leaf, Boundary, Const };
        From from = From::Leaf;
        std::size_t src = 0;               // leaf index, or index into `consts`
        // Const from a `param.*`: index into `settings`, which the page draws
        // instead of the per-wire `consts` entry Mermaid uses.
        std::size_t setting = static_cast<std::size_t>(-1);
        std::string boundary;              // Boundary: the root input's name
        std::string src_port;              // Leaf: the producer's port
        std::size_t dst = 0;               // consuming leaf
        std::string dst_port;
        std::string type;                  // as a reader wants it, with any
                                           // folded conversion: "rad to deg"
        bool feedthrough = false;          // `output()` reads it (§8.3)
        // The composite ports the wire passes through, source end first, so
        // the page can draw a collapsed composite by its own ports.
        std::vector<std::string> via;
    };

    std::vector<Cluster> clusters;
    std::vector<std::size_t> rank;         // leaf index -> §8.5 position
    std::vector<std::string> in_type;      // per `boundary_in`
    std::vector<Output> outs;
    std::vector<std::string> consts;       // §6.9.2 constant sources, as text
    // §6.9.2 — a setting wired as a source, once per setting however many
    // ports it drives, and in the box of the composite that declares it.
    struct Setting {
        std::string owner;                 // "wash", "" at the root
        std::string name;                  // "headPos"
        std::string path;                  // "wash.headPos"
        std::string type;                  // as `topo_type` writes it
        std::size_t cluster = 0;
    };
    std::vector<Setting> settings;
    std::vector<Edge> edges;
};

class Emitter {
public:
    // `stem` names the three sources: <stem>.hpp is the public interface,
    // <stem>.cpp the implementation, and the #line resyncs point at the latter,
    // because that is where every verbatim body lands.
    Emitter(const Model& m, const std::string& stem)
        : m_(m), gen_(stem + ".cpp"), hdr_(stem + ".hpp"), stem_(stem) {}

    std::string public_hpp();
    std::string impl_cpp(bool separate_runtime);
    std::string main_cpp();
    std::string manifest();
    Topo topology() const;
    std::string topology_mermaid(const Topo& t) const;
    std::string topology_json(const Topo& t) const;
    // `exe_main` is the file holding main(). With `unity` it #includes the
    // implementation itself (a white-box test program), so it is compiled alone.
    std::string build_script(Script kind, const std::string& exe_main, bool unity);

private:
    // The public header's pieces (§15.7).
    std::set<const RecordInfo*> public_records() const;
    std::set<const EnumInfo*> public_enums() const;
    void record_def(Out& o, const RecordInfo* r) const;
    void enum_def(Out& o, const EnumInfo* e) const;
    void public_class();
    std::vector<std::size_t> root_settings() const;
    std::string root_setting_default(const std::string& decl, const Type& t) const;
    std::string cpp_ns() const;
    std::uint64_t tick_count() const;
    void sim_facade();
    void sim_public_glue();
    std::string source_expr(const InputSource& s, const Type* type) const;

    // Verbatim regions (§11), with `#line` on both sides.
    void verbatim(const Verbatim& v, const std::string& src_path);
    void preamble(const NodeInfo& node, ast::Method::Which which, const char* indent);

    void records();
    void declarations();
    void leaf_class(const NodeInfo& node);
    void sim_class();

    // Sim-class sections.
    void sim_members();
    void sim_resolve_settings();
    void sim_configure();
    void sim_configure_slots();
    void sim_configure_indices();
    void sim_binders();
    void sim_elaborate();
    // Maps a node's model path to the C++ object holding its settings.
    // Built once, because a leaf's identifier is only known after the
    // collision-dedup pass and a composite has no identifier at all
    // until one is minted here.
    void build_param_bases();
    std::string param_base(const std::string& owner) const;
    std::string render_expr(const ExprCode& c) const;
    std::map<std::string, std::string> param_base_;
    void sim_init();
    void sim_output_pass();
    void sim_derivatives();
    void sim_next();
    void sim_integrate();
    void sim_integrate_dynamic();
    void sim_record();
    void sim_run();
    void sim_maps();

    // §14.1 — every path a settings source may bind, in the order of the
    // generated `setting_rows()` table: the settings first, indexed exactly as
    // `m_.settings` is, so a setting's row number IS its pin; then each scalar
    // state's initial value; then each array state, bound by element.
    struct LoadRow {
        enum class Kind { Setting, Initial, Element, Run };
        Kind kind = Kind::Setting;
        std::string path;
        std::size_t setting = 0;           // Setting: index into m_.settings
        const Leaf* leaf = nullptr;        // Initial, Element
        const StateSlot* state = nullptr;  // Initial, Element
    };
    std::vector<LoadRow> load_rows() const;
    static std::string ic_member(const Leaf& leaf, const StateSlot& st);
    void sim_settings_source();

    // The expression a consumer reads for one of its inputs (§15.3).
    std::string input_expr(const Leaf& leaf, const std::string& port) const;
    std::string in_braces(const Leaf& leaf, const ast::Method* method) const;
    std::string output_call(const Leaf& leaf, const NodeInfo::OutputGroup& g) const;
    bool minor_step_output(const Leaf& leaf) const;
    static bool has_next(const Leaf& leaf);
    bool any_next() const;
    bool jumps_continuous() const;

    // One row per scalar slot of a block: { model path, C++ member path,
    // scalar type, unit }. Built once and used by BOTH the manifest and the
    // offsetof table, so the two cannot drift apart.
    using SlotRow = std::array<std::string, 4>;
    std::vector<SlotRow> signal_rows() const;
    std::vector<SlotRow> discrete_rows() const;
    std::vector<SlotRow> finite_rows() const;
    std::vector<SlotRow> array_state_rows(bool continuous) const;

    const Model& m_;
    std::string gen_;    // <stem>.cpp
    std::string hdr_;    // <stem>.hpp
    std::string stem_;
    Out o_;
};

void Emitter::verbatim(const Verbatim& v, const std::string& src_path) {
    if (!v.present) return;

    // A brace-matched region's text starts immediately after the `{`, so it
    // opens with the remainder of that line — almost always blank — and closes
    // with the indentation before the `}`. Trimming both, and advancing
    // `first_line` by the number of leading lines dropped, makes the body start
    // on the line after the signature with nothing between them.
    std::size_t begin = 0;
    std::uint32_t skipped = 0;
    for (;;) {
        std::size_t eol = v.text.find('\n', begin);
        if (eol == std::string::npos) break;
        if (v.text.find_first_not_of(" \t\r", begin) < eol) break;   // has content
        begin = eol + 1;
        ++skipped;
    }
    std::size_t end = v.text.size();
    while (end > begin && (v.text[end - 1] == ' ' || v.text[end - 1] == '\t' ||
                           v.text[end - 1] == '\r' || v.text[end - 1] == '\n'))
        --end;
    if (end <= begin) return;   // an empty body needs no directives at all

    o_ << "#line " << static_cast<std::size_t>(v.first_line + skipped) << " "
       << quote(line_path(src_path)) << "\n";
    o_.w(v.text.substr(begin, end - begin));
    o_ << "\n";
    // Back to the generated file, so the NEXT thing — a closing brace, the next
    // signature — is attributed here and not to the body above it
    // (Appendix C item 17).
    o_ << "#line " << static_cast<std::size_t>(o_.line() + 1) << " " << quote(gen_) << "\n";
}

// Every method is `const`, and every writable thing is an out-parameter, so
// nothing here is a protection mechanism — `param` is const by construction and
// `state` is const through `this`. What is left is purely names being brought
// into scope where §8.4 says they are legal, and withheld where it does not.
//
// The two pure methods -- `output()` and `derivative()` -- get nothing at all:
// the leaf already holds a trace-only `log` member and a reflection-only `sim`
// member (§10.5), so their bodies are verbatim from brace to brace.
void Emitter::preamble(const NodeInfo& node, ast::Method::Which which, const char* ind) {
    using W = ast::Method::Which;
    const bool mutating = which == W::Init || which == W::Next || which == W::Final;
    if (!mutating) return;

    // Widens the leaf's reflection-only `sim` member with the two calls that
    // end a run. `final()` is deliberately NOT here: it already reads the
    // outcome from `ctx` (§10.3), and halting a run that is already ending is
    // not something §10.2 offers. It keeps the plain `RunView` member, so
    // `sim.time()` works there while `sim.stop` stays "no such member".
    if (which == W::Init || which == W::Next)
        o_ << ind << "const se_rt::RunControl sim{se_run_, se_path_, se_type_};"
           << "   // adds sim.stop / sim.abort, §10.2\n";
    // Shadows the leaf's trace-only member with the full set of levels.
    o_ << ind << "[[maybe_unused]] const se_rt::Log log{se_run_, se_path_};"
       << "   // every level is legal here, §10.1\n";

    // §6.2b made the member writable so `Sim::resolve_settings` can assign it.
    // `init()` is the one body that is not a const method, so without this the
    // member's constness was all that stopped a body rewriting its own settings
    // after configuration. Shadowing restores the guarantee where I2 says it
    // belongs: in what the generator puts in scope, not in a type.
    if (which == W::Init)
        o_ << ind << "const Param& param = this->param;"
           << "   // fixed by configuration, §6.2b\n";

    // §6.7 — bodies refer to natives by their bare name, and the alias exists
    // only where they are legal. Elsewhere the name is simply undeclared and
    // the C++ compiler says so, with `#line` pointing at the `.se` line. `const`
    // cannot do this job: a `const SOCKET` is still perfectly usable by
    // `recv()`, which is also why the members are `mutable`.
    for (const ast::NativeDecl& n : node.def->natives)
        o_ << ind << "[[maybe_unused]] auto& " << n.name << " = se_nat_" << n.name << ";\n";
}

// ─── Records (§5.2) ──────────────────────────────────────────────────────────

// One record, guarded by its fully-qualified name. The guard is what lets two
// model headers that both use `se.math.Vec3` be included in one translation
// unit, and lets a record appear in one model's public header and another
// model's implementation without colliding.
void Emitter::record_def(Out& o, const RecordInfo* r) const {
    std::string guard = "SE_TYPE_";
    for (char c : r->fq) guard += (c == '.' || c == '#') ? '_' : c;
    const std::size_t colon = r->cpp_name.rfind("::");
    const std::string ns = colon == std::string::npos ? "" : r->cpp_name.substr(0, colon);
    const std::string base =
        colon == std::string::npos ? r->cpp_name : r->cpp_name.substr(colon + 2);
    o << "#ifndef " << guard << "\n#define " << guard << "\n";
    if (!ns.empty()) o << "namespace " << ns << " {\n";
    std::size_t wt = 0, wd = 0;
    for (const Field& f : r->fields) {
        wt = std::max(wt, cpp_type(f.type).size());
        wd = std::max(wd, f.name.size() + 1);
    }
    o << "struct " << base << " {   // " << r->fq << "\n";
    for (const Field& f : r->fields) {
        o << "    " << pad(cpp_type(f.type), wt) << " " << pad(f.name + ";", wd);
        if (!f.type.is_record) o << "   " << unit_comment(f.type.unit);
        o << "\n";
    }
    o << "};\n";
    if (!ns.empty()) o << "}  // namespace " << ns << "\n";
    o << "#endif\n\n";
}

// The records the public interface names — a root input, output or setting —
// and every record those contain. They go in the public header; the rest stay
// in the implementation.
std::set<const RecordInfo*> Emitter::public_records() const {
    std::set<const RecordInfo*> pub;
    std::function<void(const Type&)> add = [&](const Type& t) {
        if (!t.is_record || !t.record || !pub.insert(t.record).second) return;
        for (const Field& f : t.record->fields) add(field_type(t, f));
    };
    for (const BoundaryIn& b : m_.boundary_in) add(b.type);
    for (const RootOutput& r : m_.root_outputs) add(r.type);
    for (std::size_t i : root_settings()) add(m_.settings[i].decl_type);
    return pub;
}

// §5.2a — a scoped enumeration, guarded and namespaced exactly as a record is,
// for the same reason: two models naming one enum must share a translation
// unit without colliding. The values are always written out, so the C++ and
// the manifest's `[types]` row cannot disagree about what a recorded 15 means.
void Emitter::enum_def(Out& o, const EnumInfo* e) const {
    std::string guard = "SE_TYPE_";
    for (char c : e->fq) guard += (c == '.' || c == '#') ? '_' : c;
    const std::size_t colon = e->cpp_name.rfind("::");
    const std::string ns = colon == std::string::npos ? "" : e->cpp_name.substr(0, colon);
    const std::string base =
        colon == std::string::npos ? e->cpp_name : e->cpp_name.substr(colon + 2);
    o << "#ifndef " << guard << "\n#define " << guard << "\n";
    if (!ns.empty()) o << "namespace " << ns << " {\n";
    std::size_t wn = 0;
    for (const EnumInfo::Value& v : e->values) wn = std::max(wn, v.name.size());
    o << "enum class " << base << " : int {   // " << e->fq << "\n";
    for (const EnumInfo::Value& v : e->values)
        o << "    " << pad(v.name, wn) << " = " << v.value << ",\n";
    o << "};\n";
    if (!ns.empty()) o << "}  // namespace " << ns << "\n";
    o << "#endif\n\n";
}

// The enums the public interface names, directly or through a public record's
// fields. They precede the records, which may contain them.
std::set<const EnumInfo*> Emitter::public_enums() const {
    std::set<const EnumInfo*> pub;
    std::function<void(const Type&)> add = [&](const Type& t) {
        if (t.enm) pub.insert(t.enm);
        if (!t.is_record || !t.record) return;
        for (const Field& f : t.record->fields) add(field_type(t, f));
    };
    for (const BoundaryIn& b : m_.boundary_in) add(b.type);
    for (const RootOutput& r : m_.root_outputs) add(r.type);
    for (std::size_t i : root_settings()) add(m_.settings[i].decl_type);
    return pub;
}

void Emitter::records() {
    // Enums first: they refer to nothing, and a record may hold one.
    const std::set<const EnumInfo*> pub_e = public_enums();
    for (const EnumInfo* e : m_.program->enum_order)
        if (!pub_e.count(e)) enum_def(o_, e);
    // Loading is need-driven, so `record_order` holds exactly the records this
    // model reaches. It is post-order over the field graph, which means a
    // contained record always precedes its container and the emitted sequence
    // compiles without any forward declarations.
    const std::set<const RecordInfo*> pub = public_records();
    for (const RecordInfo* r : m_.program->record_order)
        if (!pub.count(r)) record_def(o_, r);
}

// ─── `declarations` blocks (§6.6) ────────────────────────────────────────────

void Emitter::declarations() {
    // §2.6 — every loaded manifest's shared C++ first, so any node's own block
    // may use it. Guarded by the PACKAGE, which is what lets two nodes of one
    // package share one helper class in this single translation unit.
    for (const auto& f : m_.program->files) {
        if (f->public_name != "package" || !f->ast || !f->ast->sec_declarations.present)
            continue;
        std::string guard = "SE_PKG_DECLS_";
        for (char c : f->package) guard += c == '.' ? '_' : c;
        o_ << "#ifndef " << guard << "\n#define " << guard << "\n";
        verbatim(f->ast->declarations, f->path);
        o_ << "#endif  // " << guard << "\n\n";
    }

    std::set<const NodeInfo*> done;
    for (const Leaf& leaf : m_.leaves) {
        const NodeInfo& node = *leaf.node;
        if (!done.insert(&node).second) continue;
        if (!node.def->declarations.present) continue;

        std::string guard = "SE_DECLS_";
        for (char c : node.fq) guard += (c == '.' || c == '#') ? '_' : c;
        o_ << "#ifndef " << guard << "\n#define " << guard << "\n";
        verbatim(node.def->declarations, node.file->path);
        o_ << "#endif  // " << guard << "\n\n";
    }
}

// ─── One leaf class (§15.4) ──────────────────────────────────────────────────

void Emitter::leaf_class(const NodeInfo& node) {
    const std::size_t colon = node.cpp_name.rfind("::");
    const std::string ns = colon == std::string::npos ? "" : node.cpp_name.substr(0, colon);
    const std::string base =
        colon == std::string::npos ? node.cpp_name : node.cpp_name.substr(colon + 2);

    const std::string title = "// -- node " + node.fq + " ";
    o_ << title << std::string(title.size() < 78 ? 78 - title.size() : 1, '-') << "\n";
    if (!ns.empty()) o_ << "namespace " << ns << " {\n";
    o_ << "struct " << base << " {\n";

    // §2.3 / §5.2a — a file-private enum is visible to the rest of its file,
    // and a body cannot use one without naming it (`Ctl::AUTO`). It lowers into
    // the file's private namespace, which the file's public node does not sit
    // in, so each node of the file gets a member alias by the bare name. A
    // record needs none: a body reaches one's fields, never its name.
    {
        bool any = false;
        for (const EnumInfo* e : m_.program->enum_order) {
            if (e->file != node.file || e->def->name == node.file->public_name) continue;
            o_ << "    using " << e->def->name << " = " << e->cpp_name
               << ";   // file-private, §2.3\n";
            any = true;
        }
        if (any) o_ << "\n";
    }

    // ── the view structs ─────────────────────────────────────────────────────
    auto field_row = [](const std::string& name, const Type& t, const char* init) {
        Row r;
        r.type = cpp_type(t);
        r.decl = name + init + ";";
        if (t.buffer) r.comment = "// " + buffer_label(t);   // §5.1a — no unit
        else if (!t.is_record) r.comment = unit_comment(t.unit);
        return r;
    };

    std::vector<Row> rows;
    for (const auto& s : node.settings) {
        rows.push_back(field_row(s.name, s.type, "{}"));
    }
    emit_struct(o_, "Param", rows);

    rows.clear();
    for (const Field& v : node.vars) {
        Row r = field_row(v.name, v.type, "{}");
        // §6.5 — an array var is the one thing the node OWNS rather than
        // views. It is private storage outside every published block, so there
        // is no host buffer to bind and nothing external to contract with; it is
        // sized once, at configuration, and never inside a tick (Appendix C 8a).
        if (v.extent) {
            r.type = "std::vector<double>";
            r.decl = v.name + ";";
        }
        rows.push_back(r);
    }
    emit_struct(o_, "Var  ", rows);

    // Every kind of state is a view: continuous into `Sim::x`, discrete into
    // `Sim::dis`, finite into `Sim::fin`. Held BY VALUE, so constness
    // propagates from the method.
    rows.clear();
    for (const auto& s : node.states) {
        Row r = field_row(s.name, s.type, "");
        // §6.4a — an array state is a view too, one step further: a pointer
        // plus a length, because the length is a configuration result and a
        // `std::array` member is not available even in principle.
        r.type = s.extent ? "state_arr"
                          : (is_cont(s) ? "state_ref" : "value_ref<" + s.type.scalar + ">");
        rows.push_back(r);
    }
    emit_struct(o_, "State", rows);
    // §6.4 / §15.5 — `next()`'s out-parameter: one view per state of any kind,
    // bound into the buffers rather than the committed blocks. `State`'s shape
    // exactly, which is why `next.<s>` exists for precisely the names `state.<s>`
    // does, and a node with no `next()` pays for none of it.
    if (node.method(ast::Method::Which::Next) && !node.states.empty())
        emit_struct(o_, "Next ", rows);

    if (node.has_continuous()) {
        rows.clear();
        for (const auto& s : node.states) {
            if (!is_cont(s)) continue;
            rows.push_back({s.extent ? "state_arr" : "state_ref", s.name + ";",
                            unit_comment(s.der_unit)});
        }
        emit_struct(o_, "Der  ", rows);
    }
    if (node.has_discrete()) {
        // This node's slice of the discrete block, in `dis` and in its buffer
        // `nxt` alike: the same shape is what makes one whole-block `dis = nxt`
        // a simultaneous update across the entire model (§6.4).
        rows.clear();
        for (const auto& s : node.states) {
            if (!is_disc(s)) continue;
            // §6.4a — an array-shaped discrete state cannot be a member
            // here: a run-time-sized one would destroy the `offsetof` property
            // `dis` exists for. It is a view into the fourth block instead, and
            // the whole-block `dis = nxt` commits it through array_ref's
            // value-copying assignment.
            if (s.extent) {
                rows.push_back({"state_arr", s.name + ";", unit_comment(s.type.unit)});
                continue;
            }
            rows.push_back(field_row(s.name, s.type, "{}"));
        }
        emit_struct(o_, "Store", rows);
    }
    if (node.has_finite()) {
        // §6.4c — this node's slice of the finite block and of its buffer.
        rows.clear();
        for (const auto& s : node.states)
            if (is_fin(s)) rows.push_back(field_row(s.name, s.type, "{}"));
        emit_struct(o_, "Fin  ", rows);
    }

    rows.clear();
    for (const Field& f : node.outputs) rows.push_back(field_row(f.name, f.type, "{}"));
    emit_struct(o_, "Out  ", rows);
    // §8.3a / §15.4 — with output groups, each method writes through a view of
    // its own outputs only, bound to `Out`'s members at the call site, so
    // writing another group's output is "no such member" as reading an
    // undeclared input is.
    if (node.grouped) {
        for (const NodeInfo::OutputGroup& g : node.out_groups) {
            rows.clear();
            for (const std::string& w : g.writes) {
                Row r = field_row(w, node.output(w)->type, "");
                r.type += "&";
                rows.push_back(r);
            }
            emit_struct(o_, out_view(g).c_str(), rows);
        }
    }

    // FEEDTHROUGH IS THIS: one In view per method, holding exactly the inputs
    // that method's DSL signature listed. Reading an input you did not declare
    // is not a stale value — it is "no such member", a compile error.
    //
    // A scalar is a value, so a wire's unit conversion folds into the copy; a
    // record is a `const&` into the signal block, so nothing is copied.
    o_ << "\n";
    for (const ast::Method& meth : node.def->methods) {
        using W = ast::Method::Which;
        if (meth.which == W::Init || meth.which == W::Final) continue;
        const NodeInfo::OutputGroup* grp = group_of(node, meth);
        const std::string name =
            std::string("In_") + (meth.which == W::Output       ? (grp ? grp->cpp : "output")
                                  : meth.which == W::Derivative ? "derivative"
                                                                : "next");
        rows.clear();
        for (const std::string& p : meth.params) {
            const Field* f = node.input(p);
            if (!f) continue;
            // By value, like a scalar input. A record used to alias the
            // producer's storage through a reference, which left nowhere to
            // apply a per-field unit conversion (§5.2); now both kinds of input
            // are materialised in the callee's parameter block and both
            // convert. The copy is the price, and it is three doubles for a
            // `Vec3`.
            if (f->type.is_record)
                rows.push_back({cpp_type(f->type), p + ";", std::string()});
            else rows.push_back(field_row(p, f->type, "{}"));
        }
        emit_struct(o_, name.c_str(), rows,
                    meth.which == W::Output ? "// empty => sort root => breaks loops"
                                            : nullptr);
    }

    // Settings are fixed at elaboration (§6.2), so the member is `const` and is
    // set at construction. There is no spelling of it that a body can assign —
    // not `param.x`, and not `this->param.x` either.
    o_ << "\n";
    std::vector<Row> mem;
    mem.push_back({"Param", "param;", "// written by Sim::Impl::resolve_settings"});
    mem.push_back({"const char* const", "se_path_;", "// model path, for the log tag"});
    // §10.5 - `se_run_` points at the one se_rt::Run the enclosing Sim owns:
    // its clock, stop flag, contracts and log. `sim.time()` is a member read and
    // not a global, and two models in one process share none of it.
    mem.push_back({"const char* const", "se_type_;", "// fq node type, §10.5"});
    mem.push_back({"se_rt::Run* const", "se_run_;", "// this model's run"});
    // `{}` even though Sim::bind() points every view before anything reads one:
    // the leaf is aggregate-initialised through `se_run_` and no further, and a
    // trailing member with no initialiser of its own is what g++'s
    // -Wmissing-field-initializers reports, once per leaf, in every model.
    if (!node.states.empty()) mem.push_back({"State", "state{};", "// views into x / dis"});
    if (!node.vars.empty()) mem.push_back({"Var", "var{};", ""});
    for (const ast::NativeDecl& n : node.def->natives) {
        std::string type, init;
        split_native(n.cpp_type, type, init);
        // `mutable` because every method is const now. §6.7 already establishes
        // that `const` never protected a handle anyway — a `const SOCKET` is
        // still perfectly usable by `recv()` — so the confinement stays where
        // it always was, in whether the alias is emitted at all.
        mem.push_back({"mutable " + type, "se_nat_" + n.name + init + ";", ""});
    }
    mem.push_back({"const se_rt::RunView", "sim{se_run_, se_path_, se_type_};",
                   "// reflection only; §10.5"});
    mem.push_back({"const se_rt::TraceLog", "log{se_run_, se_path_};",
                   "// log.trace only; §10.1"});
    {
        std::size_t wt = 0, wd = 0;
        for (const Row& r : mem) {
            wt = std::max(wt, r.type.size());
            if (!r.comment.empty()) wd = std::max(wd, r.decl.size());
        }
        for (const Row& r : mem) {
            o_ << "    " << pad(r.type, wt) << " ";
            if (r.comment.empty()) o_ << r.decl << "\n";
            else o_ << pad(r.decl, wd) << "   " << r.comment << "\n";
        }
    }

    // ── the methods ──────────────────────────────────────────────────────────
    //  Purity is `const` on the method, and the writable thing is always an
    //  out-parameter (§15.4). `init()` is the one exception: §6.4 makes it the
    //  escape hatch for a computed IC and §6.5 lets it compute `var`, so it
    //  writes its own members directly. `param` needs no protection there — it
    //  is a `const Param`, in every method and every spelling.
    for (const ast::Method& meth : node.def->methods) {
        using W = ast::Method::Which;
        const bool has_vars = !node.vars.empty();
        o_ << "\n";
        switch (meth.which) {
            case W::Init:
                o_ << "    void init() {\n";
                break;
            case W::Output:
                if (node.grouped) {
                    // §8.3a — each group is its own member. The view is taken by
                    // value: it is a struct of references, built at the call site
                    // from the node's slice of `sig`, so a copy writes through.
                    const NodeInfo::OutputGroup* g = group_of(node, meth);
                    o_ << "    void " << g->cpp << "(const In_" << g->cpp << "& in, "
                       << out_view(*g) << " out) const {\n";
                } else {
                    o_ << "    void output(const In_output& in, Out& out) const {\n";
                }
                break;
            case W::Derivative:
                o_ << "    void derivative(const In_derivative& in, Der& der) const {\n";
                break;
            case W::Next:
                // The parameter shadows this member function's own name. Legal,
                // and invisible to authors: §11 copies bodies verbatim, so what a
                // body writes is `next.<state>`, which resolves to the parameter.
                // The accessor token has to be spelled `next` (§6.10). `var` is
                // an out-parameter for the reason `next` is: the method is const.
                o_ << "    void next(const In_next& in"
                   << (node.states.empty() ? "" : ", Next& next")
                   << (has_vars ? ", Var& var" : "") << ") const {\n";
                break;
            case W::Final: {
                const std::string p = meth.params.empty() ? "" : " " + meth.params.front();
                o_ << "    void final(const se_rt::RunContext&" << p << ") const {\n";
                break;
            }
        }
        preamble(node, meth.which, "        ");
        verbatim(meth.body, node.file->path);
        o_ << "    }\n";
    }

    for (const ast::Helper& h : node.def->helpers) {
        o_ << "\n";
        o_ << "#line " << static_cast<std::size_t>(h.signature.first_line) << " "
           << quote(line_path(node.file->path)) << "\n";
        o_.w("    ");
        o_.w(h.signature.text);
        o_.w(" {");
        o_.w(h.body.text);
        o_.w("}\n");
        o_ << "#line " << static_cast<std::size_t>(o_.line() + 1) << " " << quote(gen_)
           << "\n";
    }

    o_ << "};\n";
    if (!ns.empty()) o_ << "}  // namespace " << ns << "\n";
    o_ << "\n";
}

// ─── Wires (§15.3) ───────────────────────────────────────────────────────────

std::string Emitter::input_expr(const Leaf& leaf, const std::string& port) const {
    auto it = leaf.inputs.find(port);
    if (it == leaf.inputs.end()) return "{}";
    // Walking `ports_in` rather than the definition's ports is what gives the
    // concrete record type after unit substitution (§6.2a).
    const Type* type = nullptr;
    for (const Field& f : leaf.ports_in)
        if (f.name == port) { type = &f.type; break; }
    return source_expr(it->second, type);
}

// What a reader of one wire sees: the source's storage, a setting, or a folded
// literal, with the chain's conversion applied. `type` is the READER's declared
// type, needed only to rebuild a converted record field by field.
std::string Emitter::source_expr(const InputSource& s, const Type* type) const {
    // §6.9.2 — a setting source is read where the value is read: the setting is
    // a live member (§6.2b), so an override at configuration reaches the wire
    // without anything being folded, and no `Out` storage is invented. A
    // literal source is the opposite half of that sentence: nothing can
    // override it, so it is folded at elaboration and arrives here as a number.
    std::string expr = s.kind == InputSource::Kind::Boundary ? s.boundary
                       : s.kind == InputSource::Kind::Param  ? render_expr(s.expr)
                       : s.kind == InputSource::Kind::Const
                           ? dbl(s.value)
                           : "sig." + m_.leaves[s.producer].ident + "." + s.port;
    if (!s.converts()) return expr;

    // A record converts field by field, so it is rebuilt as a brace initialiser
    // over the same depth-first order the elaborator flattened (§5.2).
    if (!s.field_convs.empty()) {
        if (type && type->is_record) {
            std::size_t k = 0;
            std::function<std::string(const Type&, const std::string&)> build =
                [&](const Type& t, const std::string& base) -> std::string {
                std::string r = cpp_type(t) + "{";
                for (std::size_t i = 0; i < t.record->fields.size(); ++i) {
                    const Field& f = t.record->fields[i];
                    const Type ft = field_type(t, f);
                    const std::string sub_expr = base + "." + f.name;
                    if (i) r += ", ";
                    if (ft.is_record) {
                        r += build(ft, sub_expr);
                        continue;
                    }
                    if (k >= s.field_convs.size()) {
                        r += sub_expr;
                        continue;
                    }
                    const FieldConv& fc = s.field_convs[k++];
                    if (!fc.converts()) {
                        r += sub_expr;
                        continue;
                    }
                    std::string v = sub_expr + " * " + dbl(fc.scale);
                    if (fc.offset != 0.0) v += " + " + dbl(fc.offset);
                    r += "(" + v + ")";
                }
                return r + "}";
            };
            return build(*type, expr);
        }
    }

    // The conversion the two declared units imply, folded to a constant. It
    // lands here because the `In` view is a copy anyway; nothing else in the
    // generated code ever needs conversion machinery.
    std::string r = expr + " * " + dbl(s.scale);
    if (s.offset != 0.0) r += " + " + dbl(s.offset);
    return "(" + r + ")";
}

std::string Emitter::in_braces(const Leaf& leaf, const ast::Method* method) const {
    if (!method || method->params.empty()) return "{}";
    std::string r = "{";
    for (std::size_t i = 0; i < method->params.size(); ++i) {
        if (i) r += ", ";
        r += input_expr(leaf, method->params[i]);
    }
    return r + "}";
}

// One output method's call, `;` included. Without groups it is the one
// `output()` writing the node's whole slice of `sig`; with them (§8.3a) each
// method gets a view of its own outputs, bound here.
std::string Emitter::output_call(const Leaf& leaf, const NodeInfo::OutputGroup& g) const {
    const std::string slice = "sig." + leaf.ident;
    std::string r = leaf.ident + "." + g.cpp + "(" + in_braces(leaf, g.method) + ", ";
    if (!leaf.node->grouped) return r + slice + ");";
    r += "{";
    for (std::size_t i = 0; i < g.writes.size(); ++i)
        r += (i ? ", " : "") + slice + "." + g.writes[i];
    return r + "});";
}

// §10.5 — whether this leaf's `output()` is re-evaluated at the solver's minor
// steps. A node with a `discrete` state is zero-order held across the step
// (§9.3), so re-running it there would let `sim.time()` move
// underneath a held output. Everything else — continuous and purely algebraic
// nodes — is re-run, which is exactly what lets a CONTINUOUS source see the
// minor-step time and stop RK4 degrading to first order across a forcing path.
//
// A `finite` state does not hold its node (§9.3): it is committed before the
// advance, so the stages see one value across the step, the one the step was
// decided under. That is what lets a gear sit beside a torque computed from a
// continuous speed. `has_discrete()` asks for the discrete kind alone.
bool Emitter::minor_step_output(const Leaf& leaf) const {
    return !leaf.has_discrete();
}

// Whether this leaf has a `next()` to run. A node that declares none gets no
// `Next` view, no binding and no call.
bool Emitter::has_next(const Leaf& leaf) { return leaf.node->method(ast::Method::Which::Next); }

bool Emitter::any_next() const {
    for (const Leaf& leaf : m_.leaves)
        if (has_next(leaf)) return true;
    return false;
}

// §15.5 — whether the model needs a buffer for `x`. Only a `next()` beside a
// continuous state can reach it, so a model with none has no `xnx`, no seed and
// no commit of it.
bool Emitter::jumps_continuous() const {
    for (const Leaf& leaf : m_.leaves)
        if (has_next(leaf) && leaf.has_continuous()) return true;
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  The Sim class
// ═════════════════════════════════════════════════════════════════════════════

void Emitter::sim_members() {
    o_ << "    // ---- lifecycle " << std::string(57, '-') << "\n";
    o_ << "    //  The public Sim forwards here. start() is Sim::init(): it takes the\n";
    o_ << "    //  host's settings, configures, and runs every node's init().\n";
    if (!m_.dynamic_states) {
        o_ << "    Impl() { bind(); resolve_settings(); publish_settings(); }\n";
    } else {
        // §15.5a — the block sizes are not known until configuration, so
        // the constructor cannot bind what does not exist yet. It configures,
        // and start() sizes the buffers and hands them in.
        o_ << "    //  This model has array-shaped states (§6.4a), so its block sizes are a\n";
        o_ << "    //  CONFIGURATION result: start() sizes the buffers after configure()\n";
        o_ << "    //  and hands them to bind_state() (§15.5a).\n";
        o_ << "    Impl() { bind(); configure(); publish_settings(); }\n";
        o_ << "    void configure();                        // §6.2b steps 1-4\n";
        o_ << "    std::size_t state_size() const { return n_states; }\n";
        if (m_.n_disc_arr_slots)
            o_ << "    std::size_t discrete_array_size() const { return n_dstates; }\n";
        o_ << "    void bind_state(double* px, double* pxd, std::size_t n);\n";
        if (m_.n_disc_arr_slots)
            o_ << "    void bind_discrete_array(double* pd, double* pn, std::size_t n);\n";
    }
    o_ << "    bool start();                            // Sim::init()\n";
    o_ << "    void init();\n";
    o_ << "    void tick();                             // one base step (§9.6)\n";
    o_ << "    //  Sim::sync_realtime(): wait for the wall to reach time() (§9.4).\n";
    o_ << "    void sync_realtime() {\n";
    o_ << "        se_run_.realtime.sync(static_cast<double>(se_tick_) * step_seconds);\n";
    o_ << "    }\n";
    o_ << "    bool done() const { return se_done_; }\n";
    o_ << "    void finish();\n";
    o_ << "    int  exit_code() const;                  // what Sim::finish() returns\n";
    o_ << "    const char* error() const;               // what Sim::error() returns\n";
    o_ << "    //  Empty unless resolve_settings() rejected a value it was given\n";
    o_ << "    //  (§16.4). init() refuses to start a run while it is set.\n";
    o_ << "    const std::string& config_error() const { return se_cfg_error_; }\n";
    o_ << "    double time() const { return se_time_; }\n";
    o_ << "    std::uint64_t tick_index() const { return se_tick_; }\n\n";

    // The public face of the model's own state: the host's copy of the root
    // settings, the snapshot that tells an edit from a default, and the root
    // outputs as the host reads them.
    o_ << "    Sim::Settings se_settings_{};   // the host edits these through Sim::settings()\n";
    o_ << "    Sim::Settings se_resolved_{};   // what configuration last left there\n";
    o_ << "    Sim::Outputs  se_outputs_{};    // copied out of `sig` every tick\n\n";

    o_ << "    // ---- the model's whole data surface " << std::string(36, '-') << "\n";
    o_ << "    //  x    continuous state — the external solver's ABI\n";
    o_ << "    //  sig  every wire value and every root boundary input\n";
    bool any_discrete = false;
    for (const Leaf& leaf : m_.leaves) any_discrete = any_discrete || leaf.has_discrete();
    if (any_discrete) o_ << "    //  dis  every discrete state\n";
    bool any_finite = false;
    for (const Leaf& leaf : m_.leaves) any_finite = any_finite || leaf.has_finite();
    if (any_finite) o_ << "    //  fin  every finite state\n";
    o_ << "    //  Together they are a complete checkpoint. The *_map() tables below\n";
    o_ << "    //  give path, offset and unit, so an inspector, a shared-memory host or\n";
    o_ << "    //  a replay tool needs no generated accessor code at all.\n\n";

    if (!m_.dynamic_states) {
        o_ << "    static constexpr std::size_t n_states = " << m_.n_states << ";\n";
        for (const Leaf& leaf : m_.leaves)
            for (const StateSlot& s : leaf.states)
                if (is_cont(s))
                    o_ << "    //  x[" << s.slot << "] / xd[" << s.slot << "]   "
                       << col(leaf.path + "." + s.name, 24) << s.unit.str() << "\n";
        o_ << "    std::array<double, n_states> x{};\n";
        o_ << "    std::array<double, n_states> xd{};\n\n";
    } else {
        // §15.5a — the dynamic lowering. Two differences from the form
        // above and only two: the size is settled at configuration rather than
        // at compile time, and the storage is the host's. Everything else in
        // this class — `sig`, `dis`/`nxt`, the schedule, every wire — is
        // byte-for-byte what it would otherwise have been.
        o_ << "    //  A state's position is a configuration result, so what is fixed\n";
        o_ << "    //  here is only its ROW in the tables below. An array-shaped state\n";
        o_ << "    //  occupies a contiguous run of slots in the SAME block: there is no\n";
        o_ << "    //  second continuous vector, because the solver's ABI is the whole\n";
        o_ << "    //  point of the first one.\n";
        for (const Leaf& leaf : m_.leaves)
            for (const StateSlot& st : leaf.states) {
                if (!is_cont(st)) continue;
                const std::string p =
                    leaf.path.empty() ? st.name : leaf.path + "." + st.name;
                o_ << "    //  x[se_x_off_[" << st.off << "]]"
                   << (st.extent.present ? ".." : "  ") << "   "
                   << col(p + (st.extent.present ? " " + st.extent.text : ""), 30)
                   << st.unit.str() << "\n";
            }
        o_ << "    std::size_t n_states = 0;                 // §6.2b step 4\n";
        o_ << "    double*     x  = nullptr;                 // host-provided, §15.5a\n";
        o_ << "    double*     xd = nullptr;\n";
        o_ << "    std::array<std::size_t, " << m_.n_cont_slots << "> se_x_off_{};\n";
        o_ << "    std::array<std::size_t, " << m_.n_cont_slots << "> se_x_len_{};\n";
        o_ << "    bool        se_bound_x_ = false;\n\n";
    }

    // A wire coincides with its producer's `Out` storage (§15.3); that storage
    // lives here rather than as a loose member, so the whole signal set has one
    // address and one size. ZOH still falls out for free: a slow node's slice
    // is simply not written on the ticks it does not sample.
    o_ << "    struct Signals {\n";
    // The root inputs are the public struct itself, so Sim::inputs() hands the
    // host a reference into this block and nothing is copied per tick.
    o_ << "        Sim::Inputs in;   // the host writes these, through Sim::inputs()\n";
    {
        std::size_t w = 0;
        for (const Leaf& leaf : m_.leaves)
            w = std::max(w, leaf.node->cpp_name.size() + 5);
        for (const Leaf& leaf : m_.leaves)
            o_ << "        " << pad(leaf.node->cpp_name + "::Out", w) << " " << leaf.ident
               << ";\n";
    }
    o_ << "    };\n    Signals sig{};\n\n";

    if (any_discrete) {
        o_ << "    struct Discrete {\n";
        std::size_t w = 0;
        for (const Leaf& leaf : m_.leaves)
            if (leaf.has_discrete()) w = std::max(w, leaf.node->cpp_name.size() + 7);
        for (const Leaf& leaf : m_.leaves)
            if (leaf.has_discrete())
                o_ << "        " << pad(leaf.node->cpp_name + "::Store", w) << " "
                   << leaf.ident << ";\n";
        o_ << "    };\n";
        o_ << "    Discrete dis{};   // x+ as committed\n";
        o_ << "    Discrete nxt{};   // what next() writes; the commit is dis = nxt\n\n";
    }

    if (any_finite) {
        // §6.4c / §15.5 — the finite block and its buffer.
        o_ << "    struct Finite {\n";
        std::size_t w = 0;
        for (const Leaf& leaf : m_.leaves)
            if (leaf.has_finite()) w = std::max(w, leaf.node->cpp_name.size() + 5);
        for (const Leaf& leaf : m_.leaves)
            if (leaf.has_finite())
                o_ << "        " << pad(leaf.node->cpp_name + "::Fin", w) << " " << leaf.ident
                   << ";\n";
        o_ << "    };\n";
        o_ << "    Finite fin{};   // every finite state, as committed\n";
        o_ << "    Finite fnx{};   // what next() writes; the commit is fin = fnx\n\n";
    }

    if (m_.n_disc_arr_slots) {
        // §15.5a — the fourth block, and it exists for exactly one reason:
        // a run-time-sized member in `Discrete` would destroy the `offsetof`
        // property the table above depends on. Array states being `double`-only
        // (§6.4a) is what lets this be a flat block instead.
        o_ << "    //  Array-shaped discrete states. `dis` stays a plain struct;\n";
        o_ << "    //  these get the same address-size-table treatment as `x`.\n";
        for (const Leaf& leaf : m_.leaves)
            for (const StateSlot& st : leaf.states) {
                if (!is_disc(st) || !st.extent.present) continue;
                const std::string p =
                    leaf.path.empty() ? st.name : leaf.path + "." + st.name;
                o_ << "    //  dis_arr[se_da_off_[" << st.off << "]]..   "
                   << col(p + " " + st.extent.text, 30) << st.unit.str() << "\n";
            }
        o_ << "    std::size_t n_dstates = 0;\n";
        o_ << "    double*     dis_arr = nullptr;            // host-provided, §15.5a\n";
        o_ << "    double*     nxt_arr = nullptr;\n";
        o_ << "    std::array<std::size_t, " << m_.n_disc_arr_slots << "> se_da_off_{};\n";
        o_ << "    std::array<std::size_t, " << m_.n_disc_arr_slots << "> se_da_len_{};\n";
        o_ << "    bool        se_bound_da_ = false;\n\n";
    }

    o_ << "    static const se_rt::Slot* signal_map(std::size_t& count);\n";
    if (!discrete_rows().empty())
        o_ << "    static const se_rt::Slot* discrete_map(std::size_t& count);\n";
    if (!finite_rows().empty())
        o_ << "    static const se_rt::Slot* finite_map(std::size_t& count);\n";
    if (m_.dynamic_states) {
        // §15.6 — not static, and it cannot be: `sec` cannot print
        // `x[0] bw.w1[0]` for a state whose extent is not known until
        // configuration. The manifest prints the DECLARATION; these build the
        // concrete rows once configuration has settled.
        o_ << "    const se_rt::Slot* state_map(std::size_t& count) const;\n";
        if (m_.n_disc_arr_slots)
            o_ << "    const se_rt::Slot* discrete_array_map(std::size_t& count) const;\n";
    }
    o_ << "    void derivatives(double t);              // reads x, fills xd\n\n";

    o_ << "    // ---- machinery " << std::string(57, '-') << "\n";
    // The run every leaf points at: clock, stop flag, contracts, frame and log.
    // Declared ahead of the leaves because their initialisers take its address,
    // and owned by the Sim rather than by the process so two models never share
    // any of it.
    o_ << "    se_rt::Run se_run_{};   // this model's run: clock, control, checks, log\n";
    o_ << "    //  Leaf instances. Their settings are NOT baked in here: they are\n";
    o_ << "    //  assigned by resolve_settings() (§6.2b), so a settings source can\n";
    o_ << "    //  move them in a binary that has already been compiled. Any unit\n";
    o_ << "    //  CONVERSION still folded at elaboration, which is why nothing here\n";
    o_ << "    //  needs conversion machinery.\n";
    std::size_t wp = 0, wt = 0;
    for (const Leaf& leaf : m_.leaves) {
        wp = std::max(wp, quote(leaf.path.empty() ? "<root>" : leaf.path).size() + 1);
        wt = std::max(wt, quote(leaf.node->fq).size() + 1);
    }
    for (const Leaf& leaf : m_.leaves) {
        const std::string path = quote(leaf.path.empty() ? "<root>" : leaf.path);
        o_ << "    " << pad(leaf.node->cpp_name, 28) << " " << pad(leaf.ident + "{{}, ", 18)
           << pad(path + ",", wp) << " " << pad(quote(leaf.node->fq) + ",", wt)
           << " &se_run_};\n";
    }

    bool any_der = false;
    for (const Leaf& leaf : m_.leaves) any_der = any_der || leaf.has_continuous();
    if (any_der) {
        o_ << "\n    // `der` is scratch, not node data: evaluated once per solver stage\n";
        o_ << "    // at trial states the trajectory never visits, then discarded.\n";
        std::size_t w = 0;
        for (const Leaf& leaf : m_.leaves)
            if (leaf.has_continuous()) w = std::max(w, leaf.node->cpp_name.size() + 5);
        for (const Leaf& leaf : m_.leaves)
            if (leaf.has_continuous())
                o_ << "    " << pad(leaf.node->cpp_name + "::Der", w) << " " << leaf.ident
                   << "_der;\n";
    }

    // §15.5 — next()'s views, and the buffer behind its continuous half. Each
    // view is bound once, into `nxt` / `fnx` / `xnx`, and lives here rather than
    // in the leaf because the out-parameter has to be non-const at the call
    // site while `next()` is a const member.
    if (any_next()) {
        std::size_t w = 0;
        for (const Leaf& leaf : m_.leaves)
            if (has_next(leaf) && !leaf.states.empty())
                w = std::max(w, leaf.node->cpp_name.size() + 6);
        if (w) o_ << "\n    // next()'s views: `next.<s>` lands in a buffer, never in the state.\n";
        for (const Leaf& leaf : m_.leaves)
            if (has_next(leaf) && !leaf.states.empty())
                o_ << "    " << pad(leaf.node->cpp_name + "::Next", w) << " " << leaf.ident
                   << "_next;\n";
    }
    if (jumps_continuous()) {
        if (m_.dynamic_states)
            o_ << "    std::vector<double> se_xnx_;   // x+ being written; sized by bind_state()\n";
        else
            o_ << "    std::array<double, n_states> xnx{};   // x+ being written; the commit is x = xnx\n";
    }

    // §6.2b — a composite's settings, the one runtime trace a composite leaves
    // (§15.2). They hold no signal: they are the inputs to its flow-down
    // expressions and the target of a path-addressed override.
    {
        std::set<std::string> done;
        for (const Leaf& leaf : m_.leaves) done.insert(leaf.path);
        for (const SettingSlot& s : m_.settings) {
            if (!done.insert(s.owner).second) continue;
            std::vector<Row> crows;
            // §6.2c — one member per SETTING, not per leaf. A record setting
            // carries several leaves whose `name` is a dotted path into it
            // (`offset.x`); that addresses a member but cannot declare one, so
            // the struct declares the record once and the leaves reach into it,
            // exactly as a leaf node's `Param` does.
            std::set<std::string> seen;
            for (const SettingSlot& t : m_.settings) {
                if (t.owner != s.owner) continue;
                if (!seen.insert(t.decl).second) continue;
                Row r;
                r.type = cpp_type(t.decl_type);
                r.decl = t.decl + "{};";
                // A record's fields carry their own units, and they are in
                // `[types]`; naming one here would be naming the wrong thing.
                r.comment = t.decl_type.is_record ? "" : "// " + t.unit.str();
                crows.push_back(r);
            }
            o_ << "\n    struct {\n";
            for (const Row& r : crows)
                o_ << "        " << pad(r.type, 8) << " "
                   << (r.comment.empty() ? r.decl : col(r.decl, 20) + r.comment)
                   << "\n";
            o_ << "    } " << param_base(s.owner) << ";   // "
               << (s.owner.empty() ? std::string("<root>") : s.owner) << "\n";
        }
    }

    // One flag per setting, indexed exactly as `m_.settings` is. A §14 source
    // sets the flag for each path it binds, and `resolve_settings` then leaves
    // that setting alone (§13.3). One array rather than a named member each:
    // the loader has to reach a setting by index anyway.
    o_ << "\n    std::array<bool, " << m_.settings.size()
       << "> se_pin_{};   // pinned by a settings source, §6.2b\n";
    // §16.4 — the first configuration failure, recorded rather than raised.
    // resolve_settings() runs from the constructor, where there is no run yet
    // to fail; init() turns this into sim.fail() before any node starts.
    o_ << "\n    std::string   se_cfg_error_;\n";

    // §13.4 / §14.1 — the initial conditions are data rather than literals in
    // apply_initial_conditions(), so a settings source loaded into a compiled
    // model can move them. Each starts as what elaboration computed, sim-file
    // overrides included.
    {
        const std::vector<LoadRow> rows = load_rows();
        std::vector<Row> ics;
        for (const LoadRow& r : rows) {
            if (r.kind != LoadRow::Kind::Initial) continue;
            Row x;
            x.type = r.state->enm ? r.state->enm->cpp_name : r.state->scalar;
            x.decl = ic_member(*r.leaf, *r.state) + " = " +
                     literal(r.state->initial, r.state->scalar, r.state->enm) + ";";
            x.comment = "// " + r.state->unit.str();
            ics.push_back(x);
        }
        if (!ics.empty()) {
            std::size_t wty = 0, wd = 0;
            for (const Row& x : ics) {
                wty = std::max(wty, x.type.size());
                wd = std::max(wd, x.decl.size());
            }
            o_ << "\n    // The initial conditions (§13.4), as data so a settings source can\n";
            o_ << "    // move them (§14.1). apply_initial_conditions() writes them in.\n";
            o_ << "    struct InitialConditions {\n";
            for (const Row& x : ics)
                o_ << "        " << pad(x.type, wty) << " " << col(x.decl, wd + 2) << x.comment
                   << "\n";
            o_ << "    } se_ic_;\n";
        }
        bool any_element = false;
        for (const LoadRow& r : rows) any_element = any_element || r.kind == LoadRow::Kind::Element;
        if (any_element) {
            o_ << "    // An array state's initial conditions beyond its uniform default, one\n";
            o_ << "    // element each: {row in setting_rows(), index, value}.\n";
            o_ << "    std::vector<se_rt::Assignment> se_ic_elements_{";
            bool first = true;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (rows[i].kind != LoadRow::Kind::Element) continue;
                for (const StateSlot::ElementIC& ic : rows[i].state->element_ic) {
                    o_ << (first ? "\n" : ",\n") << "        {" << i << ", " << ic.index << ", "
                       << dbl(ic.value) << "}";
                    first = false;
                }
            }
            o_ << (first ? "};\n" : "};\n");
        } else {
            o_ << "    std::vector<se_rt::Assignment> se_ic_elements_;   // no array states\n";
        }
    }
    o_ << "    std::string se_load_error_;   // a refused settings source; it stands (§14.1)\n";
    o_ << "    std::string se_dump_;         // what dump_settings() last returned\n";
    o_ << "    bool        se_started_ = false;\n";
    o_ << "    std::string se_rerun_error_;  // a second init() on this Sim, refused\n";
    o_ << "\n    double        se_time_ = 0.0;\n";
    o_ << "    std::uint64_t se_tick_ = 0;\n";
    o_ << "    double        se_duration_ = " << dbl(m_.duration) << ";   // s, sim.duration (§13.3)\n";
    o_ << "    std::uint64_t se_end_tick_ = " << tick_count() << ";   // the last tick it allows\n";
    o_ << "    std::size_t   se_inited_ = 0;\n";
    o_ << "    bool          se_done_ = false;\n";
    o_ << "\n    // §13.3 — the run's own configuration: what is recorded, where and how\n";
    o_ << "    // often, and the log. The .sim's entries, edited by every source loaded.\n";
    o_ << "    se_rt::RunConfig    se_config_ = default_config();\n";
    o_ << "    se_rt::Recorder     se_rec_;\n";
    o_ << "    std::vector<double> se_row_;          // one row's values, reused every row\n";
    o_ << "    std::string         se_list_;         // what list_signals() last returned\n";
    o_ << "    std::string         se_call_error_;   // the last refused configuration call\n";
    o_ << "    std::string         se_stamp_;        // the run's `{time}`, taken at init() (§13.5)\n";
    o_ << "    std::vector<se_rt::Column> se_handles_;   // channel_handle()'s, by number (§14.1)\n";
    o_ << "    std::vector<double> se_handle_vals_;  // each as the last tick sampled it\n";
    o_ << "    bool                se_ticked_ = false;\n";

    if (m_.dynamic_states) {
        // Solver scratch, sized once at configuration. It is not a published
        // block — no host binds it and nothing external names it — so the
        // node-private rule of §6.5 applies and the model owns it. The point
        // that matters for §9.4 is that it never grows inside a tick.
        if (m_.n_cont_slots && m_.solver != "euler") {
            o_ << "\n    std::vector<double> se_x0_, se_k1_";
            if (m_.solver != "rk2") o_ << ", se_k2_, se_k3_";
            o_ << ";   // solver scratch, sized at configuration\n";
        } else if (m_.n_cont_slots) {
            o_ << "\n    std::vector<double> se_x0_;   // solver scratch\n";
        }
        // §15.6 — the concrete slot rows, built at configuration. The paths
        // are owned because `bw.w1[3]` is a string that does not exist until
        // then; `se_rt::Slot` holds a `const char*` into this store.
        o_ << "\n    std::vector<std::string>  se_slot_paths_;\n";
        o_ << "    std::vector<se_rt::Slot>  se_state_slots_;\n";
        if (m_.n_disc_arr_slots)
            o_ << "    std::vector<se_rt::Slot>  se_dstate_slots_;\n";
    }

    // For a dynamic model the buffers are the host's (§15.5a), and the public
    // Sim is that host: start() sizes these and binds them.
    if (m_.dynamic_states) {
        o_ << "\n    std::vector<double> se_x_buf_, se_xd_buf_;   // bound by start()\n";
        if (m_.n_disc_arr_slots)
            o_ << "    std::vector<double> se_d_buf_, se_dn_buf_;\n";
    }

    // Everything below is defined after the class, so the class body reads as
    // an interface and a data layout rather than as a wall of code.
    o_ << "\n    void configure_run();\n";
    o_ << "    std::string accept_settings();\n";
    o_ << "    void publish_settings();\n";
    o_ << "    void publish_outputs();\n";
    o_ << "\n    // §14.1 — a settings source, in and out. setting_rows() describes every\n";
    o_ << "    // path a source may bind; assign() and value_of() move one row's value.\n";
    o_ << "    static const se_rt::SettingRow* setting_rows(std::size_t& count);\n";
    o_ << "    bool        load_settings(const char* text, const char* origin);\n";
    o_ << "    const char* dump_settings();\n";
    o_ << "    std::string settle();\n";
    o_ << "    void        assign(const se_rt::Assignment& a);\n";
    o_ << "    double      value_of(std::size_t row) const;\n";
    {
        bool any_element = false;
        for (const LoadRow& r : load_rows())
            any_element = any_element || r.kind == LoadRow::Kind::Element;
        if (any_element) {
            o_ << "    std::size_t element_extent(std::size_t row) const;\n";
            o_ << "    double*     element_base(std::size_t row);\n";
        }
    }
    o_ << "\n    void resolve_settings();\n";
    o_ << "\n    void bind();\n";
    o_ << "    void apply_initial_conditions();\n";
    o_ << "    void output_pass();\n";
    o_ << "    void output_pass_minor();\n";
    if (any_next()) o_ << "    void seed_next();\n    void next_tick();\n";
    o_ << "    void integrate(double t, double h);\n";
    o_ << "    void final_all(const se_rt::RunContext& ctx) const;\n";
    o_ << "\n    // §13.5, §14.1 — the channels, and the calls that configure the run.\n";
    o_ << "    static se_rt::RunConfig default_config();\n";
    o_ << "    static se_rt::Tables    tables();\n";
    o_ << "    double      channel(std::size_t row, std::size_t i);\n";
    o_ << "    std::size_t channel_extent(std::size_t row);\n";
    o_ << "    void        check_columns();\n";
    o_ << "    void        record_open();\n";
    o_ << "    void        record_row();\n";
    o_ << "    const char* list_signals();\n";
    o_ << "    std::vector<Sim::ChannelInfo> channels_list();\n";
    o_ << "    bool        configuring(const char* call);\n";
    o_ << "    bool        record(const char* path, bool add);\n";
    o_ << "    void        record_clear();\n";
    o_ << "    bool        record_file(const char* file);\n";
    o_ << "    bool        record_every(std::uint64_t n);\n";
    o_ << "    bool        log_file(const char* file);\n";
    o_ << "    bool        log_level(const char* path, const char* level);\n";
    o_ << "    const char* record_file_name() const;\n";
    o_ << "    const char* log_file_name() const;\n";
    o_ << "    int         channel_handle(const char* path);\n";
    o_ << "    double      read(int handle);\n";
    o_ << "    double      handle_value(const se_rt::Column& c);\n";
    o_ << "    void        sample_handles();\n";
    o_ << "    std::vector<Sim::SettingInfo> settings_list();\n";
    o_ << "    double      get(const char* path);\n";
    o_ << "    bool        set(const char* path, double value);\n";
    o_ << "    bool        set(const char* path, const char* value);\n";
    o_ << "    bool        set_text(const std::string& path, const std::string& value);\n";
}

// The C++ object holding a node's settings. A leaf keeps them in its own
// `param` member; a composite has no runtime object of its own (§15.2), so it
// gets a bare struct that exists only to source its flow-down expressions and
// to be the target of a path-addressed override (§6.9.1, §13.2).
void Emitter::build_param_bases() {
    for (const Leaf& leaf : m_.leaves) param_base_[leaf.path] = leaf.ident + ".param";
    for (const SettingSlot& s : m_.settings) {
        if (param_base_.count(s.owner)) continue;
        std::string id = "se_cfg_";
        if (s.owner.empty()) id += "root";
        for (char c : s.owner) id += (c == '.') ? '_' : c;
        param_base_[s.owner] = id;
    }
}

std::string Emitter::param_base(const std::string& owner) const {
    auto it = param_base_.find(owner);
    return it == param_base_.end() ? std::string("se_unresolved") : it->second;
}

// One §7 expression as one C++ expression (§15.5). The tokens were built by the
// same walk that evaluated it, so nothing is re-derived here: a reference only
// needs the identifier its owner ended up with.
std::string Emitter::render_expr(const ExprCode& c) const {
    std::string r;
    for (const ExprTok& t : c)
        r += (t.kind == ExprTok::Kind::Text) ? t.text
                                             : param_base(t.owner) + "." + t.text;
    return r;
}

// §6.2b — the configuration program. Straight-line code in dependency order:
// the sort happened at elaboration, so the runtime holds no graph and does no
// ordering work. A setting a §14 source bound is PINNED and its expression is
// skipped, which is how a deep override detaches from the flow-down (§13.3).
void Emitter::sim_resolve_settings() {
    o_ << "\n// Settings (§6.2b). Re-runnable: start() calls it after pinning the host's\n";
    o_ << "// edits, and every derived value follows, exactly as at elaboration.\n";
    o_ << "void Sim::Impl::resolve_settings() {\n";
    std::size_t w = 0;
    for (const SettingSlot& s : m_.settings)
        w = std::max(w, param_base(s.owner).size() + s.name.size() + 1);
    for (std::size_t i = 0; i < m_.settings.size(); ++i) {
        const SettingSlot& s = m_.settings[i];
        const std::string lhs = param_base(s.owner) + "." + s.name;
        // §7 evaluates in `double`, so every emitted expression is a `double`
        // expression -- but §5.1 lets a setting be declared `int`, `float` or
        // `bool`. The narrowing has to be SPELLED: an implicit conversion here
        // is C4244 under /W4, and §12.4 promises a warning-free build. The cast
        // performs exactly the truncation elaboration already applied, so it
        // changes no value -- it only stops the C++ compiler guessing that it
        // might.
        //
        // An enum needs neither (§7.2a): its expression is an enumerator or a
        // copy of a setting of the same enum, already of the member's type.
        const std::string rhs =
            s.scalar == "double" || s.enm
                ? render_expr(s.expr)
                : "static_cast<" + s.scalar + ">(" + render_expr(s.expr) + ")";
        o_ << "    if (!se_pin_[" << i << "]) " << pad(lhs, w) << " = "
           << rhs << ";"
           << "   // " << s.unit.str() << "\n";
    }

    o_ << "}\n";
}

// §6.2b step 4 — the sizes. Emitted only for a model with array-shaped
// states (§15.5a); every other model settles its blocks at compile time and
// has nothing to do here.
//
// Every extent is one §7 expression, emitted exactly as a setting's is, then
// put through the same non-negative-integer rule the DSL compiler applied to
// what it could see. A state's position falls out of the running sum, so an
// array state occupies a contiguous run and a scalar one is a run of length
// one: two shapes, one table, and no second code path to disagree with the
// first.
void Emitter::sim_configure() {
    if (!m_.dynamic_states) return;

    o_ << "\n// §6.2b steps 1-4. Re-runnable, and it must be re-run after a change to\n";
    o_ << "// any setting an extent reads — that is what moves the blocks.\n";
    o_ << "void Sim::Impl::configure() {\n";
    o_ << "    se_cfg_error_.clear();\n";
    o_ << "    resolve_settings();\n\n";

    o_ << "    std::size_t se_n = 0;\n";
    for (const Leaf& leaf : m_.leaves)
        for (const StateSlot& st : leaf.states) {
            if (!is_cont(st)) continue;
            const std::string path = leaf.path.empty() ? st.name : leaf.path + "." + st.name;
            const std::string len = "se_x_len_[" + std::to_string(st.off) + "]";
            const std::string off = "se_x_off_[" + std::to_string(st.off) + "]";
            if (!st.extent.present) {
                o_ << "    " << pad(len + " = 1;", 22) << pad(off + " = se_n;", 26)
                   << "se_n += 1;   // " << path << "\n";
            } else {
                o_ << "    {   // " << path << " " << st.extent.text << "\n";
                o_ << "        const double e = " << render_expr(st.extent.expr) << ";\n";
                o_ << "        if (!se_rt::extent_of(e, " << len
                   << ") && se_cfg_error_.empty())\n";
                o_ << "            se_cfg_error_ = se_rt::extent_error(" << quote(path)
                   << ", e);\n";
                o_ << "        " << off << " = se_n;\n";
                o_ << "        se_n += " << len << ";\n";
                o_ << "    }\n";
            }
        }
    o_ << "    n_states = se_n;\n";

    if (m_.n_disc_arr_slots) {
        o_ << "\n    se_n = 0;\n";
        for (const Leaf& leaf : m_.leaves)
            for (const StateSlot& st : leaf.states) {
                if (!is_disc(st) || !st.extent.present) continue;
                const std::string path =
                    leaf.path.empty() ? st.name : leaf.path + "." + st.name;
                const std::string len = "se_da_len_[" + std::to_string(st.off) + "]";
                const std::string off = "se_da_off_[" + std::to_string(st.off) + "]";
                o_ << "    {   // " << path << " " << st.extent.text << "\n";
                o_ << "        const double e = " << render_expr(st.extent.expr) << ";\n";
                o_ << "        if (!se_rt::extent_of(e, " << len
                   << ") && se_cfg_error_.empty())\n";
                o_ << "            se_cfg_error_ = se_rt::extent_error(" << quote(path)
                   << ", e);\n";
                o_ << "        " << off << " = se_n;\n";
                o_ << "        se_n += " << len << ";\n";
                o_ << "    }\n";
            }
        o_ << "    n_dstates = se_n;\n";
    }

    bool any_var = false;
    for (const Leaf& leaf : m_.leaves) any_var = any_var || !leaf.array_vars.empty();
    if (any_var) {
        o_ << "\n    // §6.5 — the one allocation in the lowering. A var is private\n";
        o_ << "    // storage outside every published block, so the node sizes its own;\n";
        o_ << "    // it happens here, once, and never inside a tick (Appendix C 8a).\n";
        for (const Leaf& leaf : m_.leaves)
            for (const ArrayVar& av : leaf.array_vars) {
                const std::string path =
                    leaf.path.empty() ? av.name : leaf.path + "." + av.name;
                o_ << "    {\n";
                o_ << "        const double e = " << render_expr(av.extent.expr) << ";\n";
                o_ << "        std::size_t n = 0;\n";
                o_ << "        if (!se_rt::extent_of(e, n) && se_cfg_error_.empty())\n";
                o_ << "            se_cfg_error_ = se_rt::extent_error(" << quote(path)
                   << ", e);\n";
                o_ << "        " << leaf.ident << ".var." << av.name
                   << ".assign(n, 0.0);   // " << path << " " << av.extent.text << "\n";
                o_ << "    }\n";
            }
    }

    if (m_.n_cont_slots) {
        o_ << "\n    se_x0_.assign(n_states, 0.0);\n";
        if (m_.solver == "rk2") {
            o_ << "    se_k1_.assign(n_states, 0.0);\n";
        } else if (m_.solver != "euler") {
            o_ << "    se_k1_.assign(n_states, 0.0);\n";
            o_ << "    se_k2_.assign(n_states, 0.0);\n";
            o_ << "    se_k3_.assign(n_states, 0.0);\n";
        }
    }

    sim_configure_slots();
    sim_configure_indices();

    bool any_element = false;
    for (const LoadRow& r : load_rows())
        any_element = any_element || r.kind == LoadRow::Kind::Element;
    if (any_element) {
        o_ << "\n    // §13.4 / §14.1 — the element initial conditions, wherever they came\n";
        o_ << "    // from, against the extents just computed.\n";
        o_ << "    for (const se_rt::Assignment& a : se_ic_elements_) {\n";
        o_ << "        const std::size_t n = element_extent(a.row);\n";
        o_ << "        if (a.index >= n && se_cfg_error_.empty()) {\n";
        o_ << "            std::size_t count = 0;\n";
        o_ << "            const std::string path = std::string(setting_rows(count)[a.row].path) +\n";
        o_ << "                                     \"[\" + std::to_string(a.index) + \"]\";\n";
        o_ << "            se_cfg_error_ = se_rt::index_error(path.c_str(), n);\n";
        o_ << "        }\n";
        o_ << "    }\n";
    }

    o_ << "\n    // A rebind is not defined (§15.5a), but a RE-configure invalidates\n";
    o_ << "    // the buffers it sized, so the flags go back down and init() says so.\n";
    o_ << "    se_bound_x_ = false;\n";
    o_ << "    x = nullptr;\n    xd = nullptr;\n";
    if (m_.n_disc_arr_slots) {
        o_ << "    se_bound_da_ = false;\n";
        o_ << "    dis_arr = nullptr;\n    nxt_arr = nullptr;\n";
    }
    o_ << "}\n";
}

// §15.6 — the concrete slot rows. `sec` cannot print `x[0] bw.w1[0]` for a
// state whose extent is not known until now, so the manifest prints the
// declaration and this builds what it actually became. The paths are built here
// for the same reason: `bw.w1[3]` is a string that did not exist at generation
// time, and `se_rt::Slot` holds a `const char*` into the store below.
void Emitter::sim_configure_slots() {
    o_ << "\n    // §15.6 — the slot map, now that the extents have settled.\n";
    o_ << "    se_slot_paths_.clear();\n";
    o_ << "    se_slot_paths_.reserve(n_states"
       << (m_.n_disc_arr_slots ? " + n_dstates" : "") << ");\n";

    auto push_paths = [&](bool continuous) {
        for (const Leaf& leaf : m_.leaves)
            for (const StateSlot& st : leaf.states) {
                if (is_cont(st) != continuous) continue;
                if (!continuous && !st.extent.present) continue;
                const std::string path =
                    leaf.path.empty() ? st.name : leaf.path + "." + st.name;
                const std::string len = (continuous ? "se_x_len_[" : "se_da_len_[") +
                                        std::to_string(st.off) + "]";
                if (!st.extent.present) {
                    o_ << "    se_slot_paths_.push_back(" << quote(path) << ");\n";
                } else {
                    o_ << "    for (std::size_t i = 0; i < " << len << "; ++i)\n";
                    o_ << "        se_slot_paths_.push_back(std::string("
                       << quote(path + "[") << ") + std::to_string(i) + " << quote("]")
                       << ");\n";
                }
            }
    };
    push_paths(true);
    if (m_.n_disc_arr_slots) push_paths(false);

    auto push_slots = [&](bool continuous, const char* store) {
        o_ << "    " << store << ".clear();\n";
        o_ << "    " << store << ".reserve(" << (continuous ? "n_states" : "n_dstates")
           << ");\n";
        for (const Leaf& leaf : m_.leaves)
            for (const StateSlot& st : leaf.states) {
                if (is_cont(st) != continuous) continue;
                if (!continuous && !st.extent.present) continue;
                const std::string off = (continuous ? "se_x_off_[" : "se_da_off_[") +
                                        std::to_string(st.off) + "]";
                const std::string len = (continuous ? "se_x_len_[" : "se_da_len_[") +
                                        std::to_string(st.off) + "]";
                const std::string base = continuous ? off : "n_states + " + off;
                o_ << "    for (std::size_t i = 0; i < " << len << "; ++i)\n";
                o_ << "        " << store << ".push_back(se_rt::Slot{\n";
                o_ << "            se_slot_paths_[" << base << " + i].c_str(),\n";
                o_ << "            (" << off << " + i) * sizeof(double), sizeof(double),\n";
                o_ << "            " << quote("double") << ", " << quote(st.unit.str())
                   << "});\n";
            }
    };
    push_slots(true, "se_state_slots_");
    if (m_.n_disc_arr_slots) push_slots(false, "se_dstate_slots_");
}

// §13.4 / §13.5 — every element index the model names, checked where the
// extent finally exists. An index past it is a configuration failure and not a
// compile error: the same settings source may be valid against one `order` and
// invalid against another.
void Emitter::sim_configure_indices() {
    if (m_.element_refs.empty()) return;
    o_ << "\n    // §13.4 / §13.5 — the element indices this model names.\n";
    for (const Model::ElementRef& r : m_.element_refs) {
        const std::string len = (r.continuous ? "se_x_len_[" : "se_da_len_[") +
                                std::to_string(r.off) + "]";
        o_ << "    if (" << r.index << " >= " << len << " && se_cfg_error_.empty())\n";
        o_ << "        se_cfg_error_ = se_rt::index_error(" << quote(r.path) << ", " << len
           << ");\n";
    }
}

// §15.5a — the handshake. Handing the address IN rather than reading it out
// is the `x` ABI from the other end, and it is what CVODE, odeint and FMI ask
// for anyway: a host that wants `x` in shared memory, in a ring buffer or
// inside its own arena no longer has to copy to get it.
void Emitter::sim_binders() {
    if (!m_.dynamic_states) return;

    o_ << "\nvoid Sim::Impl::bind_state(double* px, double* pxd, std::size_t n) {\n";
    o_ << "    if (n != n_states) {\n";
    o_ << "        se_cfg_error_ = se_rt::bind_error(" << quote("bind_state")
       << ", n, n_states);\n        return;\n    }\n";
    o_ << "    x = px;\n    xd = pxd;\n";
    o_ << "    for (std::size_t i = 0; i < n; ++i) { x[i] = 0.0; xd[i] = 0.0; }\n";
    if (jumps_continuous()) o_ << "    se_xnx_.assign(n, 0.0);\n";
    for (const Leaf& leaf : m_.leaves)
        for (const StateSlot& st : leaf.states) {
            if (!is_cont(st)) continue;
            const std::string off = "se_x_off_[" + std::to_string(st.off) + "]";
            const std::string len = "se_x_len_[" + std::to_string(st.off) + "]";
            if (st.extent.present) {
                o_ << "    " << leaf.ident << ".state." << st.name << ".bind(x + " << off
                   << ", " << len << ");\n";
                o_ << "    " << leaf.ident << "_der." << st.name << ".bind(xd + " << off
                   << ", " << len << ");\n";
                if (has_next(leaf))
                    o_ << "    " << leaf.ident << "_next." << st.name
                       << ".bind(se_xnx_.data() + " << off << ", " << len << ");\n";
            } else {
                o_ << "    " << leaf.ident << ".state." << st.name << ".bind(x[" << off
                   << "]);\n";
                o_ << "    " << leaf.ident << "_der." << st.name << ".bind(xd[" << off
                   << "]);\n";
                if (has_next(leaf))
                    o_ << "    " << leaf.ident << "_next." << st.name << ".bind(se_xnx_[" << off
                       << "]);\n";
            }
        }
    o_ << "    se_bound_x_ = true;\n}\n";

    if (!m_.n_disc_arr_slots) return;
    o_ << "\nvoid Sim::Impl::bind_discrete_array(double* pd, double* pn, "
          "std::size_t n) {\n";
    o_ << "    if (n != n_dstates) {\n";
    o_ << "        se_cfg_error_ = se_rt::bind_error(" << quote("bind_discrete_array")
       << ", n, n_dstates);\n        return;\n    }\n";
    o_ << "    dis_arr = pd;\n    nxt_arr = pn;\n";
    o_ << "    for (std::size_t i = 0; i < n; ++i) { dis_arr[i] = 0.0; nxt_arr[i] = 0.0; }\n";
    for (const Leaf& leaf : m_.leaves)
        for (const StateSlot& st : leaf.states) {
            if (!is_disc(st) || !st.extent.present) continue;
            const std::string off = "se_da_off_[" + std::to_string(st.off) + "]";
            const std::string len = "se_da_len_[" + std::to_string(st.off) + "]";
            o_ << "    dis." << leaf.ident << "." << st.name << ".bind(dis_arr + " << off
               << ", " << len << ");\n";
            o_ << "    nxt." << leaf.ident << "." << st.name << ".bind(nxt_arr + " << off
               << ", " << len << ");\n";
            // The node's own view reads the COMMITTED side, exactly as a
            // scalar discrete state's proxy binds into `dis` and not `nxt`.
            o_ << "    " << leaf.ident << ".state." << st.name << ".bind(dis_arr + " << off
               << ", " << len << ");\n";
            o_ << "    " << leaf.ident << "_next." << st.name << ".bind(nxt_arr + " << off
               << ", " << len << ");\n";
        }
    o_ << "    se_bound_da_ = true;\n}\n";
}

void Emitter::sim_elaborate() {
    // Settings now arrive through the leaf member initializers, so there is no
    // elaborate() left to emit. What remains is the declarative ICs.
    o_ << "\n// Declarative state defaults (§6.4), as se_ic_ holds them: every override\n";
    o_ << "// folded in, from the sim file or a loaded settings source (§14.1) — the\n";
    o_ << "// single override channel, which is what a discrete state buys over a var.\n";
    o_ << "// A continuous write lands in x through the proxy.\n";
    o_ << "void Sim::Impl::apply_initial_conditions() {\n";
    std::size_t w = 0;
    for (const Leaf& leaf : m_.leaves) {
        for (const StateSlot& s : leaf.states)
            w = std::max(w, leaf.ident.size() + s.name.size() + (is_cont(s) ? 7u : 5u) +
                                (s.extent.present ? 7u : 0u));
    }
    for (const Leaf& leaf : m_.leaves)
        for (const StateSlot& s : leaf.states) {
            const std::string base = is_cont(s) ? leaf.ident + ".state." + s.name
                                     : is_fin(s) ? "fin." + leaf.ident + "." + s.name
                                                 : "dis." + leaf.ident + "." + s.name;
            // §6.4a — the declared default initialises EVERY element. There
            // is no per-element default syntax: non-uniform initialisation is
            // init()'s job, which is where a computed IC already belonged.
            if (s.extent.present) {
                o_ << "    " << pad(base + ".fill(" + literal(s.initial, s.scalar, s.enm) + ");",
                                    w + 13)
                   << " // " << s.unit.str() << "\n";
                continue;
            }
            o_ << "    " << pad(base, w) << " = se_ic_." << ic_member(leaf, s) << ";\n";
        }
    // §13.4 — element ICs, from the sim file or a loaded source, after the
    // uniform default they refine. Each index was checked at configuration and
    // init() refuses to run while a configuration error stands, so nothing is
    // guarded here.
    bool any_element = false;
    for (const LoadRow& r : load_rows())
        any_element = any_element || r.kind == LoadRow::Kind::Element;
    if (any_element) {
        o_ << "\n    // §13.4 — per-element initial conditions.\n";
        o_ << "    for (const se_rt::Assignment& a : se_ic_elements_)\n";
        o_ << "        element_base(a.row)[a.index] = a.value;\n";
    }
    o_ << "}\n";
}

void Emitter::sim_init() {
    o_ << "\n// Bind every state view to its slot: continuous into x / xd, discrete\n";
    o_ << "// into dis. Done once, from the constructor.\n";
    if (m_.dynamic_states)
        o_ << "// The blocks that are the HOST's (§15.5a) are not here: they have no\n"
              "// address yet. bind_state() and bind_discrete_array() bind those.\n";
    o_ << "void Sim::Impl::bind() {\n";
    for (const Leaf& leaf : m_.leaves) {
        for (const StateSlot& s : leaf.states) {
            if (is_fin(s)) {
                // §6.4c — the committed side, as a discrete state's proxy binds
                // into `dis` and not `nxt`.
                o_ << "    " << leaf.ident << ".state." << s.name << ".bind(fin."
                   << leaf.ident << "." << s.name << ");\n";
                if (has_next(leaf))
                    o_ << "    " << leaf.ident << "_next." << s.name << ".bind(fnx."
                       << leaf.ident << "." << s.name << ");\n";
            } else if (is_cont(s)) {
                if (m_.dynamic_states) continue;
                o_ << "    " << leaf.ident << ".state." << s.name << ".bind(x[" << s.slot
                   << "]);\n";
                o_ << "    " << leaf.ident << "_der." << s.name << ".bind(xd[" << s.slot
                   << "]);\n";
                if (has_next(leaf))
                    o_ << "    " << leaf.ident << "_next." << s.name << ".bind(xnx[" << s.slot
                       << "]);\n";
            } else {
                if (s.extent.present) continue;
                o_ << "    " << leaf.ident << ".state." << s.name << ".bind(dis."
                   << leaf.ident << "." << s.name << ");\n";
                if (has_next(leaf))
                    o_ << "    " << leaf.ident << "_next." << s.name << ".bind(nxt."
                       << leaf.ident << "." << s.name << ");\n";
            }
        }
    }
    o_ << "}\n";

    // §10.3 — final() runs on normal and error termination alike, in reverse
    // init order, but only for nodes whose init() completed.
    o_ << "\nvoid Sim::Impl::final_all(const se_rt::RunContext& ctx) const {\n";
    bool any_final = false;
    for (std::size_t i = m_.leaves.size(); i-- > 0;) {
        const Leaf& leaf = m_.leaves[i];
        if (!leaf.node->method(ast::Method::Which::Final)) continue;
        o_ << "    if (se_inited_ > " << i << ") " << leaf.ident << ".final(ctx);\n";
        any_final = true;
    }
    if (!any_final) o_ << "    (void)ctx;   // no node declares final()\n";
    o_ << "}\n";
}

void Emitter::sim_output_pass() {
    o_ << "\n// Topological order over feedthrough edges (§8.5). A node with an empty\n";
    o_ << "// In_output is a sort root and needs no predecessor.\n";
    o_ << "void Sim::Impl::output_pass() {\n";
    for (const Model::OutputCall& c : m_.calls) {
        if (c.group == Model::OutputCall::none) continue;
        const Leaf& leaf = m_.leaves[c.leaf];
        o_ << "    " << output_call(leaf, leaf.node->out_groups[c.group]) << "\n";
    }
    o_ << "}\n";

    // The minor-step pass. Same topological order, held nodes omitted: their
    // `sig` slice already holds the value they computed at the tick, so
    // skipping them IS the zero-order hold, and a downstream continuous node
    // reads that held value exactly as it did before.
    o_ << "\n// The same order, restricted to the nodes that may move WITHIN a step\n";
    o_ << "// (§10.5). A held node is absent, not stale: `sig` still carries the\n";
    o_ << "// value it published at the tick.\n";
    o_ << "void Sim::Impl::output_pass_minor() {\n";
    bool any = false;
    for (const Model::OutputCall& c : m_.calls) {
        if (c.group == Model::OutputCall::none) continue;
        const Leaf& leaf = m_.leaves[c.leaf];
        if (!minor_step_output(leaf)) continue;
        any = true;
        o_ << "    " << output_call(leaf, leaf.node->out_groups[c.group]) << "\n";
    }
    if (!any) o_ << "    // every node is zero-order held across the step\n";
    o_ << "}\n";
}

void Emitter::sim_derivatives() {
    o_ << "\n// The external solver's entry point: reads x, fills xd. Sim owns both,\n";
    o_ << "// so a solver with its own vector does one memcpy each way per call.\n";
    o_ << "void Sim::Impl::derivatives(double t) {\n";
    o_ << "    se_run_.clock.now = t;   // the minor-step instant, §10.5\n";
    o_ << "    output_pass_minor();\n";
    for (const Leaf& leaf : m_.leaves) {
        const ast::Method* r = leaf.node->method(ast::Method::Which::Derivative);
        if (!r || !leaf.has_continuous()) continue;
        o_ << "    " << leaf.ident << ".derivative(" << in_braces(leaf, r) << ", "
           << leaf.ident << "_der);\n";
    }
    o_ << "}\n";
}

// The jump map, x+ = h(t,x,u), and the node's one per-tick hook (§8.1). Once
// per accepted base step, after outputs have propagated, into buffers seeded
// from the committed state: a state the body does not write keeps its value,
// and every body reads the tick's entry state whatever the order of the calls.
void Emitter::sim_next() {
    if (!any_next()) return;
    bool any_discrete = false, any_finite = false;
    for (const Leaf& leaf : m_.leaves) {
        any_discrete = any_discrete || leaf.has_discrete();
        any_finite = any_finite || leaf.has_finite();
    }
    o_ << "\n// So that a state next() does not write keeps its value.\n";
    o_ << "void Sim::Impl::seed_next() {\n";
    if (any_discrete) o_ << "    nxt = dis;\n";
    if (any_finite) o_ << "    fnx = fin;\n";
    if (jumps_continuous()) {
        if (m_.dynamic_states)
            o_ << "    for (std::size_t i = 0; i < n_states; ++i) se_xnx_[i] = x[i];\n";
        else
            o_ << "    xnx = x;\n";
    }
    o_ << "}\n";

    o_ << "\n// Every next() reads the committed blocks and writes the buffers; tick()\n";
    o_ << "// commits them together, which is what makes the update simultaneous\n";
    o_ << "// across the MODEL and not merely within a node (§6.4).\n";
    o_ << "void Sim::Impl::next_tick() {\n";
    o_ << "    seed_next();\n";
    for (const Leaf& leaf : m_.leaves) {
        const ast::Method* s = leaf.node->method(ast::Method::Which::Next);
        if (!s) continue;
        std::string args = in_braces(leaf, s);
        if (!leaf.states.empty()) args += ", " + leaf.ident + "_next";
        if (!leaf.node->vars.empty()) args += ", " + leaf.ident + ".var";
        o_ << "    " << leaf.ident << ".next(" << args << ");\n";
    }
    o_ << "}\n";
}

void Emitter::sim_integrate() {
    o_ << "\n// The solver is global, not per-node (§9.5): `" << m_.solver << "`.\n";
    if (m_.dynamic_states) {
        sim_integrate_dynamic();
        return;
    }
    if (m_.n_states == 0) {
        o_ << "void Sim::Impl::integrate(double, double) {}   // no continuous states\n";
        return;
    }
    o_ << "void Sim::Impl::integrate(double t, double h) {\n";
    o_ << "    const std::array<double, n_states> x0 = x;\n";
    if (m_.solver == "euler") {
        o_ << "    derivatives(t);\n";
        o_ << "    for (std::size_t i = 0; i < n_states; ++i) x[i] = x0[i] + h * xd[i];\n";
    } else if (m_.solver == "rk2") {
        o_ << "    std::array<double, n_states> k1{};\n";
        o_ << "    derivatives(t);\n    k1 = xd;\n";
        o_ << "    for (std::size_t i = 0; i < n_states; ++i) x[i] = x0[i] + 0.5 * h * k1[i];\n";
        o_ << "    derivatives(t + 0.5 * h);\n";
        o_ << "    for (std::size_t i = 0; i < n_states; ++i) x[i] = x0[i] + h * xd[i];\n";
    } else {
        o_ << "    std::array<double, n_states> k1{}, k2{}, k3{};\n";
        o_ << "    derivatives(t);\n    k1 = xd;\n";
        o_ << "    for (std::size_t i = 0; i < n_states; ++i) x[i] = x0[i] + 0.5 * h * k1[i];\n";
        o_ << "    derivatives(t + 0.5 * h);\n    k2 = xd;\n";
        o_ << "    for (std::size_t i = 0; i < n_states; ++i) x[i] = x0[i] + 0.5 * h * k2[i];\n";
        o_ << "    derivatives(t + 0.5 * h);\n    k3 = xd;\n";
        o_ << "    for (std::size_t i = 0; i < n_states; ++i) x[i] = x0[i] + h * k3[i];\n";
        o_ << "    derivatives(t + h);\n";
        o_ << "    for (std::size_t i = 0; i < n_states; ++i)\n";
        o_ << "        x[i] = x0[i] + (h / 6.0) * (k1[i] + 2.0 * k2[i] + 2.0 * k3[i] + xd[i]);\n";
    }
    o_ << "}\n";
}

// The same solver, over a block whose length is a configuration result. The
// scratch vectors were sized in configure() and are never touched again, so
// nothing here allocates inside a tick (§9.4).
void Emitter::sim_integrate_dynamic() {
    if (m_.n_cont_slots == 0) {
        o_ << "void Sim::Impl::integrate(double, double) {}   // no continuous states\n";
        return;
    }
    o_ << "void Sim::Impl::integrate(double t, double h) {\n";
    o_ << "    const std::size_t n = n_states;\n";
    o_ << "    if (n == 0) { (void)t; (void)h; return; }\n";
    o_ << "    double* const x0 = se_x0_.data();\n";
    o_ << "    for (std::size_t i = 0; i < n; ++i) x0[i] = x[i];\n";
    if (m_.solver == "euler") {
        o_ << "    derivatives(t);\n";
        o_ << "    for (std::size_t i = 0; i < n; ++i) x[i] = x0[i] + h * xd[i];\n";
    } else if (m_.solver == "rk2") {
        o_ << "    double* const k1 = se_k1_.data();\n";
        o_ << "    derivatives(t);\n";
        o_ << "    for (std::size_t i = 0; i < n; ++i) k1[i] = xd[i];\n";
        o_ << "    for (std::size_t i = 0; i < n; ++i) x[i] = x0[i] + 0.5 * h * k1[i];\n";
        o_ << "    derivatives(t + 0.5 * h);\n";
        o_ << "    for (std::size_t i = 0; i < n; ++i) x[i] = x0[i] + h * xd[i];\n";
    } else {
        o_ << "    double* const k1 = se_k1_.data();\n";
        o_ << "    double* const k2 = se_k2_.data();\n";
        o_ << "    double* const k3 = se_k3_.data();\n";
        o_ << "    derivatives(t);\n";
        o_ << "    for (std::size_t i = 0; i < n; ++i) k1[i] = xd[i];\n";
        o_ << "    for (std::size_t i = 0; i < n; ++i) x[i] = x0[i] + 0.5 * h * k1[i];\n";
        o_ << "    derivatives(t + 0.5 * h);\n";
        o_ << "    for (std::size_t i = 0; i < n; ++i) k2[i] = xd[i];\n";
        o_ << "    for (std::size_t i = 0; i < n; ++i) x[i] = x0[i] + 0.5 * h * k2[i];\n";
        o_ << "    derivatives(t + 0.5 * h);\n";
        o_ << "    for (std::size_t i = 0; i < n; ++i) k3[i] = xd[i];\n";
        o_ << "    for (std::size_t i = 0; i < n; ++i) x[i] = x0[i] + h * k3[i];\n";
        o_ << "    derivatives(t + h);\n";
        o_ << "    for (std::size_t i = 0; i < n; ++i)\n";
        o_ << "        x[i] = x0[i] + (h / 6.0) * (k1[i] + 2.0 * k2[i] + 2.0 * k3[i] "
              "+ xd[i]);\n";
    }
    o_ << "}\n";
}

namespace {

const char* level_enum(const std::string& l) {
    return l == "trace"   ? "se_rt::Level::Trace"
           : l == "debug" ? "se_rt::Level::Debug"
           : l == "warn"  ? "se_rt::Level::Warn"
           : l == "error" ? "se_rt::Level::Error"
                          : "se_rt::Level::Info";
}

// The header's bracket for a channel: its unit, its finite type, or
// `buffer(N)` for a buffer's size (§13.5).
std::string channel_label(const RecordedSignal& s) {
    return !s.label.empty() ? s.label : s.enm ? s.enm->fq : s.unit.str();
}

}  // namespace

// §13.5, §14.1 — the channels, and the run's own configuration. Recording is
// quantitative: an aligned numeric table whose header carries units, because
// the values themselves no longer do. What it holds is a run-time choice: the
// .sim's `record:` is only where a run starts.
void Emitter::sim_record() {
    // ── the table ────────────────────────────────────────────────────────────
    o_ << "\n// §13.5 — every channel this model can record, in tree order, with the\n";
    o_ << "// bracket its CSV header carries; then the bare spellings of leaf members.\n";
    o_ << "// An array state is one row, its elements named by index.\n";
    o_ << "se_rt::Tables Sim::Impl::tables() {\n";
    {
        std::vector<std::array<std::string, 3>> cells;
        for (const Channel& c : m_.channels)
            cells.push_back({"{" + quote(c.path) + ",", quote(channel_label(c.sig)) + ",",
                             std::string(c.array ? "true" : "false") + "},"});
        std::array<std::size_t, 3> w{0, 0, 0};
        for (const auto& c : cells)
            for (std::size_t k = 0; k < 3; ++k) w[k] = std::max(w[k], c[k].size());
        o_ << "    static const se_rt::ChannelRow channels[] = {\n";
        o_ << "        // path, header bracket, array\n";
        for (const auto& c : cells)
            o_ << "        " << pad(c[0], w[0] + 1) << pad(c[1], w[1] + 1) << c[2] << "\n";
        o_ << "    };\n";
    }
    if (m_.channel_aliases.empty()) {
        o_ << "    static const se_rt::ChannelAlias* const aliases = nullptr;   // no bare spellings\n";
        o_ << "    const std::size_t n_aliases = 0;\n";
    } else {
        std::size_t w = 0;
        for (const ChannelAlias& a : m_.channel_aliases)
            w = std::max(w, quote(a.spelling).size() + 2);
        o_ << "    static const se_rt::ChannelAlias aliases[] = {\n";
        o_ << "        // spelling, row, the output it might also mean (ambiguous: SE0461)\n";
        for (const ChannelAlias& a : m_.channel_aliases)
            o_ << "        {" << pad(quote(a.spelling) + ",", w) << " " << a.row << ", "
               << (a.other.empty() ? std::string("nullptr") : quote(a.other)) << "},\n";
        o_ << "    };\n";
        o_ << "    const std::size_t n_aliases = sizeof aliases / sizeof aliases[0];\n";
    }
    o_ << "    std::size_t n = 0;\n";
    o_ << "    const se_rt::SettingRow* rows = setting_rows(n);\n";
    o_ << "    return {rows, n, channels, sizeof channels / sizeof channels[0], aliases, n_aliases,\n";
    o_ << "            step_seconds};\n";
    o_ << "}\n";

    // ── one channel's value ──────────────────────────────────────────────────
    bool any_array = false;
    for (const Channel& c : m_.channels) any_array = any_array || c.array;
    o_ << "\n// One channel's value now, as a double; `i` is an array state's element.\n";
    o_ << "double Sim::Impl::channel(std::size_t row, std::size_t i) {\n";
    if (!any_array) o_ << "    (void)i;\n";
    o_ << "    switch (row) {\n";
    for (std::size_t r = 0; r < m_.channels.size(); ++r) {
        // §13.5 — a port path that ended on a setting could not be rendered at
        // elaboration, because a setting reference needs an identifier this
        // stage assigns. Everything else arrives already written.
        const RecordedSignal& s = m_.channels[r].sig;
        o_ << "        case " << r << ": return static_cast<double>("
           << (s.code.empty() ? s.expr : render_expr(s.code)) << ");   // "
           << m_.channels[r].path << "\n";
    }
    o_ << "        default: return 0.0;\n";
    o_ << "    }\n}\n";

    o_ << "\n// An array channel's element count, at the extent configuration settles.\n";
    o_ << "std::size_t Sim::Impl::channel_extent(std::size_t row) {\n";
    if (!any_array) {
        o_ << "    (void)row;   // no array states\n";
        o_ << "    return 1;\n}\n";
    } else {
        o_ << "    std::size_t n = 0;\n";
        o_ << "    switch (row) {\n";
        for (std::size_t r = 0; r < m_.channels.size(); ++r) {
            const Channel& c = m_.channels[r];
            if (!c.array) continue;
            const StateSlot& st = m_.leaves[c.leaf].states[c.state];
            o_ << "        case " << r << ": se_rt::extent_of(static_cast<double>("
               << render_expr(st.extent.expr) << "), n); return n;   // " << c.path << "\n";
        }
        o_ << "        default: return 1;\n";
        o_ << "    }\n}\n";
    }

    // ── where a run starts ───────────────────────────────────────────────────
    o_ << "\n// §13.3 — the .sim's `record:` and `log:`, as rows of the table above.\n";
    o_ << "se_rt::RunConfig Sim::Impl::default_config() {\n";
    o_ << "    se_rt::RunConfig c;\n";
    o_ << "    c.record_file = " << quote(m_.record.file) << ";\n";
    o_ << "    c.every       = " << m_.record.every << ";\n";
    if (m_.record.columns.empty()) {
        o_ << "    // no channels: nothing is recorded unless a source selects some\n";
    } else {
        o_ << "    c.columns = {\n";
        for (const RecordConfig::Column& k : m_.record.columns)
            o_ << "        {" << k.row << ", " << k.index << ", " << quote(k.written) << "},\n";
        o_ << "    };\n";
    }
    o_ << "    c.log_file    = " << quote(m_.log.file) << ";\n";
    o_ << "    c.level       = " << level_enum(m_.log.level) << ";\n";
    for (const auto& kv : m_.log.levels)
        o_ << "    c.levels.emplace_back(" << quote(kv.first) << ", " << level_enum(kv.second)
           << ");\n";
    o_ << "    return c;\n";
    o_ << "}\n";

    // ── a selected element past its extent ───────────────────────────────────
    o_ << "\n// §13.5 — a selected element past its array's extent, which only\n";
    o_ << "// configuration settles: checked when a source loads and when the run\n";
    o_ << "// starts, as an override's index is (§13.4).\n";
    o_ << "void Sim::Impl::check_columns() {\n";
    if (!any_array) {
        o_ << "    // no array states, so no index to check\n";
    } else {
        o_ << "    for (const se_rt::Column& c : se_config_.columns)\n";
        o_ << "        if (tables().channels[c.row].array && c.index >= channel_extent(c.row) &&\n";
        o_ << "            se_cfg_error_.empty())\n";
        o_ << "            se_cfg_error_ = se_rt::index_error(c.written.c_str(), channel_extent(c.row));\n";
    }
    o_ << "}\n";

    // ── the table on disk ────────────────────────────────────────────────────
    o_ << "\n// §13.5 — the header, once configuration is over. No channel, no table.\n";
    o_ << "void Sim::Impl::record_open() {\n";
    o_ << "    if (se_config_.columns.empty()) return;\n";
    o_ << "    if (!se_rec_.open(se_config_.record_file, se_stamp_)) {\n";
    o_ << "        se_run_.log.write(se_rt::Level::Error, \"sim\", 0.0,\n";
    o_ << "                          \"cannot open \" + se_rec_.used() + \" for writing\");\n";
    o_ << "        return;\n";
    o_ << "    }\n";
    o_ << "    const se_rt::Tables t = tables();\n";
    o_ << "    std::vector<std::string> head;\n";
    o_ << "    for (const se_rt::Column& c : se_config_.columns)\n";
    o_ << "        head.push_back(c.written + \" [\" + t.channels[c.row].label + \"]\");\n";
    o_ << "    se_rec_.header(head);\n";
    o_ << "    se_row_.assign(se_config_.columns.size(), 0.0);\n";
    o_ << "}\n";

    o_ << "\nvoid Sim::Impl::record_row() {\n";
    o_ << "    for (std::size_t k = 0; k < se_row_.size(); ++k)\n";
    o_ << "        se_row_[k] = channel(se_config_.columns[k].row, se_config_.columns[k].index);\n";
    o_ << "    se_rec_.row(se_time_, se_row_);\n";
    o_ << "}\n";

    // ── the menu, and the list ───────────────────────────────────────────────
    o_ << "\n// §14.1 — every channel as a `record` section, the unselected ones\n";
    o_ << "// commented out: edit it, and load it back.\n";
    o_ << "const char* Sim::Impl::list_signals() {\n";
    o_ << "    const se_rt::Tables t = tables();\n";
    o_ << "    std::vector<std::size_t> extents(t.n_channels, 1);\n";
    o_ << "    for (std::size_t r = 0; r < t.n_channels; ++r)\n";
    o_ << "        if (t.channels[r].array) extents[r] = channel_extent(r);\n";
    o_ << "    se_list_ = se_rt::list_source(" << quote(m_.name)
       << ", t.channels, t.n_channels, extents.data(), se_config_);\n";
    o_ << "    return se_list_.c_str();\n";
    o_ << "}\n";

    o_ << "\nstd::vector<Sim::ChannelInfo> Sim::Impl::channels_list() {\n";
    o_ << "    const se_rt::Tables t = tables();\n";
    o_ << "    std::vector<Sim::ChannelInfo> out;\n";
    o_ << "    for (std::size_t r = 0; r < t.n_channels; ++r) {\n";
    o_ << "        const std::size_t count = t.channels[r].array ? channel_extent(r) : 1;\n";
    o_ << "        for (std::size_t i = 0; i < count; ++i) {\n";
    o_ << "            Sim::ChannelInfo c;\n";
    o_ << "            c.path = t.channels[r].path;\n";
    o_ << "            if (t.channels[r].array) c.path += \"[\" + std::to_string(i) + \"]\";\n";
    o_ << "            c.unit = t.channels[r].label;\n";
    o_ << "            for (const se_rt::Column& k : se_config_.columns)\n";
    o_ << "                c.selected = c.selected || (k.row == r && k.index == i);\n";
    o_ << "            out.push_back(std::move(c));\n";
    o_ << "        }\n";
    o_ << "    }\n";
    o_ << "    return out;\n";
    o_ << "}\n";

    // ── one call per line of a section ───────────────────────────────────────
    o_ << "\n// §14.1 — the run's configuration one call at a time, each exactly one\n";
    o_ << "// line of a `record` or `log` section. A refused call changes nothing and\n";
    o_ << "// does not stop the run; error() says why until the next call.\n";
    o_ << "bool Sim::Impl::configuring(const char* call) {\n";
    o_ << "    if (se_started_) {\n";
    o_ << "        se_call_error_ = std::string(call) + \"() comes before init(): \"\n";
    o_ << "                         \"configuration is over (§6.2b)\";\n";
    o_ << "        return false;\n";
    o_ << "    }\n";
    o_ << "    se_call_error_.clear();\n";
    o_ << "    return true;\n";
    o_ << "}\n";

    o_ << "\nbool Sim::Impl::record(const char* path, bool add) {\n";
    o_ << "    if (!configuring(add ? \"record\" : \"unrecord\")) return false;\n";
    o_ << "    se_rt::Column c{0, 0, std::string()};\n";
    o_ << "    std::string why;\n";
    o_ << "    if (!se_rt::channel_text(tables(), path, c, why)) {\n";
    o_ << "        se_call_error_ = std::string(path ? path : \"\") + \": \" + why;\n";
    o_ << "        return false;\n";
    o_ << "    }\n";
    o_ << "    if (add) se_config_.add(c);\n";
    o_ << "    else se_config_.remove(c.row, c.index);\n";
    o_ << "    return true;\n";
    o_ << "}\n";

    o_ << "\nvoid Sim::Impl::record_clear() {\n";
    o_ << "    if (configuring(\"record_clear\")) se_config_.columns.clear();\n";
    o_ << "}\n";

    o_ << "\nbool Sim::Impl::record_file(const char* file) {\n";
    o_ << "    if (!configuring(\"record_file\")) return false;\n";
    o_ << "    if (!file || !*file) {\n";
    o_ << "        se_call_error_ = \"record_file: a file name is required; record_clear() \"\n";
    o_ << "                         \"turns recording off\";\n";
    o_ << "        return false;\n";
    o_ << "    }\n";
    o_ << "    const std::string bad = se_rt::file_pattern_error(file);\n";
    o_ << "    if (!bad.empty()) {\n";
    o_ << "        se_call_error_ = \"record_file: \" + bad;\n";
    o_ << "        return false;\n";
    o_ << "    }\n";
    o_ << "    se_config_.record_file = file;\n";
    o_ << "    return true;\n";
    o_ << "}\n";

    o_ << "\nbool Sim::Impl::record_every(std::uint64_t n) {\n";
    o_ << "    if (!configuring(\"record_every\")) return false;\n";
    o_ << "    if (n < 1) {\n";
    o_ << "        se_call_error_ = \"record_every: a whole number of base steps, 1 or more\";\n";
    o_ << "        return false;\n";
    o_ << "    }\n";
    o_ << "    se_config_.every = n;\n";
    o_ << "    return true;\n";
    o_ << "}\n";

    o_ << "\nbool Sim::Impl::log_file(const char* file) {\n";
    o_ << "    if (!configuring(\"log_file\")) return false;\n";
    o_ << "    const std::string bad = se_rt::file_pattern_error(file ? file : \"\");\n";
    o_ << "    if (!bad.empty()) {\n";
    o_ << "        se_call_error_ = \"log_file: \" + bad;\n";
    o_ << "        return false;\n";
    o_ << "    }\n";
    o_ << "    se_config_.log_file = file ? file : \"\";\n";
    o_ << "    return true;\n";
    o_ << "}\n";

    o_ << "\n// §13.5 — a file's name as configured, `{time}` and all, until the run\n";
    o_ << "// opens it; then the name it opened.\n";
    o_ << "const char* Sim::Impl::record_file_name() const {\n";
    o_ << "    return se_rec_.used().empty() ? se_config_.record_file.c_str() : se_rec_.used().c_str();\n";
    o_ << "}\n";
    o_ << "\nconst char* Sim::Impl::log_file_name() const {\n";
    o_ << "    return se_started_ ? se_run_.log.used().c_str() : se_config_.log_file.c_str();\n";
    o_ << "}\n";

    o_ << "\n// §14.1 — a channel read live: resolved once, as record() resolves it,\n";
    o_ << "// then read as the last tick sampled it. Not configuration: either side\n";
    o_ << "// of init() will do. By the time tick() returns, the states have moved\n";
    o_ << "// and the solver has overwritten the signals, so tick() keeps each\n";
    o_ << "// handle's value where it records a row; one taken mid-run is NaN\n";
    o_ << "// until the next tick samples it.\n";
    o_ << "int Sim::Impl::channel_handle(const char* path) {\n";
    o_ << "    se_call_error_.clear();\n";
    o_ << "    se_rt::Column c{0, 0, std::string()};\n";
    o_ << "    std::string why;\n";
    o_ << "    if (!se_rt::channel_text(tables(), path, c, why)) {\n";
    o_ << "        se_call_error_ = std::string(path ? path : \"\") + \": \" + why;\n";
    o_ << "        return -1;\n";
    o_ << "    }\n";
    o_ << "    se_handles_.push_back(c);\n";
    o_ << "    se_handle_vals_.push_back(std::nan(\"\"));\n";
    o_ << "    return static_cast<int>(se_handles_.size() - 1);\n";
    o_ << "}\n";
    o_ << "\ndouble Sim::Impl::read(int handle) {\n";
    o_ << "    if (handle < 0 || static_cast<std::size_t>(handle) >= se_handles_.size())\n";
    o_ << "        return std::nan(\"\");\n";
    o_ << "    const std::size_t k = static_cast<std::size_t>(handle);\n";
    o_ << "    // Before the first tick nothing has moved, so the storage is the answer.\n";
    o_ << "    return se_ticked_ ? se_handle_vals_[k] : handle_value(se_handles_[k]);\n";
    o_ << "}\n";
    o_ << "\ndouble Sim::Impl::handle_value(const se_rt::Column& c) {\n";
    if (any_array) {
        o_ << "    if (tables().channels[c.row].array && c.index >= channel_extent(c.row))\n";
        o_ << "        return std::nan(\"\");   // past the extent configuration settled\n";
    }
    o_ << "    return channel(c.row, c.index);\n";
    o_ << "}\n";
    o_ << "\nvoid Sim::Impl::sample_handles() {\n";
    o_ << "    for (std::size_t k = 0; k < se_handles_.size(); ++k)\n";
    o_ << "        se_handle_vals_[k] = handle_value(se_handles_[k]);\n";
    o_ << "    se_ticked_ = true;\n";
    o_ << "}\n";

    o_ << "\n// `path` null: the global threshold; otherwise one node path's.\n";
    o_ << "bool Sim::Impl::log_level(const char* path, const char* level) {\n";
    o_ << "    if (!configuring(\"log_level\")) return false;\n";
    o_ << "    se_rt::Level l = se_rt::Level::Info;\n";
    o_ << "    if (!level || !se_rt::level_of(level, l)) {\n";
    o_ << "        se_call_error_ = std::string(\"log_level: `\") + (level ? level : \"\") +\n";
    o_ << "                         \"` is not a level: trace, debug, info, warn or error\";\n";
    o_ << "        return false;\n";
    o_ << "    }\n";
    o_ << "    if (path) se_config_.path_level(path, l);\n";
    o_ << "    else se_config_.level = l;\n";
    o_ << "    return true;\n";
    o_ << "}\n";
}

void Emitter::sim_run() {
    bool any_discrete = false;
    for (const Leaf& leaf : m_.leaves) any_discrete = any_discrete || leaf.has_discrete();
    bool any_finite = false;
    for (const Leaf& leaf : m_.leaves) any_finite = any_finite || leaf.has_finite();

    // ── init ─────────────────────────────────────────────────────────────────
    o_ << "\nvoid Sim::Impl::init() {\n";
    o_ << "    // §6.2b — configuration precedes init(), so a value it refused stops\n";
    o_ << "    // the run here, before any node sees state it was never compiled for.\n";
    o_ << "    if (!se_cfg_error_.empty()) { se_run_.control.fail(se_cfg_error_); return; }\n";
    if (m_.dynamic_states) {
        // §15.5a — the buffers are the host's, so their absence is the one
        // failure the handshake makes possible after a clean configuration.
        o_ << "    if (n_states && !se_bound_x_) {\n";
        o_ << "        se_run_.control.fail(" << quote("bind_state() was not called; "
              "this model's state buffers are the host's (§15.5a)") << ");\n";
        o_ << "        return;\n    }\n";
        if (m_.n_disc_arr_slots) {
            o_ << "    if (n_dstates && !se_bound_da_) {\n";
            o_ << "        se_run_.control.fail("
               << quote("bind_discrete_array() was not called (§15.5a)") << ");\n";
            o_ << "        return;\n    }\n";
        }
    }
    o_ << "    // §13.4 — every IC override is in place before the first node init(),\n";
    o_ << "    // so each init() starts from the IC this run was configured with.\n";
    o_ << "    apply_initial_conditions();\n";
    o_ << "    se_tick_ = 0;\n    se_time_ = 0.0;\n    se_done_ = false;\n";
    o_ << "    se_run_.clock = se_rt::RunState{0.0, step_seconds, 0};   // §10.5\n";
    o_ << "    se_inited_ = 0;\n";
    o_ << "    record_open();\n";
    for (std::size_t i = 0; i < m_.leaves.size(); ++i) {
        const Leaf& leaf = m_.leaves[i];
        o_ << "    if (se_run_.control.halted()) return;\n";
        if (leaf.node->method(ast::Method::Which::Init))
            o_ << "    " << leaf.ident << ".init();\n";
        else
            o_ << "    // " << (leaf.path.empty() ? "<root>" : leaf.path)
               << " has no init(): its ICs are declarative.\n";
        o_ << "    se_inited_ = " << (i + 1) << ";\n";
    }
    o_ << "}\n";

    // ── tick ─────────────────────────────────────────────────────────────────
    o_ << "\n// One base tick, normatively (§9.6). A host drives the root boundary\n";
    o_ << "// through `sig.in` immediately before calling this.\n";
    o_ << "//\n";
    o_ << "// Every method here observes the tick's ENTRY state, and so does the\n";
    o_ << "// recorder: one row is one consistent sample of the model at t[k]. Every\n";
    o_ << "// state moves below the exit, so a run that stops is left holding a valid\n";
    o_ << "// checkpoint (§15.5).\n";
    o_ << "void Sim::Impl::tick() {\n";
    o_ << "    if (se_tick_ == 0) se_run_.realtime.anchor();   // §9.4: pacing counts from here\n";
    o_ << "    se_time_ = static_cast<double>(se_tick_) * step_seconds;\n";
    o_ << "    se_run_.log_time = se_time_;    // what a log record is stamped with\n";
    o_ << "    se_run_.clock.now = se_time_;   // §10.5 — the tick instant; derivatives()\n";
    o_ << "    se_run_.clock.tick = se_tick_;  // moves it again at every minor step\n";
    o_ << "    output_pass();\n";
    o_ << "    publish_outputs();\n";
    if (any_next()) o_ << "    if (!se_run_.control.halted()) next_tick();\n";
    o_ << "    if (se_rec_.active() && se_tick_ % se_config_.every == 0) record_row();\n";
    o_ << "    sample_handles();   // §14.1 — what read() returns until the next tick\n";
    o_ << "    if (se_run_.control.halted() || se_tick_ >= se_end_tick_) {\n";
    o_ << "        se_done_ = true;\n        return;\n    }\n";
    // §9.6 — the commit: below the exit, so a halted run keeps the state its
    // last row reported; above the flow, so the solver starts from the jump.
    if (any_discrete) o_ << "    dis = nxt;   // simultaneous update, whole model (§6.4)\n";
    if (any_finite) o_ << "    fin = fnx;\n";
    if (jumps_continuous()) {
        if (m_.dynamic_states)
            o_ << "    for (std::size_t i = 0; i < n_states; ++i) x[i] = se_xnx_[i];\n";
        else
            o_ << "    x = xnx;\n";
    }
    o_ << "    integrate(se_time_, step_seconds);\n";
    o_ << "    ++se_tick_;\n";
    o_ << "}\n";

    // ── finish ───────────────────────────────────────────────────────────────
    o_ << "\nvoid Sim::Impl::finish() {\n";
    o_ << "    se_run_.control.complete();\n";
    o_ << "    const se_rt::RunContext ctx = se_run_.control.context(se_time_);\n";
    o_ << "    final_all(ctx);\n";
    o_ << "    se_rec_.close();\n";
    o_ << "    se_run_.realtime.summary(se_run_.log, se_time_);   // silent unless the host paced the run\n";
    // §10.6 -- after the realtime summary and before the closing line, so
    // the last thing a log carries is the run's outcome and not a contract detail.
    o_ << "    se_run_.checks.summary(se_run_.log, se_time_);\n";
    o_ << "    se_run_.log.write(se_rt::Level::Info, \"sim\", se_time_,\n";
    o_ << "                      se_rt::format(\"run {}: {}\", ctx.reason,\n";
    o_ << "                                    ctx.message.empty() ? \"-\" "
          ": ctx.message.c_str()));\n";
    o_ << "}\n";

    // ── the exit code, and the reason ────────────────────────────────────────
    // §10.6 -- a run that violated a contract is not a successful run, even
    // when nothing halted it. That is what lets a contract BE a test oracle
    // instead of something a harness has to go grepping the log for.
    o_ << "\n// 0 for a clean run; 1 if it was aborted, failed, or broke a contract (§10.6).\n";
    o_ << "int Sim::Impl::exit_code() const {\n";
    o_ << "    const se_rt::Reason r = se_run_.control.reason();\n";
    o_ << "    const bool failed = r == se_rt::Reason::Aborted || r == se_rt::Reason::Error;\n";
    o_ << "    return failed || se_run_.checks.any() ? 1 : 0;\n";
    o_ << "}\n";

    o_ << "\nconst char* Sim::Impl::error() const {\n";
    o_ << "    if (!se_rerun_error_.empty()) return se_rerun_error_.c_str();\n";
    o_ << "    const std::string& m = se_run_.control.message();\n";
    o_ << "    if (!m.empty()) return m.c_str();\n";
    o_ << "    if (!se_load_error_.empty()) return se_load_error_.c_str();\n";
    o_ << "    return se_cfg_error_.empty() ? se_call_error_.c_str() : se_cfg_error_.c_str();\n";
    o_ << "}\n";

    // ── start ────────────────────────────────────────────────────────────────
    o_ << "\n// Sim::init(): the run configuration from the .sim, the host's settings,\n";
    o_ << "// everything derived from them (§6.2b), then every node's init().\n";
    o_ << "bool Sim::Impl::start() {\n";
    o_ << "    // A Sim runs once. A second run on the same object would start from\n";
    o_ << "    // whatever the first left in every node's private storage, which no\n";
    o_ << "    // init() promises to reset; a fresh Sim is the clean slate. The first\n";
    o_ << "    // run's outcome stands, so this does not touch se_run_.\n";
    o_ << "    if (se_started_) {\n";
    o_ << "        se_rerun_error_ = \"init() was already called: a Sim runs once; \"\n";
    o_ << "                          \"construct a new one to run again\";\n";
    o_ << "        return false;\n";
    o_ << "    }\n";
    o_ << "    se_started_ = true;\n";
    o_ << "    se_stamp_ = se_rt::time_stamp();   // one `{time}` for the run's files (§13.5)\n";
    o_ << "    configure_run();\n";
    o_ << "    const std::string refused = accept_settings();\n";
    o_ << (m_.dynamic_states ? "    configure();\n" : "    resolve_settings();\n");
    o_ << "    if (!refused.empty() && se_cfg_error_.empty()) se_cfg_error_ = refused;\n";
    o_ << "    // §14.1 — a refused settings source stands, and came first.\n";
    o_ << "    if (!se_load_error_.empty()) se_cfg_error_ = se_load_error_;\n";
    o_ << "    check_columns();\n";
    o_ << "    publish_settings();\n";
    if (m_.dynamic_states) {
        // §15.5a — the buffers are the host's, and here the host is Sim.
        o_ << "    se_x_buf_.assign(state_size(), 0.0);\n";
        o_ << "    se_xd_buf_.assign(state_size(), 0.0);\n";
        o_ << "    bind_state(se_x_buf_.data(), se_xd_buf_.data(), se_x_buf_.size());\n";
        if (m_.n_disc_arr_slots) {
            o_ << "    se_d_buf_.assign(discrete_array_size(), 0.0);\n";
            o_ << "    se_dn_buf_.assign(discrete_array_size(), 0.0);\n";
            o_ << "    bind_discrete_array(se_d_buf_.data(), se_dn_buf_.data(), "
                  "se_d_buf_.size());\n";
        }
    }
    o_ << "    init();\n";
    o_ << "    return !se_run_.control.halted();\n";
    o_ << "}\n";

    // ── the log, as configuration left it ────────────────────────────────────
    o_ << "\n// Logging (§13.7): the .sim's `log:`, as every source loaded has edited it.\n";
    o_ << "// The file opens here, when the run starts, so a host that gives two copies\n";
    o_ << "// of one model a file each gets two logs.\n";
    o_ << "void Sim::Impl::configure_run() {\n";
    o_ << "    se_run_.log.configure(se_config_.log_file, se_config_.level, se_stamp_);\n";
    o_ << "    for (const auto& p : se_config_.levels)\n";
    o_ << "        se_run_.log.set_path_level(p.first.c_str(), p.second);\n";
    o_ << "}\n";
}

// The seam between the public structs and the model's own storage. The root's
// settings live where configuration writes them (a composite's `se_cfg_root`, or
// a leaf root's `param`), and the host edits a copy of them in `Sim::Settings`;
// the root outputs live in the producers' slices of `sig`, and the host reads a
// copy of them in `Sim::Outputs`.
void Emitter::sim_public_glue() {
    std::vector<std::size_t> root;
    for (std::size_t i = 0; i < m_.settings.size(); ++i)
        if (m_.settings[i].owner.empty()) root.push_back(i);

    // ── accept: the host's edits, pinned ─────────────────────────────────────
    o_ << "\n// A root setting the host changed through Sim::settings() is pinned, so\n";
    o_ << "// resolve_settings() leaves it alone and everything derived from it\n";
    o_ << "// follows. Returns why a change was refused, or nothing.\n";
    o_ << "std::string Sim::Impl::accept_settings() {\n";
    o_ << "    std::string refused;\n";
    for (std::size_t i : root) {
        const SettingSlot& s = m_.settings[i];
        const std::string a = "se_settings_." + s.name, b = "se_resolved_." + s.name;
        if (s.structural) {
            o_ << "    if (" << a << " != " << b << " && refused.empty())\n";
            o_ << "        refused = " << quote(s.path + " is structural: it was fixed when "
                                                        "the model was generated (§6.2)")
               << ";\n";
            continue;
        }
        o_ << "    if (" << a << " != " << b << ") {\n";
        o_ << "        " << param_base(s.owner) << "." << s.name << " = " << a << ";\n";
        o_ << "        se_pin_[" << i << "] = true;\n";
        o_ << "    }\n";
    }
    o_ << "    return refused;\n";
    o_ << "}\n";

    // ── publish: what configuration settled on ───────────────────────────────
    o_ << "\n// Hands the configured root settings back, so Sim::settings() shows what the\n";
    o_ << "// model runs with, derived values included.\n";
    o_ << "void Sim::Impl::publish_settings() {\n";
    std::size_t w = 0;
    for (std::size_t i : root) w = std::max(w, m_.settings[i].name.size());
    for (std::size_t i : root) {
        const SettingSlot& s = m_.settings[i];
        o_ << "    se_settings_." << pad(s.name, w) << " = " << param_base(s.owner) << "."
           << s.name << ";\n";
    }
    o_ << "    se_resolved_ = se_settings_;\n";
    o_ << "}\n";

    // ── the root outputs ─────────────────────────────────────────────────────
    o_ << "\n// The root outputs, read through the same wires a consumer would read them\n";
    o_ << "// through (§15.3), unit conversions included.\n";
    o_ << "void Sim::Impl::publish_outputs() {\n";
    w = 0;
    for (const RootOutput& r : m_.root_outputs) w = std::max(w, r.name.size());
    for (const RootOutput& r : m_.root_outputs) {
        if (!r.driven) {
            o_ << "    // " << r.name << ": nothing drives it, so it stays zero\n";
            continue;
        }
        o_ << "    se_outputs_." << pad(r.name, w) << " = " << source_expr(r.src, &r.type)
           << ";\n";
    }
    o_ << "}\n";
}

void Emitter::sim_class() {
    const std::string ns = cpp_ns();
    o_ << "// -- Sim::Impl: the whole model, flat " << std::string(41, '-') << "\n";
    if (!ns.empty()) o_ << "namespace " << ns << " {\n\n";
    // The class body is the data layout and the list of what it does; every
    // body follows it, so `struct Sim::Impl { … };` can be read in one screen.
    o_ << "struct Sim::Impl {\n";
    build_param_bases();
    sim_members();
    o_ << "};\n";
    sim_resolve_settings();
    sim_configure();
    sim_elaborate();
    sim_init();
    sim_binders();
    sim_output_pass();
    sim_derivatives();
    sim_next();
    sim_integrate();
    sim_record();
    sim_run();
    sim_public_glue();
    sim_settings_source();
    sim_maps();
    sim_facade();
    if (!ns.empty()) o_ << "\n}  // namespace " << ns << "\n";
}

// ─── Settings sources (§14.1) ────────────────────────────────────────────────

std::string Emitter::ic_member(const Leaf& leaf, const StateSlot& st) {
    return (leaf.ident.empty() ? std::string() : leaf.ident + "_") + st.name;
}

std::vector<Emitter::LoadRow> Emitter::load_rows() const {
    std::vector<LoadRow> rows;
    for (std::size_t i = 0; i < m_.settings.size(); ++i) {
        LoadRow r;
        r.kind = LoadRow::Kind::Setting;
        r.path = m_.settings[i].path;
        r.setting = i;
        rows.push_back(r);
    }
    for (const bool element : {false, true})
        for (const Leaf& leaf : m_.leaves)
            for (const StateSlot& st : leaf.states) {
                if (st.extent.present != element) continue;
                LoadRow r;
                r.kind = element ? LoadRow::Kind::Element : LoadRow::Kind::Initial;
                r.path = leaf.path.empty() ? st.name : leaf.path + "." + st.name;
                r.leaf = &leaf;
                r.state = &st;
                rows.push_back(r);
            }
    // §13.3 — last, so a setting's row stays its pin.
    LoadRow run;
    run.kind = LoadRow::Kind::Run;
    run.path = "sim.duration";
    rows.push_back(run);
    return rows;
}

// The model's half of §14.1. The runtime reads a source and checks every
// binding against setting_rows(); what is generated here is the table, and the
// two switches that move one row's value in and out of the member holding it.
void Emitter::sim_settings_source() {
    const std::vector<LoadRow> rows = load_rows();
    bool any_element = false;
    for (const LoadRow& r : rows) any_element = any_element || r.kind == LoadRow::Kind::Element;

    static const Unit seconds = [] {
        Unit u;
        lookup_unit_symbol("s", u);
        return u;
    }();
    auto member = [&](const LoadRow& r) -> std::string {
        if (r.kind == LoadRow::Kind::Run) return "se_duration_";
        if (r.kind == LoadRow::Kind::Setting) {
            const SettingSlot& s = m_.settings[r.setting];
            return param_base(s.owner) + "." + s.name;
        }
        return "se_ic_." + ic_member(*r.leaf, *r.state);
    };
    auto scalar_of = [&](const LoadRow& r) -> std::string {
        if (r.kind == LoadRow::Kind::Run) return std::string("double");
        return r.kind == LoadRow::Kind::Setting ? m_.settings[r.setting].scalar : r.state->scalar;
    };
    auto enum_of = [&](const LoadRow& r) -> const EnumInfo* {
        if (r.kind == LoadRow::Kind::Run) return nullptr;
        return r.kind == LoadRow::Kind::Setting ? m_.settings[r.setting].enm : r.state->enm;
    };
    auto unit_of = [&](const LoadRow& r) -> const Unit& {
        if (r.kind == LoadRow::Kind::Run) return seconds;
        return r.kind == LoadRow::Kind::Setting ? m_.settings[r.setting].unit : r.state->unit;
    };

    // ── the table ────────────────────────────────────────────────────────────
    o_ << "\n// §14.1 — every path a settings source may bind, with what the loader\n";
    o_ << "// checks it against: its type, and the dimension and scale behind its unit\n";
    o_ << "// string (§15.6). Settings come first, in configuration order, so a\n";
    o_ << "// setting's row is also its pin.\n";
    o_ << "const se_rt::SettingRow* Sim::Impl::setting_rows(std::size_t& count) {\n";
    if (rows.empty()) {
        o_ << "    count = 0;   // nothing here a source could bind\n";
        o_ << "    return nullptr;\n}\n";
    } else {
        std::vector<const EnumInfo*> enums;
        for (const LoadRow& r : rows)
            if (const EnumInfo* e = enum_of(r))
                if (std::find(enums.begin(), enums.end(), e) == enums.end()) enums.push_back(e);
        // A private finite type's name carries its file's (`fsm.Finite#Ctl`),
        // so anything that is not an identifier character becomes `_`.
        auto table = [&](const EnumInfo* e) {
            std::string id = "kEnum_";
            for (char c : e->fq)
                id += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
            return id;
        };
        for (const EnumInfo* e : enums) {
            o_ << "    static const se_rt::Enumerator " << table(e) << "[] = {";
            for (std::size_t k = 0; k < e->values.size(); ++k)
                o_ << (k ? ", " : "") << "{" << quote(e->values[k].name) << ", "
                   << e->values[k].value << "}";
            o_ << "};\n";
        }
        // One row's fields as text first, so the columns can be aligned: the
        // table is read by people as well as by the loader.
        std::vector<std::vector<std::string>> cells;
        for (const LoadRow& r : rows) {
            const Unit& u = unit_of(r);
            const EnumInfo* e = enum_of(r);
            std::vector<std::string> c;
            c.push_back("{" + quote(r.path) + ",");
            c.push_back(r.kind == LoadRow::Kind::Setting   ? "K::Setting,"
                        : r.kind == LoadRow::Kind::Initial ? "K::Initial,"
                        : r.kind == LoadRow::Kind::Run     ? "K::Run,"
                                                           : "K::Element,");
            c.push_back(quote(e ? e->fq : scalar_of(r)) + ",");
            c.push_back(quote(u.str()) + ",");
            std::string dim = "{";
            for (int k = 0; k < kBaseDims; ++k) {
                const Rat q = u.dim[k];
                dim += (k ? ", " : "") +
                       (q.d == 1 ? std::to_string(q.n)
                                 : dbl(static_cast<double>(q.n) / static_cast<double>(q.d)));
            }
            c.push_back(dim + "},");
            c.push_back(dbl(u.scale) + ",");
            c.push_back(dbl(u.offset) + ",");
            c.push_back(e ? table(e) + ", " + std::to_string(e->values.size()) + ","
                          : std::string("nullptr, 0,"));
            const bool structural =
                r.kind == LoadRow::Kind::Setting && m_.settings[r.setting].structural;
            c.push_back(std::string(structural ? "true" : "false") + "},");
            cells.push_back(c);
        }
        std::vector<std::size_t> w(cells.front().size(), 0);
        for (const auto& c : cells)
            for (std::size_t k = 0; k < c.size(); ++k) w[k] = std::max(w[k], c[k].size());
        o_ << "    using K = se_rt::RowKind;\n";
        o_ << "    static const se_rt::SettingRow rows[] = {\n";
        o_ << "        // path, kind, type, unit, {m kg s A K mol cd rad}, scale, offset,\n";
        o_ << "        // enumerators, structural\n";
        for (const auto& c : cells) {
            o_ << "        ";
            for (std::size_t k = 0; k < c.size(); ++k)
                o_ << (k + 1 < c.size() ? pad(c[k], w[k] + 1) : c[k]);
            o_ << "\n";
        }
        o_ << "    };\n";
        o_ << "    count = sizeof rows / sizeof rows[0];\n";
        o_ << "    return rows;\n}\n";
    }

    // ── one row's value, in ──────────────────────────────────────────────────
    o_ << "\n// One checked binding into the member it names. A setting is pinned, so\n";
    o_ << "// resolve_settings() leaves it where the source put it (§13.3).\n";
    o_ << "void Sim::Impl::assign(const se_rt::Assignment& a) {\n";
    if (any_element) {
        o_ << "    std::size_t count = 0;\n";
        o_ << "    if (setting_rows(count)[a.row].kind == se_rt::RowKind::Element) {\n";
        o_ << "        for (se_rt::Assignment& e : se_ic_elements_)\n";
        o_ << "            if (e.row == a.row && e.index == a.index) {\n";
        o_ << "                e.value = a.value;   // a later writer (§13.3)\n";
        o_ << "                return;\n";
        o_ << "            }\n";
        o_ << "        se_ic_elements_.push_back(a);\n";
        o_ << "        return;\n";
        o_ << "    }\n";
    }
    bool any_scalar = false;
    for (const LoadRow& r : rows) any_scalar = any_scalar || r.kind != LoadRow::Kind::Element;
    if (!any_scalar) {
        o_ << "    (void)a;\n}\n";
    } else {
        o_ << "    const double v = a.value;\n";
        o_ << "    switch (a.row) {\n";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const LoadRow& r = rows[i];
            if (r.kind == LoadRow::Kind::Element) continue;
            const std::string sc = scalar_of(r);
            const EnumInfo* e = enum_of(r);
            const std::string rhs = e                 ? "static_cast<" + e->cpp_name +
                                                            ">(static_cast<int>(v))"
                                    : sc == "double" ? std::string("v")
                                    : sc == "bool"   ? std::string("v != 0.0")
                                                     : "static_cast<" + sc + ">(v)";
            o_ << "        case " << i << ": " << member(r) << " = " << rhs << ";";
            if (r.kind == LoadRow::Kind::Setting) o_ << " se_pin_[" << i << "] = true;";
            if (r.kind == LoadRow::Kind::Run)
                o_ << "\n            se_end_tick_ = static_cast<std::uint64_t>("
                      "std::floor(v / step_seconds + 0.5));";
            o_ << " break;\n";
        }
        o_ << "        default: break;\n";
        o_ << "    }\n}\n";
    }

    // ── one row's value, out ─────────────────────────────────────────────────
    o_ << "\n// One row's current value, as a double: what dump_settings() writes. An\n";
    o_ << "// array state's row is its declared default, which every unbound element\n";
    o_ << "// holds.\n";
    o_ << "double Sim::Impl::value_of(std::size_t row) const {\n";
    if (!any_scalar) {
        o_ << "    (void)row;\n    return 0.0;\n}\n";
    } else {
        o_ << "    switch (row) {\n";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            const LoadRow& r = rows[i];
            if (r.kind == LoadRow::Kind::Element) {
                o_ << "        case " << i << ": return " << dbl(r.state->initial) << ";   // "
                   << r.path << "[*]\n";
                continue;
            }
            const std::string sc = scalar_of(r);
            const std::string m = member(r);
            const std::string v = enum_of(r)        ? "static_cast<double>(static_cast<int>(" + m + "))"
                                  : sc == "double" ? m
                                                   : "static_cast<double>(" + m + ")";
            o_ << "        case " << i << ": return " << v << ";\n";
        }
        o_ << "        default: return 0.0;\n";
        o_ << "    }\n}\n";
    }

    // ── an array state's run, for its element ICs ────────────────────────────
    if (any_element) {
        auto run = [&](const LoadRow& r, bool base) {
            const bool cont = is_cont(*r.state);
            const std::string idx = std::to_string(r.state->off);
            if (base) return std::string(cont ? "x + se_x_off_[" : "dis_arr + se_da_off_[") + idx + "]";
            return std::string(cont ? "se_x_len_[" : "se_da_len_[") + idx + "]";
        };
        o_ << "\n// An array state's extent and first element, by row (§6.4a).\n";
        o_ << "std::size_t Sim::Impl::element_extent(std::size_t row) const {\n";
        o_ << "    switch (row) {\n";
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].kind == LoadRow::Kind::Element)
                o_ << "        case " << i << ": return " << run(rows[i], false) << ";   // "
                   << rows[i].path << "\n";
        o_ << "        default: return 0;\n";
        o_ << "    }\n}\n";
        o_ << "\ndouble* Sim::Impl::element_base(std::size_t row) {\n";
        o_ << "    switch (row) {\n";
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].kind == LoadRow::Kind::Element)
                o_ << "        case " << i << ": return " << run(rows[i], true) << ";   // "
                   << rows[i].path << "\n";
        o_ << "        default: return nullptr;\n";
        o_ << "    }\n}\n";
    }

    const char* settle = m_.dynamic_states ? "configure();" : "resolve_settings();";

    // ── load ─────────────────────────────────────────────────────────────────
    o_ << "\n// §14.1 — a settings source, all of it or none of it. Every binding is\n";
    o_ << "// checked before any is applied; a refusal stands, and init() will not run.\n";
    o_ << "bool Sim::Impl::load_settings(const char* text, const char* origin) {\n";
    o_ << "    if (se_started_) {\n";
    o_ << "        se_load_error_ = \"settings cannot be loaded after init(): configuration \"\n";
    o_ << "                         \"is over (§6.2b)\";\n";
    o_ << "        return false;\n";
    o_ << "    }\n";
    o_ << "    if (!se_load_error_.empty()) return false;   // an earlier refusal stands\n";
    o_ << "    se_rt::SourceReader reader(text, origin);\n";
    o_ << "    std::vector<se_rt::Assignment> got;\n";
    o_ << "    std::vector<se_rt::RunEdit> edits;\n";
    o_ << "    if (!reader.check(tables(), got, edits)) {\n";
    o_ << "        se_load_error_ = reader.error();\n";
    o_ << "        return false;\n";
    o_ << "    }\n";
    o_ << "    // The host's edits through settings() first, so this source is the\n";
    o_ << "    // later writer (§13.3).\n";
    o_ << "    const std::string refused = accept_settings();\n";
    o_ << "    for (const se_rt::Assignment& a : got) assign(a);\n";
    o_ << "    for (const se_rt::RunEdit& e : edits) se_rt::apply(se_config_, e);\n";
    o_ << "    settle();\n";
    o_ << "    if (!refused.empty()) se_load_error_ = refused;\n";
    o_ << "    return se_load_error_.empty() && se_cfg_error_.empty();\n";
    o_ << "}\n";

    // ── settle ───────────────────────────────────────────────────────────────
    o_ << "\n// §6.2b step 3 onwards, then the root settings back out to settings().\n";
    o_ << "std::string Sim::Impl::settle() {\n";
    o_ << "    " << settle << "\n";
    o_ << "    check_columns();\n";
    o_ << "    publish_settings();\n";
    o_ << "    return se_cfg_error_;\n";
    o_ << "}\n";

    // ── dump ─────────────────────────────────────────────────────────────────
    o_ << "\n// §14.1 — the recipe for the run: every setting and initial condition,\n";
    o_ << "// the recording and the log, as a §14 source that load_settings() reads\n";
    o_ << "// back. Before init() it first takes the host's pending edits, so the\n";
    o_ << "// dump is what init() would run with.\n";
    o_ << "const char* Sim::Impl::dump_settings() {\n";
    o_ << "    if (!se_started_) {\n";
    o_ << "        const std::string refused = accept_settings();\n";
    o_ << "        settle();\n";
    o_ << "        if (!refused.empty() && se_load_error_.empty()) se_load_error_ = refused;\n";
    o_ << "    }\n";
    o_ << "    std::size_t n = 0;\n";
    o_ << "    const se_rt::SettingRow* rows = setting_rows(n);\n";
    o_ << "    std::vector<double> values(n, 0.0);\n";
    o_ << "    for (std::size_t r = 0; r < n; ++r) values[r] = value_of(r);\n";
    o_ << "    se_dump_ = se_rt::dump_source(" << quote(m_.name)
       << ", rows, n, values.data(), se_ic_elements_) +\n"
          "               se_rt::dump_run(se_config_);\n";
    o_ << "    return se_dump_.c_str();\n";
    o_ << "}\n";

    // ── one key at a time ────────────────────────────────────────────────────
    o_ << "\n// §14.1 — the same paths one at a time, with structured answers. Before\n";
    o_ << "// init() each first takes the host's pending settings() edits, as the\n";
    o_ << "// dump does, so what it reports is what init() would run with.\n";
    o_ << "std::vector<Sim::SettingInfo> Sim::Impl::settings_list() {\n";
    o_ << "    if (!se_started_) {\n";
    o_ << "        const std::string refused = accept_settings();\n";
    o_ << "        settle();\n";
    o_ << "        if (!refused.empty() && se_load_error_.empty()) se_load_error_ = refused;\n";
    o_ << "    }\n";
    o_ << "    std::size_t n = 0;\n";
    o_ << "    const se_rt::SettingRow* rows = setting_rows(n);\n";
    o_ << "    std::vector<Sim::SettingInfo> out;\n";
    o_ << "    for (std::size_t r = 0; r < n; ++r) {\n";
    o_ << "        Sim::SettingInfo s;\n";
    o_ << "        s.kind = rows[r].kind == se_rt::RowKind::Setting ? Sim::SettingKind::Setting\n";
    o_ << "                 : rows[r].kind == se_rt::RowKind::Run   ? Sim::SettingKind::Run\n";
    o_ << "                                                         : Sim::SettingKind::Initial;\n";
    o_ << "        s.unit = rows[r].unit;\n";
    o_ << "        s.type = rows[r].type;\n";
    o_ << "        s.structural = rows[r].structural;\n";
    if (any_element) {
        o_ << "        if (rows[r].kind == se_rt::RowKind::Element) {   // one row per element\n";
        o_ << "            for (std::size_t i = 0; i < element_extent(r); ++i) {\n";
        o_ << "                s.path = std::string(rows[r].path) + \"[\" + std::to_string(i) + \"]\";\n";
        o_ << "                s.value = value_of(r);\n";
        o_ << "                for (const se_rt::Assignment& a : se_ic_elements_)\n";
        o_ << "                    if (a.row == r && a.index == i) s.value = a.value;\n";
        o_ << "                out.push_back(s);\n";
        o_ << "            }\n";
        o_ << "            continue;\n";
        o_ << "        }\n";
    }
    o_ << "        s.path = rows[r].path;\n";
    o_ << "        s.value = value_of(r);\n";
    o_ << "        out.push_back(s);\n";
    o_ << "    }\n";
    o_ << "    return out;\n";
    o_ << "}\n";

    o_ << "\ndouble Sim::Impl::get(const char* path) {\n";
    o_ << "    se_call_error_.clear();\n";
    o_ << "    if (!se_started_) {\n";
    o_ << "        const std::string refused = accept_settings();\n";
    o_ << "        settle();\n";
    o_ << "        if (!refused.empty() && se_load_error_.empty()) se_load_error_ = refused;\n";
    o_ << "    }\n";
    o_ << "    const std::string p = path ? path : \"\";\n";
    o_ << "    const std::size_t lb = p.find('[');\n";
    o_ << "    const bool indexed = lb != std::string::npos;\n";
    o_ << "    const std::string stem = p.substr(0, lb);\n";
    if (any_element)
        o_ << "    const std::size_t index = indexed ? "
              "static_cast<std::size_t>(std::strtoull(p.c_str() + lb + 1, nullptr, 10)) : 0;\n";
    o_ << "    std::size_t n = 0;\n";
    o_ << "    const se_rt::SettingRow* rows = setting_rows(n);\n";
    o_ << "    for (std::size_t r = 0; r < n; ++r) {\n";
    o_ << "        if (stem != rows[r].path) continue;\n";
    o_ << "        const bool element = rows[r].kind == se_rt::RowKind::Element;\n";
    o_ << "        if (element != indexed) {\n";
    o_ << "            se_call_error_ = p + (indexed ? \": not an array state, so it takes no index\"\n";
    o_ << "                                          : \": an array state is read one element at a \"\n";
    o_ << "                                            \"time, as `\" + p + \"[0]`\");\n";
    o_ << "            return std::nan(\"\");\n";
    o_ << "        }\n";
    if (any_element) {
        o_ << "        if (element) {\n";
        o_ << "            if (index >= element_extent(r)) {\n";
        o_ << "                se_call_error_ = se_rt::index_error(p.c_str(), element_extent(r));\n";
        o_ << "                return std::nan(\"\");\n";
        o_ << "            }\n";
        o_ << "            for (const se_rt::Assignment& a : se_ic_elements_)\n";
        o_ << "                if (a.row == r && a.index == index) return a.value;\n";
        o_ << "        }\n";
    }
    o_ << "        return value_of(r);\n";
    o_ << "    }\n";
    o_ << "    se_call_error_ = p + \": not a setting, a state or sim.duration of this model\";\n";
    o_ << "    return std::nan(\"\");\n";
    o_ << "}\n";

    o_ << "\n// In the canonical unit. A finite type's value is written as the name of\n";
    o_ << "// its enumerator, so the reader checks it as it checks any other.\n";
    o_ << "bool Sim::Impl::set(const char* path, double value) {\n";
    o_ << "    if (!configuring(\"set\")) return false;\n";
    o_ << "    const std::string p = path ? path : \"\";\n";
    o_ << "    const std::string stem = p.substr(0, p.find('['));\n";
    o_ << "    std::size_t n = 0;\n";
    o_ << "    const se_rt::SettingRow* rows = setting_rows(n);\n";
    o_ << "    for (std::size_t r = 0; r < n; ++r) {\n";
    o_ << "        if (stem != rows[r].path || !rows[r].enumerators) continue;\n";
    o_ << "        for (std::size_t k = 0; k < rows[r].n_enumerators; ++k)\n";
    o_ << "            if (static_cast<double>(rows[r].enumerators[k].value) == value)\n";
    o_ << "                return set_text(p, rows[r].enumerators[k].name);\n";
    o_ << "        se_call_error_ = p + \": \" + se_rt::source_number(value) + \" is not a value of \" +\n";
    o_ << "                         rows[r].type;\n";
    o_ << "        return false;\n";
    o_ << "    }\n";
    o_ << "    return set_text(p, se_rt::source_number(value));\n";
    o_ << "}\n";

    o_ << "\n// As a source line would have it: \"340 (kg)\", \"0.34 (t)\", \"CONNECTED\".\n";
    o_ << "bool Sim::Impl::set(const char* path, const char* value) {\n";
    o_ << "    if (!configuring(\"set\")) return false;\n";
    o_ << "    const std::string v = value ? value : \"\";\n";
    o_ << "    if (v.find(';') != std::string::npos || v.find('\\n') != std::string::npos) {\n";
    o_ << "        se_call_error_ = std::string(path ? path : \"\") + \": one value, as `340 (kg)`\";\n";
    o_ << "        return false;\n";
    o_ << "    }\n";
    o_ << "    return set_text(path ? path : \"\", v);\n";
    o_ << "}\n";

    o_ << "\n// One binding, read and checked as a source's would be, then applied as a\n";
    o_ << "// later writer (§13.3). A refused one changes nothing.\n";
    o_ << "bool Sim::Impl::set_text(const std::string& path, const std::string& value) {\n";
    o_ << "    if (!se_load_error_.empty()) return false;   // a refused source stands\n";
    o_ << "    const std::string text = path + \" = \" + value + \";\";\n";
    o_ << "    se_rt::SourceReader reader(text.c_str(), \"set\", false);\n";
    o_ << "    std::vector<se_rt::Assignment> got;\n";
    o_ << "    std::vector<se_rt::RunEdit> edits;\n";
    o_ << "    if (!reader.check(tables(), got, edits) || got.size() != 1 || !edits.empty()) {\n";
    o_ << "        se_call_error_ = !reader.error().empty()\n";
    o_ << "                             ? reader.error()\n";
    o_ << "                             : path + \": not one setting, state or sim.duration\";\n";
    o_ << "        return false;\n";
    o_ << "    }\n";
    o_ << "    const std::string refused = accept_settings();\n";
    o_ << "    assign(got[0]);\n";
    o_ << "    settle();\n";
    o_ << "    if (!refused.empty() && se_load_error_.empty()) se_load_error_ = refused;\n";
    o_ << "    return se_cfg_error_.empty() && se_load_error_.empty();\n";
    o_ << "}\n";
}

// The public interface, each call one line onto Sim::Impl. Nothing here has any
// logic of its own, which is the point: Sim.hpp promises exactly what Impl does.
void Emitter::sim_facade() {
    o_ << "\n// -- Sim: the public interface, forwarded " << std::string(37, '-') << "\n";
    o_ << "Sim::Sim() : impl_(std::make_unique<Impl>()) {}\n";
    o_ << "Sim::~Sim() = default;\n";
    o_ << "Sim::Sim(Sim&&) noexcept = default;\n";
    o_ << "Sim& Sim::operator=(Sim&&) noexcept = default;\n\n";
    o_ << "Sim::Settings&      Sim::settings()         { return impl_->se_settings_; }\n";
    o_ << "bool                Sim::load_settings(const char* text, const char* origin) {\n";
    o_ << "    return impl_->load_settings(text, origin);\n";
    o_ << "}\n";
    o_ << "const char*         Sim::dump_settings()    { return impl_->dump_settings(); }\n";
    o_ << "const char*         Sim::list_signals()     { return impl_->list_signals(); }\n";
    o_ << "std::vector<Sim::ChannelInfo> Sim::channels_list() { return impl_->channels_list(); }\n";
    o_ << "bool          Sim::record(const char* path)    { return impl_->record(path, true); }\n";
    o_ << "bool          Sim::unrecord(const char* path)  { return impl_->record(path, false); }\n";
    o_ << "void          Sim::record_clear()              { impl_->record_clear(); }\n";
    o_ << "bool          Sim::record_file(const char* f)  { return impl_->record_file(f); }\n";
    o_ << "const char*   Sim::record_file() const         { return impl_->record_file_name(); }\n";
    o_ << "bool          Sim::record_every(std::uint64_t n) { return impl_->record_every(n); }\n";
    o_ << "std::uint64_t Sim::record_every() const        { return impl_->se_config_.every; }\n";
    o_ << "bool          Sim::log_file(const char* f)     { return impl_->log_file(f); }\n";
    o_ << "const char*   Sim::log_file() const            { return impl_->log_file_name(); }\n";
    o_ << "bool          Sim::log_level(const char* l)    { return impl_->log_level(nullptr, l); }\n";
    o_ << "bool          Sim::log_level(const char* p, const char* l) "
          "{ return impl_->log_level(p, l); }\n";
    o_ << "bool                Sim::init()             { return impl_->start(); }\n";
    o_ << "const char*         Sim::error() const      { return impl_->error(); }\n";
    o_ << "Sim::Inputs&        Sim::inputs()           { return impl_->sig.in; }\n";
    o_ << "void                Sim::tick()             { impl_->tick(); }\n";
    o_ << "void                Sim::sync_realtime()    { impl_->sync_realtime(); }\n";
    o_ << "const Sim::Outputs& Sim::outputs() const    { return impl_->se_outputs_; }\n";
    o_ << "bool                Sim::done() const       { return impl_->done(); }\n";
    o_ << "std::uint64_t       Sim::tick_index() const { return impl_->tick_index(); }\n";
    o_ << "std::uint64_t       Sim::tick_count() const { return impl_->se_end_tick_; }\n";
    o_ << "std::vector<Sim::SettingInfo> Sim::settings_list() { return impl_->settings_list(); }\n";
    o_ << "double        Sim::get(const char* path) const { return impl_->get(path); }\n";
    o_ << "bool          Sim::set(const char* path, double v) { return impl_->set(path, v); }\n";
    o_ << "bool          Sim::set(const char* path, const char* v) { return impl_->set(path, v); }\n";
    o_ << "double              Sim::time() const       "
          "{ return static_cast<double>(tick_index()) * step_seconds; }\n";
    o_ << "Sim::RealtimeStats  Sim::realtime_stats() const {\n";
    o_ << "    const se_rt::Realtime& r = impl_->se_run_.realtime;\n";
    o_ << "    return {r.utilization(), r.overrun_pct(), r.lag(), r.frames(), r.overruns()};\n";
    o_ << "}\n";
    o_ << "int           Sim::channel_handle(const char* p) { return impl_->channel_handle(p); }\n";
    o_ << "double        Sim::read(int handle) const      { return impl_->read(handle); }\n";
    o_ << "void          Sim::log_capture(bool on)        { impl_->se_run_.log.capture(on); }\n";
    o_ << "bool          Sim::log_next(LogLine& line) {\n";
    o_ << "    se_rt::LogLine l;\n";
    o_ << "    if (!impl_->se_run_.log.next(l)) return false;\n";
    o_ << "    line.level = static_cast<LogLevel>(static_cast<int>(l.level));\n";
    o_ << "    line.path  = std::move(l.path);\n";
    o_ << "    line.time  = l.time;\n";
    o_ << "    line.text  = std::move(l.text);\n";
    o_ << "    return true;\n";
    o_ << "}\n";
    o_ << "int                 Sim::finish()           "
          "{ impl_->finish(); return impl_->exit_code(); }\n";
}

// ─── The block offset tables (§15.6) ─────────────────────────────────────────
//  Emitted after the class, because offsetof needs the complete type. The
//  offsets are the C++ compiler's, not the DSL compiler's: padding is not
//  something `sec` can know, and a guessed table would be worse than none.

std::vector<Emitter::SlotRow> Emitter::signal_rows() const {
    std::vector<SlotRow> rows;
    // A record occupies several consecutive slots, and a host wants each one
    // named, so walk every port type down to its scalar leaves.
    std::function<void(const std::string&, const std::string&, const Type&)> go =
        [&](const std::string& p, const std::string& mem, const Type& ty) {
            if (!ty.is_record) {
                rows.push_back({p, mem, ty.scalar, ty.unit.str()});
                return;
            }
            for (const Field& f : ty.record->fields)
                go(p + "." + f.name, mem + "." + f.name, field_type(ty, f));
        };

    for (const BoundaryIn& b : m_.boundary_in) go(b.path, "in." + b.ident, b.type);
    // §6.2a — `leaf.ports_out`, not `leaf.node->outputs`: the definition only
    // knows `(U)`, and the manifest promises a real unit.
    for (const Leaf& leaf : m_.leaves)
        for (const Field& f : leaf.ports_out)
            go(leaf.path.empty() ? f.name : leaf.path + "." + f.name,
               leaf.ident + "." + f.name, f.type);
    return rows;
}

std::vector<Emitter::SlotRow> Emitter::discrete_rows() const {
    std::vector<SlotRow> rows;
    for (const Leaf& leaf : m_.leaves)
        for (const StateSlot& s : leaf.states) {
            if (!is_disc(s)) continue;
            // §15.5a — an array-shaped discrete state is not in `dis` at
            // all. It lives in the flat fourth block, whose rows are built at
            // configuration because their count is not known before then.
            if (s.extent.present) continue;
            rows.push_back({leaf.path.empty() ? s.name : leaf.path + "." + s.name,
                            leaf.ident + "." + s.name, s.scalar, s.unit.str()});
        }
    return rows;
}

// §6.4c — the finite block's rows: a generated struct for `dis`'s reasons,
// and never array-shaped, so every row is a plain offsetof one.
std::vector<Emitter::SlotRow> Emitter::finite_rows() const {
    std::vector<SlotRow> rows;
    for (const Leaf& leaf : m_.leaves)
        for (const StateSlot& s : leaf.states)
            if (is_fin(s))
                rows.push_back({leaf.path.empty() ? s.name : leaf.path + "." + s.name,
                                leaf.ident + "." + s.name, s.scalar, s.unit.str()});
    return rows;
}

// §6.4a — the array-shaped states, in block order, as
// { model path, extent as written, "double", unit }. Every row is a RUN of
// slots whose length is a configuration result, which is why this feeds the
// manifest's `dynamic` rows rather than an offsetof table.
std::vector<Emitter::SlotRow> Emitter::array_state_rows(bool continuous) const {
    std::vector<SlotRow> rows;
    for (const Leaf& leaf : m_.leaves)
        for (const StateSlot& s : leaf.states) {
            if (is_cont(s) != continuous || !s.extent.present) continue;
            rows.push_back({leaf.path.empty() ? s.name : leaf.path + "." + s.name,
                            s.extent.text, "double", s.unit.str()});
        }
    return rows;
}

void Emitter::sim_maps() {
    auto emit_table = [&](const char* fn, const char* block,
                          const std::vector<SlotRow>& rows) {
        o_ << "\nconst se_rt::Slot* Sim::Impl::" << fn << "(std::size_t& count) {\n";
        if (rows.empty()) {
            o_ << "    count = 0;\n    return nullptr;\n}\n";
            return;
        }
        o_ << "    static const se_rt::Slot table[] = {\n";
        std::size_t wp = 0, wm = 0;
        for (const auto& r : rows) {
            wp = std::max(wp, r[0].size() + 2);
            wm = std::max(wm, r[1].size());
        }
        for (const auto& r : rows)
            o_ << "        {" << pad(quote(r[0]) + ",", wp + 1) << " offsetof(Sim::Impl::" << block
               << ", " << pad(r[1] + "),", wm + 2) << " sizeof(" << r[2] << "), "
               << quote(r[2]) << ", " << quote(r[3]) << "},\n";
        o_ << "    };\n    count = sizeof(table) / sizeof(table[0]);\n    return table;\n}\n";
    };

    emit_table("signal_map", "Signals", signal_rows());
    const std::vector<SlotRow> dis = discrete_rows();
    if (!dis.empty()) emit_table("discrete_map", "Discrete", dis);
    const std::vector<SlotRow> fin = finite_rows();
    if (!fin.empty()) emit_table("finite_map", "Finite", fin);

    // §15.6 — the dynamic blocks. These are not offsetof tables and cannot
    // be: the rows do not exist until configure() has run, so what is emitted
    // here is only the accessor onto what it built. A host reads the manifest
    // to learn what paths exist, and calls these to learn where they landed.
    if (!m_.dynamic_states) return;
    auto emit_dyn = [&](const char* fn, const char* store) {
        o_ << "\nconst se_rt::Slot* Sim::Impl::" << fn
           << "(std::size_t& count) const {\n";
        o_ << "    count = " << store << ".size();\n";
        o_ << "    return " << store << ".empty() ? nullptr : " << store << ".data();\n}\n";
    };
    emit_dyn("state_map", "se_state_slots_");
    if (m_.n_disc_arr_slots) emit_dyn("discrete_array_map", "se_dstate_slots_");
}

// ─── The three sources ───────────────────────────────────────────────────────
//
//  <stem>.hpp is the public interface and nothing else: the root node's
//  settings, inputs and outputs as typed structs, and the calls that configure,
//  start, step and finish a run. <stem>.cpp is everything behind it. main.cpp is
//  an example host that uses the header alone.

std::string Emitter::cpp_ns() const {
    std::string r;
    for (char c : m_.root_namespace) {
        if (c == '.') r += "::";
        else r += c;
    }
    return r;
}

std::uint64_t Emitter::tick_count() const {
    const double steps = m_.step > 0.0 ? m_.duration / m_.step : 0.0;
    return static_cast<std::uint64_t>(steps + 0.5);
}

std::vector<std::size_t> Emitter::root_settings() const {
    std::vector<std::size_t> r;
    for (std::size_t i = 0; i < m_.settings.size(); ++i)
        if (m_.settings[i].owner.empty()) r.push_back(i);
    return r;
}

// A root setting's default, as the .sim file left it: a scalar is its value, and
// a record is a brace list of its leaves in field order (§6.2c).
std::string Emitter::root_setting_default(const std::string& decl, const Type& t) const {
    if (!t.is_record) {
        for (std::size_t i : root_settings())
            if (m_.settings[i].name == decl)
                return literal(m_.settings[i].value, m_.settings[i].scalar, m_.settings[i].enm);
        return "{}";
    }
    std::string r = "{";
    for (std::size_t i = 0; i < t.record->fields.size(); ++i) {
        const Field& f = t.record->fields[i];
        if (i) r += ", ";
        r += root_setting_default(decl + "." + f.name, field_type(t, f));
    }
    return r + "}";
}

void Emitter::public_class() {

    // One member: type, name, default, and a unit comment, each in its own
    // column, because the unit is what a reader scans for.
    struct Member {
        std::string type, name, init, comment;
    };
    auto emit_rows = [&](const char* name, const std::vector<Member>& rows, const char* none) {
        o_ << "    struct " << name << " {";
        if (rows.empty()) {
            o_ << "};   // " << none << "\n";
            return;
        }
        std::size_t wt = 0, wn = 0, wi = 0;
        for (const Member& r : rows) {
            wt = std::max(wt, r.type.size());
            wn = std::max(wn, r.name.size());
            wi = std::max(wi, r.init.size());
        }
        o_ << "\n";
        for (const Member& r : rows)
            o_ << "        " << pad(r.type, wt) << " " << pad(r.name, wn) << " = "
               << pad(r.init + ";", wi + 1) << "   " << r.comment << "\n";
        o_ << "    };\n";
    };
    auto zero = [](const Type& t) { return t.is_record ? std::string("{}") : literal(0.0, t.scalar, t.enm); };

    std::vector<Member> settings;
    {
        std::set<std::string> seen;
        for (std::size_t i : root_settings()) {
            const SettingSlot& s = m_.settings[i];
            if (!seen.insert(s.decl).second) continue;
            Member r{cpp_type(s.decl_type), s.decl, root_setting_default(s.decl, s.decl_type),
                     "// " + (s.decl_type.is_record ? type_label(s.decl_type) : s.unit.str())};
            if (s.structural) r.comment += "   structural: fixed when generated";
            settings.push_back(r);
        }
    }
    std::vector<Member> inputs;
    for (const BoundaryIn& b : m_.boundary_in)
        inputs.push_back({cpp_type(b.type), b.ident, zero(b.type), "// " + type_label(b.type)});
    std::vector<Member> outputs;
    for (const RootOutput& r : m_.root_outputs)
        outputs.push_back({cpp_type(r.type), r.name, zero(r.type),
                           "// " + type_label(r.type) + (r.driven ? "" : "   undriven: always zero")});

    o_ << "class Sim {\n";
    o_ << "public:\n";
    o_ << "    // ---- the root node, " << m_.root->fq << " " << std::string(
                  m_.root->fq.size() < 52 ? 52 - m_.root->fq.size() : 1, '-') << "\n";
    o_ << "    //  Its settings, at the values " << m_.name << ".sim gives them.\n";
    emit_rows("Settings", settings, "the root declares no settings");
    o_ << "    //  Its inputs. The host writes these before every tick().\n";
    emit_rows("Inputs", inputs, "the root declares no inputs");
    o_ << "    //  Its outputs, as of the last tick().\n";
    emit_rows("Outputs", outputs, "the root declares no outputs");
    o_ << "    //  What sync_realtime() measures (§13.6), over the last 1 s of model time\n";
    o_ << "    //  except where it says the whole run.\n";
    o_ << "    struct RealtimeStats {\n";
    o_ << "        double        utilization = 0.0;   // %, of the model time spent computing\n";
    o_ << "        double        overrun_pct = 0.0;   // %, of calls that found the wall ahead\n";
    o_ << "        double        lag         = 0.0;   // s, the wall was ahead at the last call\n";
    o_ << "        std::uint64_t frames      = 0;     // sync_realtime() calls, the whole run\n";
    o_ << "        std::uint64_t overruns    = 0;     // of which overran, the whole run\n";
    o_ << "    };\n";
    o_ << "    //  One settable path (§14.1): a setting, an initial condition, or the\n";
    o_ << "    //  run's length, with its value in the canonical unit.\n";
    o_ << "    enum class SettingKind { Setting, Initial, Run };\n";
    o_ << "    struct SettingInfo {\n";
    o_ << "        std::string path;               // \"whl.radius\", \"bw.w1[3]\", \"sim.duration\"\n";
    o_ << "        SettingKind kind = SettingKind::Setting;\n";
    o_ << "        std::string unit;               // canonical; \"-\" for none\n";
    o_ << "        std::string type;               // double, int, bool, or a finite type\n";
    o_ << "        double      value = 0.0;        // in `unit`; an enumerator's value\n";
    o_ << "        bool        structural = false; // listed, never settable (§6.2)\n";
    o_ << "    };\n";
    o_ << "    //  One channel the run can record (§13.5), and whether it is selected.\n";
    o_ << "    struct ChannelInfo {\n";
    o_ << "        std::string path;               // \"whl.state.omega\", \"bw.state.w1[3]\"\n";
    o_ << "        std::string unit;               // the CSV header's bracket: a unit, a type\n";
    o_ << "        bool        selected = false;\n";
    o_ << "    };\n";
    o_ << "    //  One line the console would have received, kept for a host that\n";
    o_ << "    //  has none (§14.1).\n";
    o_ << "    enum class LogLevel { Trace, Debug, Info, Warn, Error };\n";
    o_ << "    struct LogLine {\n";
    o_ << "        LogLevel    level = LogLevel::Info;\n";
    o_ << "        std::string path;               // the node, or \"sim\"\n";
    o_ << "        double      time = 0.0;         // s, model time\n";
    o_ << "        std::string text;\n";
    o_ << "    };\n";
    o_ << "\n";
    const std::string step = dbl(m_.step) + ";", ticks = std::to_string(tick_count()) + ";";
    o_ << "    //  One tick. Fixed: every rate in the model is a count of it.\n";
    o_ << "    static constexpr double step_seconds = " << step << "   // s\n";
    (void)ticks;
    o_ << "\n";

    o_ << "    Sim();\n";
    o_ << "    ~Sim();\n";
    o_ << "    Sim(Sim&&) noexcept;\n";
    o_ << "    Sim& operator=(Sim&&) noexcept;\n\n";

    o_ << "    // ---- 1. configure: before init() " << std::string(40, '-') << "\n";
    o_ << "    Settings& settings();\n";
    o_ << "    //  A §14 settings source, as text: `front.left.mass = 340 (kg);`. It may\n";
    o_ << "    //  reach any setting or initial condition, not only the root's, and a\n";
    o_ << "    //  value's unit is checked and converted. All or nothing: false if any\n";
    o_ << "    //  line was refused, and error() says which and why. A refusal stands,\n";
    o_ << "    //  so init() will not run.\n";
    o_ << "    bool        load_settings(const char* text, const char* origin = \"settings\");\n";
    o_ << "    //  The recipe for the run as a §14 source: every setting and initial\n";
    o_ << "    //  condition, the recording and the log. load_settings() reads it back.\n";
    o_ << "    //  Valid until the next call.\n";
    o_ << "    const char* dump_settings();\n";
    o_ << "    //  Every channel as a `record` section, the unselected ones commented\n";
    o_ << "    //  out: save it, edit it, load it back. Valid until the next call.\n";
    o_ << "    const char* list_signals();\n\n";
    o_ << "    //  The same paths one at a time (§14.1). get() and set(path, double)\n";
    o_ << "    //  speak the canonical unit; set(path, text) takes what a source line\n";
    o_ << "    //  would, \"340 (kg)\" or an enumerator. A refused set() returns false,\n";
    o_ << "    //  changes nothing, and does not stop the run; error() says why, and\n";
    o_ << "    //  get() of a path the model does not have is NaN.\n";
    o_ << "    std::vector<SettingInfo> settings_list();\n";
    o_ << "    double        get(const char* path) const;\n";
    o_ << "    bool          set(const char* path, double value);\n";
    o_ << "    bool          set(const char* path, const char* value);\n\n";
    o_ << "    //  The recording one line of a `record` section at a time (§13.5):\n";
    o_ << "    //  record() is `path;`, unrecord() is `-path;`, record_clear() is\n";
    o_ << "    //  `clear;`. A refused call returns false, changes nothing, and does\n";
    o_ << "    //  not stop the run; error() says why.\n";
    o_ << "    std::vector<ChannelInfo> channels_list();\n";
    o_ << "    bool          record(const char* path);\n";
    o_ << "    bool          unrecord(const char* path);\n";
    o_ << "    void          record_clear();\n";
    o_ << "    //  A file name may hold `{time}` (§13.5): the getters give it as\n";
    o_ << "    //  configured, then, once init() opens the file, the name it opened.\n";
    o_ << "    bool          record_file(const char* file);\n";
    o_ << "    const char*   record_file() const;\n";
    o_ << "    bool          record_every(std::uint64_t ticks);\n";
    o_ << "    std::uint64_t record_every() const;\n";
    o_ << "    //  The log (§13.7): its file, \"\" for the console only, then the threshold,\n";
    o_ << "    //  overall or for one node path: trace, debug, info, warn or error.\n";
    o_ << "    bool          log_file(const char* file);\n";
    o_ << "    const char*   log_file() const;\n";
    o_ << "    bool          log_level(const char* level);\n";
    o_ << "    bool          log_level(const char* path, const char* level);\n\n";

    o_ << "    // ---- 2. start " << std::string(58, '-') << "\n";
    o_ << "    //  Applies settings(), sets every initial condition and runs every\n";
    o_ << "    //  node's init(). False if a setting was refused or a node stopped the\n";
    o_ << "    //  run: error() says why, and finish() is still owed. A Sim runs once:\n";
    o_ << "    //  a second init() is refused, and a fresh Sim is how to run again.\n";
    o_ << "    bool        init();\n";
    o_ << "    const char* error() const;\n\n";

    o_ << "    // ---- 3. step " << std::string(59, '-') << "\n";
    o_ << "    //  tick() samples the model at time(), with inputs() as the host left\n";
    o_ << "    //  them, then advances one step. outputs() hold what it sampled. It\n";
    o_ << "    //  never waits: the host paces the run.\n";
    o_ << "    Inputs&        inputs();\n";
    o_ << "    void           tick();\n";
    o_ << "    const Outputs& outputs() const;\n";
    o_ << "    bool           done() const;         // the run is over: tick_count(), or stopped\n";
    o_ << "    std::uint64_t  tick_count() const;   // the run's length in ticks: sim.duration\n";
    o_ << "    double         time() const;         // s, the instant the next tick() samples\n";
    o_ << "    std::uint64_t  tick_index() const;   // the next tick(); time() / step_seconds\n\n";
    o_ << "    //  To keep pace with wall time, tick() as many times as you like, then\n";
    o_ << "    //  sync_realtime(): it waits until the wall time since the first tick()\n";
    o_ << "    //  equals time(). If the wall is already past it, it returns at once,\n";
    o_ << "    //  and the ticks run flat out until the model has caught up.\n";
    o_ << "    void           sync_realtime();\n";
    o_ << "    //  How well the run is keeping pace. Zero until sync_realtime() is called.\n";
    o_ << "    RealtimeStats  realtime_stats() const;\n\n";
    o_ << "    //  Watching the run (§14.1); not configuration, so before or after\n";
    o_ << "    //  init(). channel_handle() resolves a path as record() does, -1 if\n";
    o_ << "    //  refused; read() is that channel as the last tick() sampled it, NaN\n";
    o_ << "    //  for a bad handle. log_capture(true) keeps the console's lines for\n";
    o_ << "    //  log_next() instead of printing them; the log file is unaffected.\n";
    o_ << "    int            channel_handle(const char* path);\n";
    o_ << "    double         read(int handle) const;\n";
    o_ << "    void           log_capture(bool on);\n";
    o_ << "    bool           log_next(LogLine& line);   // false when none is waiting\n\n";

    o_ << "    // ---- 4. finish " << std::string(57, '-') << "\n";
    o_ << "    //  Runs every node's final(), closes the recording and the log. 0 for a\n";
    o_ << "    //  clean run; 1 if it was aborted, failed, or broke a contract.\n";
    o_ << "    int finish();\n\n";

    o_ << "    // The model itself: the node instances, the state blocks, the schedule.\n";
    o_ << "    // Defined in " << gen_ << "; incomplete here, so nothing in this header\n";
    o_ << "    // depends on its layout.\n";
    o_ << "    struct Impl;\n\n";
    o_ << "private:\n";
    o_ << "    std::unique_ptr<Impl> impl_;\n";
    o_ << "};\n";
}

std::string Emitter::public_hpp() {
    Out saved;
    std::swap(saved, o_);

    const std::string ns = cpp_ns();
    const std::string sim_t = (ns.empty() ? "" : ns + "::") + "Sim";
    o_ << "// " << std::string(76, '-') << "\n";
    o_ << "//  " << hdr_ << " — GENERATED by sec from " << m_.name << ".sim. DO NOT EDIT.\n";
    o_ << "//\n";
    o_ << "//  The public interface of the " << m_.name << " simulation.\n";
    o_ << "//      root:      " << m_.root->fq << "\n";
    char hz[32] = "";
    if (m_.step > 0.0) std::snprintf(hz, sizeof hz, " (%g Hz)", 1.0 / m_.step);
    o_ << "//      base step: " << dbl(m_.step) << " s" << hz << ", " << tick_count()
       << " ticks per run\n";
    o_ << "//\n";
    o_ << "//  A host does four things, in this order (main.cpp is a complete one):\n";
    o_ << "//\n";
    {
        // The example, with its step numbers in one column.
        std::vector<std::pair<std::string, std::string>> ex;
        ex.push_back({sim_t + " sim;", ""});
        const std::vector<std::size_t> rs = root_settings();
        if (!rs.empty())
            ex.push_back({"sim.settings()." + m_.settings[rs.front()].name + " = ...;",
                          "1. configure, if needed"});
        ex.push_back({"if (!sim.init()) { ...sim.error()... }", "2. start"});
        ex.push_back({"while (!sim.done()) {", "3. step"});
        if (!m_.boundary_in.empty())
            ex.push_back({"    sim.inputs()." + m_.boundary_in.front().ident + " = ...;", ""});
        ex.push_back({"    sim.tick();", ""});
        ex.push_back({"    sim.sync_realtime();", "   only to keep pace with wall time"});
        if (!m_.root_outputs.empty())
            ex.push_back({"    ...sim.outputs()." + m_.root_outputs.front().name + "...", ""});
        ex.push_back({"}", ""});
        ex.push_back({"return sim.finish();", "4. finish"});
        std::size_t w = 0;
        for (const auto& e : ex) w = std::max(w, e.first.size());
        for (const auto& e : ex) {
            if (e.second.empty()) o_ << "//      " << e.first << "\n";
            else o_ << "//      " << pad(e.first, w) << "   // " << e.second << "\n";
        }
    }
    o_ << "//\n";
    o_ << "//  The implementation — the engine runtime, the node classes and the\n";
    o_ << "//  schedule — is in " << gen_ << " and is not part of this interface.\n";
    o_ << "// " << std::string(76, '-') << "\n";
    o_ << "#pragma once\n\n";
    o_ << "#include <cstdint>\n#include <memory>\n#include <string>\n#include <vector>\n\n";

    const std::set<const EnumInfo*> pub_e = public_enums();
    if (!pub_e.empty()) {
        o_ << "// ---- the enums the interface uses " << std::string(40, '-') << "\n";
        for (const EnumInfo* e : m_.program->enum_order)
            if (pub_e.count(e)) enum_def(o_, e);
    }
    const std::set<const RecordInfo*> pub = public_records();
    if (!pub.empty()) {
        o_ << "// ---- the record types the interface uses " << std::string(34, '-') << "\n";
        for (const RecordInfo* r : m_.program->record_order)
            if (pub.count(r)) record_def(o_, r);
    }

    if (!ns.empty()) o_ << "namespace " << ns << " {\n\n";
    public_class();
    if (!ns.empty()) o_ << "\n}  // namespace " << ns << "\n";

    std::string text = o_.str();
    std::swap(saved, o_);
    return text;
}

std::string Emitter::impl_cpp(bool separate_runtime) {
    o_ << "// " << std::string(76, '-') << "\n";
    o_ << "//  " << gen_ << " — GENERATED by sec from " << m_.name << ".sim. DO NOT EDIT.\n";
    o_ << "//\n";
    o_ << "//  The implementation of " << hdr_ << ", in this order:\n";
    if (!separate_runtime)
        o_ << "//    - the engine runtime (namespace se_rt), the same in every model\n";
    o_ << "//    - the record types the public header does not already define\n";
    o_ << "//    - one class per node type\n";
    o_ << "//    - Sim::Impl, the flat model: state blocks, signals, the schedule\n";
    o_ << "//    - Sim, the public interface, each call forwarded to Sim::Impl\n";
    o_ << "//\n";
    o_ << "//  Everything is generated EXCEPT the body of each node method, which is\n";
    o_ << "//  copied byte for byte from the `.se` file named in the `#line` above it.\n";
    o_ << "// " << std::string(76, '-') << "\n";
    o_ << "#include " << quote(hdr_) << "\n\n";
    // The runtime goes in ahead of everything model-shaped. Its guard is named
    // for a hash of its text, so a unity build of two models skips the second.
    if (separate_runtime)
        o_ << "#include \"se_runtime.hpp\"\n\n";
    else
        o_ << se_runtime_hpp() << "\n"
           << "// " << std::string(76, '=') << "\n"
           << "//  END OF THE ENGINE RUNTIME — the model begins here.\n"
           << "// " << std::string(76, '=') << "\n\n";
    o_ << "#include <array>\n#include <cstddef>\n#include <cstdint>\n#include <cstring>\n";
    o_ << "#include <memory>\n#include <string>\n#include <utility>\n";
    // §6.5 / §15.5a — the two places the lowering owns storage: an array
    // var, and the solver scratch plus slot rows of a dynamic model. Both are
    // sized once at configuration and neither is a published block.
    bool any_array_var = false;
    for (const Leaf& leaf : m_.leaves) any_array_var = any_array_var || !leaf.array_vars.empty();
    if (m_.dynamic_states || any_array_var) o_ << "#include <vector>\n";
    o_ << "\n";

    // Said once, here, rather than restated as `(void)x;` at forty use sites.
    // The generator cannot know whether a body reads `in` or logs, because it
    // never parses the body (I1) — so it declares everything and asserts the
    // consequence in one place.
    o_ << "// The generator never parses a verbatim body (I1), so it cannot know which\n";
    o_ << "// of the names it brings into scope that body actually uses. It declares\n";
    o_ << "// them all and says so once, here.\n";
    o_ << "#ifdef _MSC_VER\n";
    o_ << "#pragma warning(disable : 4100)   // unreferenced formal parameter\n";
    o_ << "#pragma warning(disable : 4189)   // local initialised but not referenced\n";
    o_ << "#pragma warning(disable : 4458)   // local `log` hides the trace-only member\n";
    o_ << "#elif defined(__GNUC__)\n";
    o_ << "#pragma GCC diagnostic ignored \"-Wunused-parameter\"\n";
    o_ << "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n";
    o_ << "#pragma GCC diagnostic ignored \"-Wshadow\"\n";
    o_ << "#endif\n\n";
    o_ << "using se_rt::state_ref;\nusing se_rt::value_ref;\n";
    if (m_.dynamic_states) o_ << "using se_rt::state_arr;\n";
    o_ << "\n";

    declarations();
    records();

    std::set<const NodeInfo*> done;
    for (const Leaf& leaf : m_.leaves)
        if (done.insert(leaf.node).second) leaf_class(*leaf.node);

    sim_class();
    return o_.str();
}

// The example host. It uses the public header and nothing else, so it is also
// the proof that the header is enough.
std::string Emitter::main_cpp() {
    const std::string ns = cpp_ns();
    const std::string sim_t = (ns.empty() ? "" : ns + "::") + "Sim";
    const std::string who = m_.name;
    Out t;
    t << "// " << std::string(76, '-') << "\n";
    t << "//  main.cpp — GENERATED by sec from " << m_.name << ".sim. An example host.\n";
    t << "//\n";
    t << "//  Runs the model the way " << m_.name << ".sim describes, through the public\n";
    t << "//  interface in " << hdr_ << " and nothing else. A program of your own does the\n";
    t << "//  same four things; copy this file to start one.\n";
    t << "// " << std::string(76, '-') << "\n";
    t << "#include " << quote(hdr_) << "\n\n";
    t << "#include <cmath>\n#include <cstdint>\n#include <cstdio>\n#include <cstdlib>\n"
         "#include <fstream>\n#include <iterator>\n#include <string>\n\n";

    {
        // The --set example is a real path of this model, at its real value.
        std::string eg = "PATH = VALUE";
        for (const SettingSlot& s : m_.settings) {
            if (s.structural) continue;
            eg = s.path + " = " + shown(s.value, s.scalar, s.enm);
            if (!s.enm && s.unit.str() != "-") eg += " (" + s.unit.str() + ")";
            break;
        }
        // One C string literal per line, each ending in an escaped newline.
        auto line = [](const std::string& s) {
            std::string q = quote(s);
            q.insert(q.size() - 1, "\\n");
            return q;
        };
        t << "static const char* const usage =\n";
        t << "    " << line("usage: " + m_.name +
                            " [--settings FILE...]... [--set PATH=VALUE]... [--dump-settings]"
                            " [--duration TIME] [--list-signals] [--realtime SYNC]")
          << "\n";
        t << "    " << line("  --settings FILE... load settings files (section 14), left to "
                            "right; a comma-separated list too")
          << "\n";
        t << "    " << line("  --set PATH=VALUE   load one binding, as --set \"" + eg + "\"")
          << "\n";
        t << "    " << line("  --duration TIME    run this long, as --duration 20s; a whole "
                            "number of steps")
          << "\n";
        t << "    " << line("  --dump-settings    print how the run is configured, and stop")
          << "\n";
        t << "    " << line("  --list-signals     print every channel as a record section, "
                            "and stop")
          << "\n";
        t << "    " << line("  --realtime SYNC    keep pace with wall time, catching up every "
                            "SYNC, as --realtime \"10 (ms)\"")
          << ";\n\n";
    }

    t << "// The whole file, or false.\n";
    t << "static bool read_file(const char* path, std::string& out) {\n";
    t << "    std::ifstream f(path, std::ios::binary);\n";
    t << "    if (!f) return false;\n";
    t << "    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());\n";
    t << "    return true;\n";
    t << "}\n\n";

    t << "// A time in seconds, written `0.01`, `10ms` or `10 (ms)`; negative if it is not one.\n";
    t << "static double read_time(const std::string& text) {\n";
    t << "    char* end = nullptr;\n";
    t << "    const double v = std::strtod(text.c_str(), &end);\n";
    t << "    if (end == text.c_str()) return -1.0;\n";
    t << "    std::string unit;\n";
    t << "    for (; *end; ++end)\n";
    t << "        if (*end != ' ' && *end != '(' && *end != ')') unit += *end;\n";
    t << "    if (unit.empty() || unit == \"s\") return v;\n";
    t << "    if (unit == \"ms\") return v * 1e-3;\n";
    t << "    if (unit == \"us\") return v * 1e-6;\n";
    t << "    return -1.0;\n";
    t << "}\n\n";

    t << "int main(int argc, char** argv) {\n";
    t << "    " << sim_t << " sim;\n\n";

    t << "    // 1. Configure. Every setting already holds the value " << m_.name << ".sim\n";
    t << "    //    gives it; change any of them here, before init().";
    const std::vector<std::size_t> rs = root_settings();
    const SettingSlot* example = nullptr;
    for (std::size_t i : rs)
        if (!m_.settings[i].structural) {
            example = &m_.settings[i];
            break;
        }
    if (example)
        t << " For example:\n    //        sim.settings()." << example->name << " = "
          << literal(example->value, example->scalar, example->enm) << ";   // "
          << example->unit.str() << "\n";
    else
        t << "\n";
    t << "    //    A settings source reaches any setting or initial condition by path,\n";
    t << "    //    the recording and the log, and here it comes from the command line\n";
    t << "    //    (section 14.1).\n";
    t << "    bool dump = false;\n";
    t << "    bool list = false;\n";
    t << "    bool realtime = false;     // --realtime: keep pace with wall time\n";
    t << "    std::uint64_t frame = 1;   // ticks between sync_realtime() calls\n";
    t << "    for (int i = 1; i < argc; ++i) {\n";
    t << "        const std::string arg = argv[i];\n";
    t << "        std::string text;\n";
    t << "        const char* origin = nullptr;\n";
    t << "        if (arg == \"--settings\" && i + 1 < argc) {\n";
    t << "            // Every argument up to the next option, each split at its commas, so\n";
    t << "            // `--settings a.settings, b.settings` reads the same from any shell.\n";
    t << "            while (i + 1 < argc && std::string(argv[i + 1]).compare(0, 2, \"--\") != 0) {\n";
    t << "                const std::string list_arg = argv[++i];\n";
    t << "                for (std::size_t at = 0; at <= list_arg.size();) {\n";
    t << "                    std::size_t comma = list_arg.find(',', at);\n";
    t << "                    if (comma == std::string::npos) comma = list_arg.size();\n";
    t << "                    std::string file = list_arg.substr(at, comma - at);\n";
    t << "                    at = comma + 1;\n";
    t << "                    while (!file.empty() && file.front() == ' ') file.erase(0, 1);\n";
    t << "                    while (!file.empty() && file.back() == ' ') file.pop_back();\n";
    t << "                    if (file.empty()) continue;\n";
    t << "                    if (!read_file(file.c_str(), text)) {\n";
    t << "                        std::fprintf(stderr, \"" << who << ": cannot read %s\\n\", file.c_str());\n";
    t << "                        return 2;\n";
    t << "                    }\n";
    t << "                    if (!sim.load_settings(text.c_str(), file.c_str())) {\n";
    t << "                        std::fprintf(stderr, \"" << who << ": %s\\n\", sim.error());\n";
    t << "                        return 2;\n";
    t << "                    }\n";
    t << "                }\n";
    t << "            }\n";
    t << "            continue;\n";
    t << "        } else if (arg == \"--set\" && i + 1 < argc) {\n";
    t << "            origin = \"--set\";\n";
    t << "            text = argv[++i];\n";
    t << "            if (text.empty() || text.back() != ';') text += ';';\n";
    t << "        } else if (arg == \"--duration\" && i + 1 < argc) {\n";
    t << "            // Shorthand for the binding `sim.duration = ...;` (section 13.3).\n";
    t << "            const double d = read_time(argv[++i]);\n";
    t << "            if (!(d > 0.0)) {\n";
    t << "                std::fprintf(stderr, \"" << who << ": --duration %s is not a time\\n\", argv[i]);\n";
    t << "                return 2;\n";
    t << "            }\n";
    t << "            char buf[64];\n";
    t << "            std::snprintf(buf, sizeof buf, \"sim.duration = %.17g;\", d);\n";
    t << "            origin = \"--duration\";\n";
    t << "            text = buf;\n";
    t << "        } else if (arg == \"--dump-settings\") {\n";
    t << "            dump = true;\n";
    t << "            continue;\n";
    t << "        } else if (arg == \"--list-signals\") {\n";
    t << "            list = true;\n";
    t << "            continue;\n";
    t << "        } else if (arg == \"--realtime\" && i + 1 < argc) {\n";
    t << "            const double sync = read_time(argv[++i]);\n";
    t << "            const double n = std::round(sync / " << sim_t << "::step_seconds);\n";
    t << "            if (!(sync > 0.0) || n < 1.0 ||\n";
    t << "                std::fabs(n * " << sim_t << "::step_seconds - sync) > 1e-9 * sync) {\n";
    t << "                std::fprintf(stderr, \"" << who
      << ": --realtime %s is not a whole number of %g s steps\\n\",\n";
    t << "                             argv[i], " << sim_t << "::step_seconds);\n";
    t << "                return 2;\n";
    t << "            }\n";
    t << "            realtime = true;\n";
    t << "            frame = static_cast<std::uint64_t>(n);\n";
    t << "            continue;\n";
    t << "        } else {\n";
    t << "            std::fputs(usage, arg == \"--help\" ? stdout : stderr);\n";
    t << "            return arg == \"--help\" ? 0 : 2;\n";
    t << "        }\n";
    t << "        if (!sim.load_settings(text.c_str(), origin)) {\n";
    t << "            std::fprintf(stderr, \"" << who << ": %s\\n\", sim.error());\n";
    t << "            return 2;\n";
    t << "        }\n";
    t << "    }\n";
    t << "    if (dump || list) {\n";
    t << "        std::fputs(dump ? sim.dump_settings() : sim.list_signals(), stdout);\n";
    t << "        return 0;\n";
    t << "    }\n\n";

    t << "    // 2. Start.\n";
    t << "    if (!sim.init()) {\n";
    t << "        std::fprintf(stderr, \"" << who << ": %s\\n\", sim.error());\n";
    t << "        return sim.finish();\n";
    t << "    }\n\n";

    t << "    // 3. Step. Flat out, or with --realtime, `frame` ticks at a time and then\n";
    t << "    //    wait for the wall clock: tick() itself never waits.";
    if (!m_.boundary_in.empty()) {
        std::string names;
        for (const BoundaryIn& b : m_.boundary_in) names += (names.empty() ? "" : ", ") + b.ident;
        t << " " << m_.name << ".sim has\n";
        t << "    //    no way to drive the root inputs, so this run holds them at zero; a\n";
        t << "    //    host of your own writes them before every tick().\n";
        t << "    std::fprintf(stderr, \"" << who << ": nothing drives the root inputs, \"\n";
        t << "                         \"so they stay at zero: " << names << "\\n\");\n";
    } else {
        t << "\n";
    }
    t << "    while (!sim.done()) {\n";
    t << "        for (std::uint64_t k = 0; k < frame && !sim.done(); ++k) {\n";
    if (!m_.boundary_in.empty()) {
        auto zero = [](const Type& ty) {
            return (ty.is_record ? std::string("{}") : literal(0.0, ty.scalar, ty.enm)) + ";";
        };
        std::size_t w = 0, wz = 0;
        for (const BoundaryIn& b : m_.boundary_in) {
            w = std::max(w, b.ident.size());
            wz = std::max(wz, zero(b.type).size());
        }
        for (const BoundaryIn& b : m_.boundary_in)
            t << "            sim.inputs()." << pad(b.ident, w) << " = " << pad(zero(b.type), wz)
              << "   // " << type_label(b.type) << "\n";
    }
    t << "            sim.tick();\n";
    if (!m_.root_outputs.empty()) {
        std::string names;
        for (const RootOutput& r : m_.root_outputs) names += (names.empty() ? "" : ", ") + r.name;
        t << "            // sim.outputs() now holds " << names << "\n";
        t << "            // at the instant this tick() sampled.\n";
    }
    t << "        }\n";
    t << "        if (realtime) sim.sync_realtime();\n";
    t << "    }\n\n";
    t << "    // 4. Finish: every node's final(), then the recording and the log close.\n";
    t << "    //    0 for a clean run; 1 if it was aborted, failed, or broke a contract.\n";
    t << "    return sim.finish();\n";
    t << "}\n";
    return t.str();
}

// — The topology diagram —
//
//  A picture of the model as it actually RUNS, which is why it is drawn from
//  the flat model rather than from the `.se` hierarchy. Composites have
//  evaporated by this point (§15.2), so a group below is the path prefix one
//  left behind and not a runtime object. Drawing the source hierarchy instead
//  would show what was WRITTEN; this shows what got built, which is the thing
//  whose behaviour anyone is trying to understand.
//
//  `topology()` is the one walk; every notation is a printer over the `Topo`
//  it returns. Mermaid is the text one: it renders in any markdown preview
//  and diffs, so it can be a checked expectation exactly as the manifest is.
//  "Nothing installed" holds only for a fenced block inside a `.md`, not for
//  a bare `.mmd`, so `emit()` writes it inside a ```mermaid fence; the
//  printer returns the diagram alone and the fence is added at the write.

namespace {

// What a wire carries, as a reader wants it: a unit for a scalar, the
// record's own name for a record (§5.3 — the whole record travels).
std::string topo_type(const Type& ty) {
    if (ty.buffer) return buffer_label(ty);   // §5.1a — bytes, no unit
    if (!ty.is_record) return ty.unit.str();
    const std::size_t dot = ty.record->fq.rfind('.');
    std::string r = dot == std::string::npos ? ty.record->fq : ty.record->fq.substr(dot + 1);
    // §5.2 — the binding, not just the type. `Vec3` alone would make two
    // ports look interchangeable when `Vec3(m)` into `Vec3(m/s)` is SE0410
    // and `Vec3(mm)` into `Vec3(m)` silently scales every field, so the
    // arguments ARE part of what crosses the wire.
    if (!ty.unit_args.empty()) {
        r += "(";
        for (std::size_t i = 0; i < ty.unit_args.size(); ++i)
            r += (i ? ", " : "") + ty.unit_args[i].str();
        r += ")";
    }
    return r;
}

}  // namespace

Topo Emitter::topology() const {
    Topo t;

    // Position in the §8.5 order, so each group lists its nodes in the order
    // the tick actually calls them.
    t.rank.assign(m_.leaves.size(), 0);
    for (std::size_t k = 0; k < m_.order.size(); ++k) t.rank[m_.order[k]] = k;

    // ---- the cluster tree: composites from the paths, groups from §6.9.3 ---
    //  Built lazily from the leaves upward, so a model with no groups gets
    //  exactly the clusters, in exactly the order, it always did.
    constexpr std::size_t npos = static_cast<std::size_t>(-1);
    std::map<std::string, std::size_t> composite_of;
    std::map<std::size_t, std::size_t> group_cluster;     // Model::groups index -> cluster
    t.clusters.push_back(Topo::Cluster{});
    composite_of[std::string()] = 0;

    auto attach = [&](Topo::Cluster c, std::size_t parent) {
        c.parent = parent;
        t.clusters.push_back(std::move(c));
        const std::size_t id = t.clusters.size() - 1;
        t.clusters[parent].subs.push_back(id);
        return id;
    };
    std::function<std::size_t(const std::string&)> composite;
    std::function<std::size_t(std::size_t)> group;
    // The box an instance at `path` is drawn in: its innermost group, or else
    // the composite it is a child of.
    auto container = [&](const std::string& path) -> std::size_t {
        auto g = m_.instance_group.find(path);
        if (g != m_.instance_group.end()) return group(g->second);
        const std::size_t dot = path.rfind('.');
        return composite(dot == std::string::npos ? std::string() : path.substr(0, dot));
    };
    composite = [&](const std::string& path) -> std::size_t {
        auto it = composite_of.find(path);
        if (it != composite_of.end()) return it->second;
        const std::size_t parent = container(path);
        const std::size_t dot = path.rfind('.');
        Topo::Cluster c;
        c.name = dot == std::string::npos ? path : path.substr(dot + 1);
        c.path = path;
        c.collapsed = m_.collapsed.count(path) != 0;
        const std::size_t id = attach(std::move(c), parent);
        composite_of[path] = id;
        return id;
    };
    group = [&](std::size_t gi) -> std::size_t {
        auto it = group_cluster.find(gi);
        if (it != group_cluster.end()) return it->second;
        const Model::Group& g = m_.groups[gi];
        const std::size_t parent = g.parent != npos ? group(g.parent) : composite(g.owner);
        Topo::Cluster c;
        c.is_group = true;
        c.name = g.label;
        c.path = g.owner;
        c.file = g.file;
        c.line = g.line;
        c.doc = g.doc;
        c.collapsed = g.collapsed;
        const std::size_t id = attach(std::move(c), parent);
        group_cluster[gi] = id;
        return id;
    };

    for (std::size_t i = 0; i < m_.leaves.size(); ++i) {
        const std::string& path = m_.leaves[i].path;
        // The root itself may be a leaf, and it is in no group.
        t.clusters[path.empty() ? 0 : container(path)].leaves.push_back(i);
    }
    for (Topo::Cluster& c : t.clusters)
        std::sort(c.leaves.begin(), c.leaves.end(),
                  [&](std::size_t a, std::size_t b) { return t.rank[a] < t.rank[b]; });

    // ---- the root boundary the host drives ---------------------------------
    for (const BoundaryIn& b : m_.boundary_in) t.in_type.push_back(topo_type(b.type));

    // ---- the root outputs the host may read --------------------------------
    //  `boundary_out` is expanded field by field, because that is what a host
    //  wiring itself up needs (§15.6). A diagram wants the PORT, so the rows
    //  are collapsed back onto the producer they all came from.
    std::map<std::string, std::size_t> leaf_by_ident;
    for (std::size_t i = 0; i < m_.leaves.size(); ++i) leaf_by_ident[m_.leaves[i].ident] = i;

    for (const BoundaryOut& b : m_.boundary_out) {
        if (b.expr.rfind("sig.", 0) != 0) continue;
        const std::string rest = b.expr.substr(4);
        const std::size_t d1 = rest.find('.');
        if (d1 == std::string::npos) continue;
        const std::string ident = rest.substr(0, d1);
        const std::string after = rest.substr(d1 + 1);
        const std::size_t d2 = after.find('.');
        const std::string port = d2 == std::string::npos ? after : after.substr(0, d2);
        bool seen = false;
        for (const Topo::Output& o : t.outs) seen = seen || (o.ident == ident && o.port == port);
        if (seen) continue;
        Topo::Output o;
        o.ident = ident;
        o.port = port;
        o.leaf = std::string::npos;
        o.type = "?";
        auto it = leaf_by_ident.find(ident);
        if (it != leaf_by_ident.end()) {
            const Leaf& leaf = m_.leaves[it->second];
            o.leaf = it->second;
            o.path = leaf.path.empty() ? port : leaf.path + "." + port;
            for (const Field& f : leaf.ports_out)
                if (f.name == port) o.type = topo_type(f.type);
            for (const RootOutput& r : m_.root_outputs)
                if (r.src.kind == InputSource::Kind::Leaf && r.src.producer == it->second &&
                    r.src.port == port) {
                    o.via = r.src.via;
                    break;
                }
        }
        t.outs.push_back(o);
    }

    // A setting source's node: found by path, or made in its owner's box.
    std::map<std::string, std::size_t> setting_of;
    auto setting_node = [&](const ExprTok& ref) -> std::size_t {
        const std::string path = ref.owner.empty() ? ref.text : ref.owner + "." + ref.text;
        auto it = setting_of.find(path);
        if (it != setting_of.end()) return it->second;
        Topo::Setting st;
        st.owner = ref.owner;
        st.name = ref.text;
        st.path = path;
        st.cluster = composite(ref.owner);
        st.type = "?";
        for (const SettingSlot& slot : m_.settings) {
            if (slot.owner != ref.owner) continue;
            if (slot.decl == ref.text) { st.type = topo_type(slot.decl_type); break; }
            if (slot.name == ref.text) { st.type = slot.unit.str(); break; }  // one field of a record
        }
        t.settings.push_back(std::move(st));
        setting_of[path] = t.settings.size() - 1;
        return t.settings.size() - 1;
    };

    // ---- wires -------------------------------------------------------------
    for (std::size_t i = 0; i < m_.leaves.size(); ++i) {
        const Leaf& leaf = m_.leaves[i];
        for (const Field& port : leaf.ports_in) {
            auto it = leaf.inputs.find(port.name);
            if (it == leaf.inputs.end()) continue;   // unconnected; SE0430 said so
            const InputSource& src = it->second;

            Topo::Edge e;
            e.dst = i;
            e.dst_port = port.name;
            // The SAME question schedule.cpp asks when it builds the sort: only
            // an output method's parameter list constrains ordering (§8.3), and
            // with groups (§8.3a) the port orders the schedule if any lists it.
            for (const NodeInfo::OutputGroup& g : leaf.node->out_groups)
                for (const std::string& q : g.method->params)
                    if (q == port.name) e.feedthrough = true;

            if (src.kind == InputSource::Kind::Boundary) {
                e.from = Topo::Edge::From::Boundary;
                e.boundary = src.boundary.substr(std::strlen("sig.in."));
            } else if (src.kind == InputSource::Kind::Param ||
                       src.kind == InputSource::Kind::Const) {
                // §6.9.2 — a constant source has no producing leaf, so there is
                // no node in the graph to draw the edge from. It gets a
                // terminal of its own rather than being attributed to whichever
                // leaf happens to sit at `producer`'s default index.
                e.from = Topo::Edge::From::Const;
                e.src = t.consts.size();
                t.consts.push_back(src.kind == InputSource::Kind::Const ? dbl(src.value)
                                                                        : render_expr(src.expr));
                if (src.kind == InputSource::Kind::Param && src.expr.size() == 1 &&
                    src.expr.front().kind == ExprTok::Kind::Param)
                    e.setting = setting_node(src.expr.front());
            } else {
                e.from = Topo::Edge::From::Leaf;
                e.src = src.producer;
                e.src_port = src.port;
            }

            // A conversion folded to a constant at elaboration (§4.2) is
            // invisible in the generated code, so this is the only place it
            // shows: the two declared units, in the direction they convert.
            e.type = topo_type(port.type);
            if (src.converts() && src.kind == InputSource::Kind::Leaf) {
                const Leaf& prod = m_.leaves[src.producer];
                for (const Field& f : prod.ports_out)
                    if (f.name == src.port && !f.type.is_record)
                        e.type = f.type.unit.str() + " to " + e.type;
            }
            e.via = src.via;
            t.edges.push_back(e);
        }
    }
    return t;
}

std::string Emitter::topology_mermaid(const Topo& topo) const {
    Out t;

    // `#` opens a Mermaid entity code and `"` closes a label. Neither is
    // hypothetical: a file-private declaration's fq name carries a `#`
    // (`Checks#Vec3`), and a unit can be arbitrary text.
    auto esc = [](const std::string& in) {
        std::string r;
        for (char c : in) {
            if (c == '#') r += "#35;";
            else if (c == '"') r += "#quot;";
            else r += c;
        }
        return r;
    };
    auto lab = [&](const std::string& in) { return "\"" + esc(in) + "\""; };

    // A generated identifier is unique (I5) and C-safe, which is nearly enough
    // to be a Mermaid node id as well. The gap is Mermaid's own keywords: a
    // leaf called `end` would close the enclosing subgraph instead of naming a
    // node, and the diagram would silently draw the wrong shape rather than
    // fail. Renaming the handful of collisions keeps every other id readable.
    auto id = [](const std::string& ident) {
        static const char* const reserved[] = {"end",   "graph", "subgraph", "class",
                                               "click", "style", "linkStyle", "direction",
                                               "flowchart"};
        for (const char* r : reserved)
            if (ident == r) return ident + "_";
        return ident;
    };

    // ---- header ------------------------------------------------------------
    //  One comment line. It is a function rather than a literal for one
    //  reason, and it is a sharp one: Mermaid's comment stripper requires at
    //  least one character after the `%%`, so a BARE `%%` line is not a
    //  comment. It survives into the parser, concatenates with whatever
    //  follows -- three of them in a row become `%%%%%%flowchart` -- and the
    //  whole diagram fails to render with an error that points at line 1 and
    //  names none of this. An empty line is therefore emitted as an actual
    //  blank line, which Mermaid does accept, and a bare `%%` cannot be
    //  written here by accident.
    auto cmt = [&](const std::string& text) {
        if (text.empty()) t << "\n";
        else t << "%%  " << text << "\n";
    };
    const std::string rule = "%% " + std::string(74, '-');

    t << rule << "\n";
    cmt("GENERATED — DO NOT EDIT.");
    cmt("Topology for: " + m_.name + "   root: " + m_.root->fq);
    cmt("");
    cmt("Signal flow, as the model RUNS. Composites are flattened by now");
    cmt("(§15.2), so a subgraph is the path prefix one left behind, not a");
    cmt("runtime object. Nodes are listed in §8.5 schedule order.");
    if (!m_.groups.empty()) {
        cmt("A subgraph may instead be a §6.9.3 group: a labelled box with");
        cmt("no path, drawn for the reader and absent from everything else.");
    }
    cmt("");
    cmt("  A --> B    B's `output()` READS this input: an algebraic");
    cmt("             dependency, and what the §8.5 sort is over");
    cmt("  A -.-> B   it does not. The value reaches B through a state, so");
    cmt("             the edge orders nothing and a cycle through it is");
    cmt("             legal (§8.6)");
    cmt("  ( )        the root boundary: the host writes in, reads out");
    cmt("  thick      the node owns continuous state — the solver");
    cmt("             integrates it, so every edge leaving it is delayed");
    cmt("");
    {
        std::string base = "Base step " + dbl(m_.step) + " s";
        if (m_.step > 0.0) base += "  (" + dbl(1.0 / m_.step) + " Hz)";
        cmt(base + "; a node that samples slower says so.");
    }
    t << rule << "\n";
    t << "flowchart LR\n";

    // ---- the root boundary the host drives ---------------------------------
    for (std::size_t k = 0; k < m_.boundary_in.size(); ++k) {
        const BoundaryIn& b = m_.boundary_in[k];
        t << "    se_in_" << b.ident << "([" << lab("in." + b.ident + "<br/>" + topo.in_type[k])
          << "])\n";
    }

    // ---- nodes, nested by composite ----------------------------------------
    //  Mermaid lays each group out left to right in the order listed, which is
    //  the order the tick calls them.
    std::function<void(std::size_t, const std::string&)> draw =
        [&](std::size_t g, const std::string& ind) {
        for (std::size_t i : topo.clusters[g].leaves) {
            const Leaf& leaf = m_.leaves[i];
            const std::size_t dot = leaf.path.rfind('.');
            std::string name = leaf.path.empty()
                                   ? std::string("<root>")
                                   : (dot == std::string::npos ? leaf.path
                                                               : leaf.path.substr(dot + 1));
            std::string text = name + "<br/>" + leaf.node->fq;
            t << ind << id(leaf.ident) << "[" << lab(text) << "]\n";
        }
        for (std::size_t sub : topo.clusters[g].subs) {
            t << ind << "subgraph sg" << sub << "[" << lab(topo.clusters[sub].name) << "]\n";
            t << ind << "    direction LR\n";
            draw(sub, ind + "    ");
            t << ind << "end\n";
        }
    };
    draw(0, "    ");

    // ---- the root outputs the host may read --------------------------------
    for (std::size_t k = 0; k < topo.outs.size(); ++k) {
        const Topo::Output& o = topo.outs[k];
        if (o.leaf == std::string::npos) continue;
        t << "    se_out" << k << "([" << lab("out " + o.path + "<br/>" + o.type) << "])\n";
    }

    // ---- wires -------------------------------------------------------------
    //  The label names the port a reader would look up: the producer's for a
    //  leaf-to-leaf wire, the consumer's for a boundary or constant one. A
    //  constant's terminal is declared inline, the way Mermaid allows.
    t << "\n";
    for (const Topo::Edge& e : topo.edges) {
        std::string from, label;
        switch (e.from) {
        case Topo::Edge::From::Boundary:
            from = "se_in_" + e.boundary;
            label = e.dst_port;
            break;
        case Topo::Edge::From::Const:
            from = "se_k" + std::to_string(e.src) + "([" + lab(topo.consts[e.src]) + "])";
            label = e.dst_port;
            break;
        case Topo::Edge::From::Leaf:
            from = id(m_.leaves[e.src].ident);
            label = e.src_port;
            break;
        }
        t << "    " << from << (e.feedthrough ? " -->|" : " -.->|") << lab(label + " (" + e.type + ")")
          << "| " << id(m_.leaves[e.dst].ident) << "\n";
    }
    for (std::size_t k = 0; k < topo.outs.size(); ++k) {
        if (topo.outs[k].leaf == std::string::npos) continue;
        t << "    " << id(topo.outs[k].ident) << " --> se_out" << k << "\n";
    }

    // ---- where the solver's state lives ------------------------------------
    //  Stroke width only, no fill: the diagram renders on whatever background
    //  the viewer has, and a colour that reads in one theme is unreadable in
    //  the other.
    std::string ct;
    for (const Leaf& leaf : m_.leaves) {
        if (!leaf.has_continuous()) continue;
        if (!ct.empty()) ct += ",";
        ct += id(leaf.ident);
    }
    if (!ct.empty()) {
        t << "\n    classDef se_solver stroke-width:3px\n";
        t << "    class " << ct << " se_solver\n";
    }
    return t.str();
}

// — The topology as data —
//
//  What the interactive page (<stem>.topology.html) is drawn from. Everything
//  the Mermaid diagram has, plus the three things it never could: BOTH port
//  names on every wire, every node's settings, states and ports whether wired
//  or not, and where each node is defined. Values are strings formatted
//  exactly as the unit manifest formats them, so the page and `.units.txt`
//  cannot show a setting two ways.
//
//  One object or array entry per line: the JSON is checked against goldens,
//  and a diff should point at the node that changed, not at one long line.

namespace {

// A JSON string literal. `/` after `<` is escaped too, which JSON permits
// and which matters here: the text is embedded in a <script> element, and a
// unit or expression containing `</script>` would otherwise end it.
std::string jstr(const std::string& s) {
    std::string r = "\"";
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
        case '"': r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n"; break;
        case '\r': r += "\\r"; break;
        case '\t': r += "\\t"; break;
        case '/': r += (i && s[i - 1] == '<') ? "\\/" : "/"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                r += buf;
            } else {
                r += static_cast<char>(c);
            }
        }
    }
    return r + "\"";
}

std::string jbool(bool b) { return b ? "true" : "false"; }

// A string or `null` when empty.
std::string jopt(const std::string& s) { return s.empty() ? "null" : jstr(s); }

// `, "via": [...]`, or nothing for a wire that crosses no composite boundary.
std::string jvia(const std::vector<std::string>& via) {
    if (via.empty()) return std::string();
    std::string r = ", \"via\": [";
    for (std::size_t i = 0; i < via.size(); ++i) r += (i ? ", " : "") + jstr(via[i]);
    return r + "]";
}

// A port list: [{"name":..,"type":..}, ...]
std::string jports(const std::vector<Field>& ports) {
    std::string r = "[";
    for (std::size_t i = 0; i < ports.size(); ++i)
        r += (i ? ", " : "") + std::string("{\"name\": ") + jstr(ports[i].name) +
             ", \"type\": " + jstr(topo_type(ports[i].type)) + "}";
    return r + "]";
}

}  // namespace

std::string Emitter::topology_json(const Topo& topo) const {
    Out t;
    auto gid = [](std::size_t g) { return g == 0 ? std::string("null") : jstr("g" + std::to_string(g)); };

    std::vector<std::size_t> cluster_of(m_.leaves.size(), 0);
    for (std::size_t c = 0; c < topo.clusters.size(); ++c)
        for (std::size_t i : topo.clusters[c].leaves) cluster_of[i] = c;

    t << "{\n";
    t << "\"name\": " << jstr(m_.name) << ",\n";
    t << "\"root\": " << jstr(m_.root->fq) << ",\n";
    t << "\"step\": " << jstr(dbl(m_.step)) << ",\n";
    t << "\"solver\": " << jstr(m_.solver) << ",\n";

    // ---- clusters: composites and §6.9.3 groups --------------------------------
    //  A composite's `path` is its own; a group's is the composite that owns
    //  it, and a group alone has a source location and a `doc`.
    t << "\"clusters\": [";
    for (std::size_t g = 1; g < topo.clusters.size(); ++g) {
        const Topo::Cluster& c = topo.clusters[g];
        t << (g > 1 ? ",\n " : "\n ") << "{\"id\": " << gid(g) << ", \"kind\": "
          << (c.is_group ? "\"group\"" : "\"composite\"") << ", \"name\": " << jstr(c.name)
          << ", \"path\": " << jstr(c.path) << ", \"parent\": " << gid(c.parent);
        if (c.is_group)
            t << ", \"file\": " << jstr(line_path(c.file))
              << ", \"line\": " << static_cast<std::size_t>(c.line) << ", \"doc\": " << jopt(c.doc);
        // §6.15 — written only when set, so a model with no `@diagram` emits
        // the JSON it always did.
        if (c.collapsed) t << ", \"collapsed\": true";
        // A composite's own ports, which the page draws when it is collapsed.
        if (!c.is_group) {
            auto p = m_.composite_ports.find(c.path);
            if (p != m_.composite_ports.end())
                t << ", \"ins\": " << jports(p->second.ins) << ", \"outs\": " << jports(p->second.outs);
        }
        t << "}";
    }
    t << (topo.clusters.size() > 1 ? "\n],\n" : "],\n");

    // ---- nodes: leaves, then the terminals ------------------------------------
    //  Ids are the leaves' generated identifiers, which are unique (I5) and
    //  never contain a dot, so the terminals' dotted ids cannot collide.
    std::vector<std::string> rows;
    for (std::size_t i = 0; i < m_.leaves.size(); ++i) {
        const Leaf& leaf = m_.leaves[i];
        const std::size_t dot = leaf.path.rfind('.');
        const std::string name = leaf.path.empty()
                                     ? std::string("<root>")
                                     : (dot == std::string::npos ? leaf.path : leaf.path.substr(dot + 1));
        std::string r = "{\"id\": " + jstr(leaf.ident) + ", \"kind\": \"leaf\", \"name\": " + jstr(name) +
                        ", \"path\": " + jstr(leaf.path) + ", \"type\": " + jstr(leaf.node->fq) +
                        ", \"cluster\": " + gid(cluster_of[i]) + ", \"order\": " +
                        std::to_string(topo.rank[i]) + ", \"solver\": " + jbool(leaf.has_continuous());
        if (leaf.node->file && leaf.node->def)
            r += ", \"file\": " + jstr(line_path(leaf.node->file->path)) +
                 ", \"line\": " + std::to_string(leaf.node->def->loc.line);
        r += ", \"ins\": " + jports(leaf.ports_in) + ", \"outs\": " + jports(leaf.ports_out);
        r += ", \"states\": [";
        for (std::size_t k = 0; k < leaf.states.size(); ++k) {
            const StateSlot& s = leaf.states[k];
            r += (k ? ", " : "") + std::string("{\"name\": ") + jstr(s.name) +
                 ", \"kind\": " + jstr(ast::state_kind_name(s.kind)) +
                 ", \"unit\": " + jstr(s.unit.str()) +
                 ", \"initial\": " + jstr(shown(s.initial, s.scalar, s.enm)) +
                 ", \"extent\": " + jopt(s.extent.present ? s.extent.text : std::string()) +
                 ", \"slot\": " +
                 (is_cont(s) && !m_.dynamic_states ? std::to_string(s.slot) : std::string("null")) +
                 "}";
        }
        rows.push_back(r + "]}");
    }
    for (std::size_t k = 0; k < m_.boundary_in.size(); ++k)
        rows.push_back("{\"id\": " + jstr("in." + m_.boundary_in[k].ident) +
                       ", \"kind\": \"in\", \"name\": " + jstr(m_.boundary_in[k].path) +
                       ", \"type\": " + jstr(topo.in_type[k]) + "}");
    for (std::size_t k = 0; k < topo.outs.size(); ++k) {
        if (topo.outs[k].leaf == std::string::npos) continue;
        rows.push_back("{\"id\": " + jstr("out." + std::to_string(k)) +
                       ", \"kind\": \"out\", \"name\": " + jstr("out." + topo.outs[k].path) +
                       ", \"type\": " + jstr(topo.outs[k].type) + "}");
    }
    // A literal is a terminal per wire; a setting, one node however many
    // wires it drives, so its `consts` entries are not written.
    std::vector<bool> is_literal(topo.consts.size(), true);
    for (const Topo::Edge& e : topo.edges)
        if (e.from == Topo::Edge::From::Const && e.setting != std::string::npos) is_literal[e.src] = false;
    for (std::size_t k = 0; k < topo.consts.size(); ++k)
        if (is_literal[k])
            rows.push_back("{\"id\": " + jstr("const." + std::to_string(k)) +
                           ", \"kind\": \"const\", \"name\": " + jstr(topo.consts[k]) + "}");
    for (const Topo::Setting& st : topo.settings)
        rows.push_back("{\"id\": " + jstr("setting." + st.path) + ", \"kind\": \"setting\", \"name\": " +
                       jstr(st.name) + ", \"owner\": " + jstr(st.owner) + ", \"path\": " +
                       jstr(st.path) + ", \"cluster\": " + gid(st.cluster) + ", \"type\": " +
                       jstr(st.type) + "}");
    t << "\"nodes\": [";
    for (std::size_t k = 0; k < rows.size(); ++k) t << (k ? ",\n " : "\n ") << rows[k];
    t << (rows.empty() ? "],\n" : "\n],\n");

    // ---- wires -----------------------------------------------------------------
    //  A terminal's single port is called `out` (a source) or `in` (a sink),
    //  so every edge names a node AND a port at both ends.
    rows.clear();
    for (const Topo::Edge& e : topo.edges) {
        std::string from, port;
        switch (e.from) {
        case Topo::Edge::From::Leaf: from = m_.leaves[e.src].ident; port = e.src_port; break;
        case Topo::Edge::From::Boundary: from = "in." + e.boundary; port = "out"; break;
        case Topo::Edge::From::Const:
            from = e.setting != std::string::npos ? "setting." + topo.settings[e.setting].path
                                     : "const." + std::to_string(e.src);
            port = "out";
            break;
        }
        rows.push_back("{\"from\": " + jstr(from) + ", \"fromPort\": " + jstr(port) +
                       ", \"to\": " + jstr(m_.leaves[e.dst].ident) + ", \"toPort\": " +
                       jstr(e.dst_port) + ", \"type\": " + jstr(e.type) +
                       ", \"feedthrough\": " + jbool(e.feedthrough) + jvia(e.via) + "}");
    }
    for (std::size_t k = 0; k < topo.outs.size(); ++k) {
        const Topo::Output& o = topo.outs[k];
        if (o.leaf == std::string::npos) continue;
        rows.push_back("{\"from\": " + jstr(o.ident) + ", \"fromPort\": " + jstr(o.port) +
                       ", \"to\": " + jstr("out." + std::to_string(k)) +
                       ", \"toPort\": \"in\", \"type\": " + jstr(o.type) +
                       ", \"feedthrough\": true" + jvia(o.via) + "}");
    }
    t << "\"edges\": [";
    for (std::size_t k = 0; k < rows.size(); ++k) t << (k ? ",\n " : "\n ") << rows[k];
    t << (rows.empty() ? "],\n" : "\n],\n");

    // ---- settings ----------------------------------------------------------------
    //  Every one, composites' included: `owner` is the path of the node or
    //  composite holding it, which is how the page finds a node's settings.
    t << "\"settings\": [";
    for (std::size_t k = 0; k < m_.settings.size(); ++k) {
        const SettingSlot& s = m_.settings[k];
        t << (k ? ",\n " : "\n ") << "{\"path\": " << jstr(s.path) << ", \"owner\": " << jstr(s.owner)
          << ", \"name\": " << jstr(s.name) << ", \"unit\": " << jstr(s.unit.str())
          << ", \"value\": " << jstr(shown(s.value, s.scalar, s.enm))
          << ", \"from\": " << (s.constant ? std::string("null") : jstr(derived_from(s.expr)))
          << ", \"structural\": " << jbool(s.structural) << "}";
    }
    t << (m_.settings.empty() ? "]\n" : "\n]\n");
    t << "}\n";
    return t.str();
}

// ─── The unit manifest (§15.6) ───────────────────────────────────────────────

std::string Emitter::manifest() {
    Out t;
    t << "# " << std::string(76, '-') << "\n";
    t << "#  GENERATED — DO NOT EDIT.\n";
    t << "#  Unit manifest for: " << hdr_ << "\n";
    t << "#  sim: " << m_.name << "   root: " << m_.root->fq << "   base step: "
      << dbl(m_.step) << " s\n#\n";
    t << "#  Units are checked by the DSL compiler at every declarative site and then\n";
    t << "#  ERASED — the generated C++ is plain `double` throughout. This file is where\n";
    t << "#  the erased information goes. It is a real generator output, not\n";
    t << "#  documentation: it is the contract for anyone writing an external solver,\n";
    t << "#  feeding the root boundary, or overriding a setting from outside the model.\n";
    t << "#\n#  Every value named here is IN THE UNIT SHOWN. `-` means dimensionless.\n";
    t << "# " << std::string(76, '-') << "\n\n";

    t << "[state.continuous]        # slot | model path | unit  -- the solver's ABI\n";
    if (m_.dynamic_states)
        t << "#  This model has array-shaped states (§6.4a), so no slot number is\n"
             "#  known here: an extent does not settle until configuration. The extent\n"
             "#  is printed AS WRITTEN, and the concrete rows come from\n"
             "#  Sim::Impl::state_map() once configure() has run (§15.6).\n";
    for (const Leaf& leaf : m_.leaves)
        for (const StateSlot& s : leaf.states) {
            if (!is_cont(s)) continue;
            const std::string path = leaf.path.empty() ? s.name : leaf.path + "." + s.name;
            if (!m_.dynamic_states) {
                t << col("x[" + std::to_string(s.slot) + "]", 10) << col(path, 24)
                  << s.unit.str() << "\n";
                t << col("xd[" + std::to_string(s.slot) + "]", 10) << col(path + "'", 24)
                  << s.der_unit.str() << "\n";
                continue;
            }
            const std::string ext = s.extent.present ? " " + s.extent.text : "";
            t << col("x[?]", 10) << col(path + ext, 24)
              << col(s.unit.str(), 12) << "dynamic\n";
            t << col("xd[?]", 10) << col(path + "'" + ext, 24)
              << col(s.der_unit.str(), 12) << "dynamic\n";
        }
    if (m_.n_cont_slots == 0 && m_.n_states == 0) t << "          (none)\n";

    // §15.6 — `sec` cannot write byte offsets: padding is the C++ compiler's
    // business, and a guessed table would be worse than none. So the manifest
    // names, units and types the slots, and the generated header builds the
    // offsets with offsetof(). Both come from the same row list, so they cannot
    // disagree.
    //
    // A slot row carries the C++ type, which `sizeof` needs; the manifest names
    // an enum the way the DSL does, so its `[types]` row is findable (§15.6).
    auto shown_type = [&](const std::string& cpp) {
        for (const EnumInfo* e : m_.program->enum_order)
            if (e->cpp_name == cpp) return e->fq;
        // §15.6 — a buffer by its DSL spelling, `buffer(1500)`.
        static const std::string kBuf = "se_rt::buffer<";
        if (cpp.compare(0, kBuf.size(), kBuf) == 0 && cpp.back() == '>')
            return "buffer(" + cpp.substr(kBuf.size(), cpp.size() - kBuf.size() - 1) + ")";
        return cpp;
    };
    t << "\n[state.discrete]          # Sim::Impl::dis; offsets from Sim::Impl::discrete_map()\n";
    const std::vector<SlotRow> dis = discrete_rows();
    for (const SlotRow& r : dis)
        t << col("", 10) << col(r[0], 24) << col(r[3], 12) << shown_type(r[2]) << "\n";
    if (dis.empty()) t << "          (none)\n";

    // §6.4c — present only in a model that has the block, so a model with no
    // finite state keeps the manifest it always had.
    const std::vector<SlotRow> fin = finite_rows();
    if (!fin.empty()) {
        t << "\n[state.finite]            # Sim::Impl::fin; offsets from Sim::Impl::finite_map()\n";
        for (const SlotRow& r : fin)
            t << col("", 10) << col(r[0], 24) << col(r[3], 12) << shown_type(r[2]) << "\n";
    }

    // §15.5a — the fourth block. An array-shaped discrete state cannot live
    // in `dis` without destroying its offsetof property, so it gets a flat
    // `double` block of its own and the same address-size-table treatment as x.
    if (m_.n_disc_arr_slots) {
        t << "\n[state.discrete.array]    # Sim::Impl::dis_arr; rows from "
             "Sim::Impl::discrete_array_map()\n";
        for (const SlotRow& r : array_state_rows(false))
            t << col("dis_arr[?]", 12) << col(r[0] + " " + r[1], 22) << col(r[3], 12)
              << "dynamic\n";
    }

    t << "\n[signals]                 # Sim::Impl::sig; offsets from Sim::Impl::signal_map()\n";
    for (const SlotRow& r : signal_rows())
        t << col("", 10) << col(r[0], 24) << col(r[3], 12) << shown_type(r[2]) << "\n";

    t << "\n[boundary.in]             # root inputs the host must drive, through Sim::inputs()\n";
    for (const BoundaryIn& b : m_.boundary_in)
        t << col("", 10) << col(b.path, 24)
          << (b.type.is_record ? b.type.record->fq
                               : b.type.enm ? b.type.enm->fq : b.type.unit.str())
          << "\n";
    if (m_.boundary_in.empty()) t << "          (none)\n";

    t << "\n[boundary.out]            # root outputs the host may read\n";
    for (const BoundaryOut& b : m_.boundary_out)
        t << col("", 10) << col(b.path, 24) << (b.type.enm ? b.type.enm->fq : b.type.unit.str())
          << "\n";
    if (m_.boundary_out.empty()) t << "          (none)\n";

    // §15.6 — the configuration schema (§14.1): every path a settings source may
    // bind, the unit it is checked against, and the value elaboration produced.
    // A derived setting also shows what it is derived FROM, path-addressed, so
    // the file says which knobs move which values without the reader having to
    // open the model.
    t << "\n[settings]                # configurable; value is the elaborated default\n";
    for (const SettingSlot& s : m_.settings) {
        t << col("", 10) << col(s.path, 24) << col(s.unit.str(), 12) << "= "
          << shown(s.value, s.scalar, s.enm);
        if (!s.constant) t << "   <- " << derived_from(s.expr);
        t << "\n";
    }

    // Field units are shown AS DECLARED, so a parametric record reports `U`
    // rather than any one binding. The substituted units are the per-field rows
    // in [signals] and [boundary.*], which is where a host reads them; this
    // section says what the type is, not how one reference bound it (§5.2).
    t << "\n[types]\n";
    for (const RecordInfo* r : m_.program->record_order) {
        if (!r->unit_params.empty()) {
            std::string ps;
            for (std::size_t i = 0; i < r->unit_params.size(); ++i)
                ps += (i ? ", " : "") + r->unit_params[i];
            t << col(r->fq, 34) << "units { " << ps << " }\n";
        }
        for (const Field& f : r->fields)
            t << col(r->fq + "." + f.name, 34)
              << (f.type.is_record ? f.type.record->fq
                                   : f.type.enm ? f.type.enm->fq : f.type.unit.str())
              << "\n";
    }
    // §15.6 — an enum's row is its decoding table, every value written out.
    for (const EnumInfo* e : m_.program->enum_order) {
        std::string vs;
        for (std::size_t i = 0; i < e->values.size(); ++i)
            vs += (i ? ", " : "") + e->values[i].name + " = " + std::to_string(e->values[i].value);
        t << col(e->fq, 34) << "finite { " << vs << " }\n";
    }

    if (m_.record.present) {
        t << "\n[recorded]                # columns of " << m_.record.file << "\n";
        t << col("", 10) << col("time", 24) << "s\n";
        for (const RecordedSignal& s : m_.record.signals)
            t << col("", 10) << col(s.path, 24)
              << (!s.label.empty() ? s.label : s.enm ? s.enm->fq : s.unit.str()) << "\n";
    }
    return t.str();
}

// ─── The reference build (§12.4) ─────────────────────────────────────────────
//
//  Two scripts, one build: `build.ps1` for Windows and `build.sh` for a POSIX
//  shell. They are not translations of one text — PowerShell and sh disagree
//  about quoting, about how an exit code is read and about what a comment
//  looks like — but they take the same modes and produce the same layout:
//
//      build/<config>/bin/       the exe (and, later, the dll)
//      build/<config>/lib/       the static library
//      build/<config>/include/   the public header that goes with it
//
//  The generated sources are the compiler's output and the binaries are the
//  C++ compiler's; keeping them apart means the emit directory can be
//  re-emitted, diffed or deleted without binaries mixed in among the files
//  `sec` actually wrote.

std::string Emitter::build_script(Script kind, const std::string& exe_main, bool unity) {
    const char* platform = kind == Script::Sh ? "linux" : "windows";
    const bool windows = kind != Script::Sh;
    const std::string name = stem_;   // the product is named for its sources

    // The model's own build primitives (§12.1), one argument per element and
    // UNQUOTED: each shell has its own idea of what needs quoting, and an
    // `include_dir` with a space in it is the case that separates them, so the
    // quoting is applied where the text is laid out.
    std::vector<std::string> extra, links;
    for (const ast::BuildStmt& b : m_.program->build) {
        if (!b.when.empty() && b.when != platform) continue;
        if (b.primitive == "include_dir")
            extra.push_back((windows ? "/I" : "-I") + b.argument);
        else if (b.primitive == "define")
            extra.push_back((windows ? "/D" : "-D") + b.argument);
        else if (b.primitive == "cflag")
            extra.push_back(b.argument);
        else if (b.primitive == "link")
            links.push_back(windows ? b.argument + ".lib" : "-l" + b.argument);
    }
    // A white-box test program #includes the implementation, so it is the
    // whole program on its own.
    const std::string exe_sources_ps =
        unity ? "'" + exe_main + "'" : "'" + exe_main + "', \"$name.cpp\"";
    const std::string exe_sources_sh = unity ? exe_main : exe_main + " \"$name.cpp\"";

    Out t;
    if (kind == Script::Ps1) {
        auto ps_arg = [](const std::string& a) {
            std::string q = "'";
            for (char c : a) {
                q += c;
                if (c == '\'') q += '\'';
            }
            return q + "'";
        };
        t << "# " << std::string(74, '-') << "\n";
        t << "#  build.ps1 — GENERATED by sec for " << name << ". The reference build (§12.4).\n";
        t << "#\n";
        t << "#      .\\build.ps1 [run|exe|lib|test|clean|all] [-Config release|debug]\n";
        t << "#                  [-Sanitize | -NoSanitize] [-StaticCrt] [exe arguments]\n";
        t << "#\n";
        t << "#      run    build the exe, then run it here" << (unity ? "" : ", as " + m_.name + ".sim describes") << "\n";
        t << "#      exe    build\\<config>\\bin\\" << name << ".exe\n";
        t << "#      lib    build\\<config>\\lib\\" << name << ".lib, with include\\" << hdr_ << "\n";
        t << "#      test   build the exe in debug with AddressSanitizer, run it, and\n";
        t << "#             fail unless it exits 0\n";
        t << "#      clean  delete build\\\n";
        t << "#      all    exe and lib\n";
        t << "#\n";
        t << "#  release is the default for all but test: optimised, with debug\n";
        t << "#  information. -Sanitize adds AddressSanitizer to any mode; -NoSanitize\n";
        t << "#  takes it out of test. -StaticCrt links the C runtime statically (/MT),\n";
        t << "#  so nothing needs the VC++ redistributable.\n";
        t << "#\n";
        t << "#  Every build is a full one; nothing is tracked between runs. If `cl` is\n";
        t << "#  not on PATH, the script finds Visual Studio and loads its environment.\n";
        t << "# " << std::string(74, '-') << "\n";
        // Only the mode is positional, so whatever follows it reaches $ExeArgs
        // rather than being taken for -Config.
        t << "[CmdletBinding(PositionalBinding = $false)]\n";
        t << "param(\n";
        t << "    [Parameter(Position = 0)]\n";
        t << "    [ValidateSet('run', 'exe', 'lib', 'test', 'clean', 'all')]\n";
        t << "    [string]$Mode = 'run',\n";
        t << "    [ValidateSet('release', 'debug')]\n";
        t << "    [string]$Config = '',\n";
        t << "    [switch]$Sanitize,\n";
        t << "    [switch]$NoSanitize,\n";
        t << "    [switch]$StaticCrt,\n";
        t << "    # Everything else goes to the exe, in run and test (§12.4).\n";
        t << "    [Parameter(ValueFromRemainingArguments = $true)]\n";
        t << "    [string[]]$ExeArgs\n";
        t << ")\n\n";
        t << "$name = " << ps_arg(name) << "\n\n";

        t << "if ($Mode -eq 'clean') {\n";
        t << "    Remove-Item -Recurse -Force (Join-Path $PSScriptRoot 'build') "
             "-ErrorAction SilentlyContinue\n";
        t << "    Write-Host 'build: removed build\\'\n";
        t << "    exit 0\n";
        t << "}\n";
        t << "if (-not $Config) { $Config = if ($Mode -eq 'test') { 'debug' } else { 'release' } }\n\n";

        t << "# -- the compiler " << std::string(58, '-') << "\n";
        t << "if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {\n";
        t << "    $vswhere = Join-Path ${env:ProgramFiles(x86)} "
             "'Microsoft Visual Studio\\Installer\\vswhere.exe'\n";
        t << "    if (Test-Path $vswhere) {\n";
        t << "        $vs = & $vswhere -latest -products * -property installationPath\n";
        t << "        $vcvars = Join-Path \"$vs\" 'VC\\Auxiliary\\Build\\vcvars64.bat'\n";
        t << "        if (Test-Path $vcvars) {\n";
        t << "            & cmd /c \"call `\"$vcvars`\" >nul 2>&1 && set\" | ForEach-Object {\n";
        t << "                if ($_ -match '^([^=]+)=(.*)$') { Set-Item \"env:$($matches[1])\" "
             "$matches[2] }\n";
        t << "            }\n";
        t << "        }\n";
        t << "    }\n";
        t << "}\n";
        t << "if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {\n";
        t << "    Write-Host 'build: cl not found; install Visual Studio with C++, or run "
             "from a Developer PowerShell'\n";
        t << "    exit 2\n";
        t << "}\n\n";

        // `test` is the one mode whose whole job is finding bugs, and the one
        // that runs its own exe inside the environment it just loaded, so it
        // is the one that sanitizes unasked. It has to stay usable without the
        // optional ASan component, so there it checks first and says so.
        t << "# -- the sanitizer " << std::string(57, '-') << "\n";
        t << "$asan = $Sanitize -or ($Mode -eq 'test' -and -not $NoSanitize)\n";
        t << "if ($asan -and -not $Sanitize) {\n";
        t << "    $runtime = 'clang_rt.asan_dynamic-x86_64.dll'\n";
        t << "    $found = $env:PATH -split ';' | Where-Object { $_ -and "
             "(Test-Path (Join-Path $_ $runtime)) }\n";
        t << "    if (-not $found) {\n";
        t << "        $asan = $false\n";
        t << "        Write-Host 'test: AddressSanitizer is not installed; testing without it'\n";
        t << "    }\n";
        t << "}\n\n";

        t << "# -- the flags " << std::string(61, '-') << "\n";
        t << "$crt = if ($StaticCrt) { '/MT' } else { '/MD' }\n";
        t << "if ($Config -eq 'debug') { $crt += 'd' }\n";
        t << "$flags = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/permissive-', '/utf-8', "
             "'/Z7', $crt,\n";
        t << "           '/D_CRT_SECURE_NO_WARNINGS')\n";
        t << "if ($Config -eq 'debug') {\n";
        t << "    $flags += '/Od', '/D_DEBUG'\n";
        t << "    if (-not $asan) { $flags += '/RTC1' }   # /RTC and AddressSanitizer "
             "do not combine\n";
        t << "} else {\n";
        t << "    $flags += '/O2', '/DNDEBUG'\n";
        t << "}\n";
        t << "if ($asan) { $flags += '/fsanitize=address' }\n";
        if (!extra.empty()) {
            t << "$flags += @(";
            for (std::size_t i = 0; i < extra.size(); ++i)
                t << (i ? ", " : "") << ps_arg(extra[i]);
            t << ")   # from the model's build declarations\n";
        }
        t << "$links = @(";
        for (std::size_t i = 0; i < links.size(); ++i) t << (i ? ", " : "") << ps_arg(links[i]);
        t << ")\n";
        t << "$out = \"build\\$Config\"\n\n";

        t << "# -- the products " << std::string(58, '-') << "\n";
        t << "function Build-Exe {\n";
        t << "    New-Item -ItemType Directory -Force \"$out\\obj\", \"$out\\bin\" | Out-Null\n";
        t << "    & cl @flags \"/Fo$out\\obj\\\" \"/Fe:$out\\bin\\$name.exe\" " << exe_sources_ps
          << " `\n";
        t << "        /link /DEBUG /INCREMENTAL:NO @links | Out-Host\n";
        t << "    if ($LASTEXITCODE -ne 0) { return $false }\n";
        t << "    Write-Host \"build: $out\\bin\\$name.exe\"\n";
        t << "    return $true\n";
        t << "}\n\n";
        t << "function Build-Lib {\n";
        t << "    New-Item -ItemType Directory -Force \"$out\\obj\", \"$out\\lib\", "
             "\"$out\\include\" | Out-Null\n";
        t << "    & cl @flags /c \"/Fo$out\\obj\\\" \"$name.cpp\" | Out-Host\n";
        t << "    if ($LASTEXITCODE -ne 0) { return $false }\n";
        t << "    & lib /nologo \"/OUT:$out\\lib\\$name.lib\" \"$out\\obj\\$name.obj\" | Out-Host\n";
        t << "    if ($LASTEXITCODE -ne 0) { return $false }\n";
        t << "    Copy-Item \"$name.hpp\" \"$out\\include\\\" -Force\n";
        t << "    Write-Host \"build: $out\\lib\\$name.lib\"\n";
        t << "    Write-Host \"build: $out\\include\\$name.hpp\"\n";
        t << "    Write-Host \"build: link it into a program built with the same C runtime "
             "($crt)\"\n";
        if (!links.empty()) {
            t << "    Write-Host 'build: a program using it also links";
            for (const std::string& l : links) t << " " << l;
            t << "'\n";
        }
        t << "    return $true\n";
        t << "}\n\n";

        t << "Push-Location $PSScriptRoot\n";
        t << "try {\n";
        t << "    $code = 0\n";
        t << "    if ($Mode -eq 'lib') {\n";
        t << "        if (-not (Build-Lib)) { $code = 1 }\n";
        t << "    } elseif (-not (Build-Exe)) {\n";
        t << "        $code = 1\n";
        t << "    } elseif ($Mode -eq 'all') {\n";
        t << "        if (-not (Build-Lib)) { $code = 1 }\n";
        t << "    } elseif ($Mode -eq 'run') {\n";
        t << "        & \".\\$out\\bin\\$name.exe\" @ExeArgs\n";
        t << "        $code = $LASTEXITCODE\n";
        t << "    } elseif ($Mode -eq 'test') {\n";
        t << "        & \".\\$out\\bin\\$name.exe\" @ExeArgs\n";
        t << "        $code = $LASTEXITCODE\n";
        t << "        $how = if ($asan) { ' (AddressSanitizer)' } else { '' }\n";
        t << "        if ($code -eq 0) { Write-Host \"test: passed$how\" } "
             "else { Write-Host \"test: FAILED$how, exit $code\" }\n";
        t << "    }\n";
        t << "} finally {\n";
        t << "    Pop-Location\n";
        t << "}\n";
        t << "exit $code\n";
        return t.str();
    }

    // ── build.sh ─────────────────────────────────────────────────────────────
    auto sh_arg = [](const std::string& a) {
        std::string q = "'";
        for (char c : a) {
            if (c == '\'') q += "'\\''";
            else q += c;
        }
        return q + "'";
    };
    auto bare = [&](const std::string& a) {
        // Quote only what needs it, so the script reads like a command line
        // someone typed.
        return a.find_first_of(" \t'\"$`\\*?") == std::string::npos ? a : sh_arg(a);
    };
    std::string model_args, model_libs;
    for (const std::string& a : extra) model_args += " " + bare(a);
    for (const std::string& l : links) model_libs += " " + bare(l);

    t << "#!/bin/sh\n";
    t << "# " << std::string(74, '-') << "\n";
    t << "#  build.sh — GENERATED by sec for " << name << ". The reference build (§12.4).\n";
    t << "#\n";
    t << "#      ./build.sh [run|exe|lib|test|clean|all] [--config release|debug]\n";
    t << "#                 [--sanitize | --no-sanitize] [--static-crt] [exe arguments]\n";
    t << "#\n";
    t << "#  The same modes as build.ps1, into build/<config>/{bin,lib,include};\n";
    t << "#  test sanitizes (address, undefined) unless --no-sanitize, or unless the\n";
    t << "#  compiler cannot. CXX picks the compiler; the default is c++. In run and\n";
    t << "#  test, the first argument this script does not know, and every one after\n";
    t << "#  it, goes to the exe.\n";
    t << "# " << std::string(74, '-') << "\n";
    t << "set -e\n";
    t << "cd \"$(dirname \"$0\")\"\n\n";
    t << "name=" << bare(name) << "\n";
    t << "mode=run\nconfig=\nsanitize=\nstatic_crt=0\n";
    t << "while [ $# -gt 0 ]; do\n";
    t << "    case \"$1\" in\n";
    t << "        run|exe|lib|test|clean|all) mode=$1 ;;\n";
    t << "        --config) shift; config=$1 ;;\n";
    t << "        --sanitize) sanitize=yes ;;\n";
    t << "        --no-sanitize) sanitize=no ;;\n";
    t << "        --static-crt) static_crt=1 ;;\n";
    t << "        *) if [ \"$mode\" = run ] || [ \"$mode\" = test ]; then break; fi\n";
    t << "           echo \"build: unknown argument '$1'\" >&2; exit 2 ;;\n";
    t << "    esac\n";
    t << "    shift\n";
    t << "done\n\n";
    t << "if [ \"$mode\" = clean ]; then rm -rf build; echo \"build: removed build/\"; exit 0; fi\n";
    t << "if [ -z \"$config\" ]; then\n";
    t << "    if [ \"$mode\" = test ]; then config=debug; else config=release; fi\n";
    t << "fi\n";
    t << "case \"$config\" in release|debug) ;; *) echo \"build: --config is release or "
         "debug\" >&2; exit 2 ;; esac\n\n";
    t << "CXX=${CXX:-c++}\n\n";
    t << "asan=0\n";
    t << "if [ \"$sanitize\" = yes ]; then\n";
    t << "    asan=1\n";
    t << "elif [ \"$sanitize\" != no ] && [ \"$mode\" = test ]; then\n";
    t << "    if echo 'int main() { return 0; }' | "
         "$CXX -x c++ -fsanitize=address,undefined -o /dev/null - 2>/dev/null; then\n";
    t << "        asan=1\n";
    t << "    else\n";
    t << "        echo \"test: the sanitizers are not available; testing without them\"\n";
    t << "    fi\n";
    t << "fi\n\n";
    t << "flags=\"-std=c++20 -Wall -Wextra -g\"\n";
    t << "if [ \"$config\" = debug ]; then flags=\"$flags -O0\"; "
         "else flags=\"$flags -O2 -DNDEBUG\"; fi\n";
    t << "if [ $asan = 1 ]; then flags=\"$flags -fsanitize=address,undefined\"; fi\n";
    t << "ldflags=\n";
    t << "if [ $static_crt = 1 ]; then ldflags=\"-static-libstdc++ -static-libgcc\"; fi\n";
    t << "out=build/$config\n\n";
    t << "build_exe() {\n";
    t << "    mkdir -p \"$out/bin\"\n";
    t << "    $CXX $flags" << model_args << " -o \"$out/bin/$name\" " << exe_sources_sh
      << " $ldflags" << model_libs << "\n";
    t << "    echo \"build: $out/bin/$name\"\n";
    t << "}\n\n";
    t << "build_lib() {\n";
    t << "    mkdir -p \"$out/obj\" \"$out/lib\" \"$out/include\"\n";
    t << "    $CXX $flags" << model_args << " -c -o \"$out/obj/$name.o\" \"$name.cpp\"\n";
    t << "    ar rcs \"$out/lib/lib$name.a\" \"$out/obj/$name.o\"\n";
    t << "    cp \"$name.hpp\" \"$out/include/\"\n";
    t << "    echo \"build: $out/lib/lib$name.a\"\n";
    t << "    echo \"build: $out/include/$name.hpp\"\n";
    t << "}\n\n";
    t << "case \"$mode\" in\n";
    t << "    exe) build_exe ;;\n";
    t << "    lib) build_lib ;;\n";
    t << "    all) build_exe; build_lib ;;\n";
    t << "    run) build_exe; exec \"./$out/bin/$name\" \"$@\" ;;\n";
    t << "    test)\n";
    t << "        build_exe\n";
    t << "        how=; if [ $asan = 1 ]; then how=\" (AddressSanitizer)\"; fi\n";
    t << "        if \"./$out/bin/$name\" \"$@\"; then echo \"test: passed$how\"; "
         "else code=$?; echo \"test: FAILED$how, exit $code\"; exit 1; fi ;;\n";
    t << "esac\n";
    return t.str();
}

bool write_file(Diagnostics& diag, const std::string& path, const std::string& text,
                bool quiet) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "sec: cannot write `%s`\n", path.c_str());
        (void)diag;
        return false;
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    if (!quiet) std::printf("  wrote %s\n", path.c_str());
    return true;
}

}  // namespace

bool emit(Diagnostics& diag, const Model& m, const EmitOptions& opt) {
    // Named for the sim unless told otherwise, so two models' products can sit
    // side by side: Drivetrain.hpp, Drivetrain.lib, Drivetrain.exe.
    const std::string stem = !opt.stem.empty() ? opt.stem : !m.name.empty() ? m.name : "Sim";
    const std::string dir = opt.out_dir.empty() ? "." : opt.out_dir;

    // Creating the output directory is the compiler's job, not the caller's:
    // `sec --emit -o build/washout` should write there, not fail because
    // nothing has made `build/washout` yet. create_directories is a no-op on a
    // directory that already exists, and reports through `ec` rather than
    // throwing, so an unwritable path becomes a diagnostic and not a crash.
    {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec && !std::filesystem::is_directory(dir)) {
            std::fprintf(stderr, "sec: cannot create output directory `%s`: %s\n",
                         dir.c_str(), ec.message().c_str());
            return false;
        }
    }

    auto path = [&](const std::string& name) {
        return dir == "." ? name : dir + "/" + name;
    };

    Emitter e(m, stem);
    bool ok = true;
    // The implementation first: it is the pass that settles the settings'
    // C++ owners, and the manifest and header only read the model.
    ok = write_file(diag, path(stem + ".cpp"), e.impl_cpp(opt.separate_runtime), opt.quiet) && ok;
    ok = write_file(diag, path(stem + ".hpp"), e.public_hpp(), opt.quiet) && ok;
    const bool unity = !opt.unity_main.empty();
    const bool has_main = unity || opt.write_main;
    if (opt.write_main && !unity)
        ok = write_file(diag, path("main.cpp"), e.main_cpp(), opt.quiet) && ok;
    ok = write_file(diag, path(stem + ".units.txt"), e.manifest(), opt.quiet) && ok;
    if (opt.topology_html || opt.topology_mermaid) {
        const Topo topo = e.topology();
        if (opt.topology_html) {
            // Source paths in the JSON are as opened, so usually relative to
            // where `sec` ran; the page needs that directory to make links.
            std::error_code ec;
            const std::string base = std::filesystem::current_path(ec).generic_string();
            ok = write_file(diag, path(stem + ".topology.html"),
                            topology_page(m.name, e.topology_json(topo), jstr(base)),
                            opt.quiet) && ok;
        }
        if (opt.topology_mermaid)
            ok = write_file(diag, path(stem + ".topology.md"),
                            "```mermaid\n" + e.topology_mermaid(topo) + "```\n", opt.quiet) && ok;
    }
    if (opt.separate_runtime)
        ok = write_file(diag, path("se_runtime.hpp"), se_runtime_hpp(), opt.quiet) && ok;
    if (opt.write_build && has_main) {
        const std::string exe_main = unity ? opt.unity_main : "main.cpp";
        ok = write_file(diag, path("build.ps1"), e.build_script(Script::Ps1, exe_main, unity),
                        opt.quiet) && ok;
        ok = write_file(diag, path("build.sh"), e.build_script(Script::Sh, exe_main, unity),
                        opt.quiet) && ok;
        // Its own header says `./build.sh`, which needs the x bit, and fopen
        // leaves a file at 0666 & ~umask. Executable by whoever may read it,
        // the way `chmod +x` would set it. A failure is not an emit failure:
        // `sh build.sh` still works. On Windows there is no bit to set.
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::perms p = fs::status(path("build.sh"), ec).permissions();
        if (!ec) {
            fs::perms x = fs::perms::none;
            if ((p & fs::perms::owner_read) != fs::perms::none) x |= fs::perms::owner_exec;
            if ((p & fs::perms::group_read) != fs::perms::none) x |= fs::perms::group_exec;
            if ((p & fs::perms::others_read) != fs::perms::none) x |= fs::perms::others_exec;
            fs::permissions(path("build.sh"), x, fs::perm_options::add, ec);
        }
    }
    return ok;
}

}  // namespace se
