// ─────────────────────────────────────────────────────────────────────────────
//  Stage 6 — emission.  SPECIFICATION.md §11, §12.4, §15.
// ─────────────────────────────────────────────────────────────────────────────
#include "emit.hpp"

#include <array>
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

std::string literal(double v, const std::string& scalar) {
    if (scalar == "bool") return v != 0.0 ? "true" : "false";
    if (scalar == "int") {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(std::llround(v)));
        return buf;
    }
    if (scalar == "float") return dbl(v) + "f";
    return dbl(v);
}

std::string cpp_type(const Type& t) {
    return t.is_record ? t.record->cpp_name : t.scalar;
}

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
    std::string topology() const;
    // `exe_main` is the file holding main(). With `unity` it #includes the
    // implementation itself (a white-box test program), so it is compiled alone.
    std::string build_script(Script kind, const std::string& exe_main, bool unity);

private:
    // The public header's pieces (§15.7).
    std::set<const RecordInfo*> public_records() const;
    void record_def(Out& o, const RecordInfo* r) const;
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
    void sim_adjust();
    void sim_on_step();
    void sim_integrate();
    void sim_integrate_dynamic();
    void sim_record();
    void sim_run();
    void sim_maps();

    // The expression a consumer reads for one of its inputs (§15.3).
    std::string input_expr(const Leaf& leaf, const std::string& port) const;
    std::string in_braces(const Leaf& leaf, const ast::Method* method) const;
    bool needs_tick_gate(const Leaf& leaf) const;
    bool minor_step_output(const Leaf& leaf) const;
    // True when the leaf's decimation is a runtime value rather than 1.
    bool dynamic_rate(const Leaf& leaf) const;
    static bool adjusts(const Leaf& leaf);
    std::string decim_of(const Leaf& leaf) const;

    // One row per scalar slot of a block: { model path, C++ member path,
    // scalar type, unit }. Built once and used by BOTH the manifest and the
    // offsetof table, so the two cannot drift apart.
    using SlotRow = std::array<std::string, 4>;
    std::vector<SlotRow> signal_rows() const;
    std::vector<SlotRow> discrete_rows() const;
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
// The three pure methods -- `output()`, `derivative()` and `next()` -- get
// nothing at all: the leaf already holds a trace-only `log` member and a
// reflection-only `sim` member (§10.5), so their bodies are verbatim from
// brace to brace.
void Emitter::preamble(const NodeInfo& node, ast::Method::Which which, const char* ind) {
    using W = ast::Method::Which;
    const bool mutating = which == W::Init || which == W::OnStep || which == W::Final;
    if (!mutating) return;

    // Widens the leaf's reflection-only `sim` member with the two calls that
    // end a run. `final()` is deliberately NOT here: it already reads the
    // outcome from `ctx` (§10.3), and halting a run that is already ending is
    // not something §10.2 offers. It keeps the plain `RunView` member, so
    // `sim.time()` works there while `sim.stop` stays "no such member".
    if (which == W::Init || which == W::OnStep)
        o_ << ind << "const se_rt::RunControl sim{se_run_, se_path_, se_type_};"
           << "   // adds sim.stop / sim.abort, §10.2\n";
    // Shadows the leaf's trace-only member with the full set of levels.
    o_ << ind << "const se_rt::Log log{se_path_};" << "   // every level is legal here, §10.1\n";

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
        o_ << ind << "auto& " << n.name << " = se_nat_" << n.name << ";\n";
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

void Emitter::records() {
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

    // ── the view structs ─────────────────────────────────────────────────────
    auto field_row = [](const std::string& name, const Type& t, const char* init) {
        Row r;
        r.type = cpp_type(t);
        r.decl = name + init + ";";
        if (!t.is_record) r.comment = unit_comment(t.unit);
        return r;
    };

    std::vector<Row> rows;
    for (const auto& s : node.settings) {   // §6.2b — `rate` included
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

    // Both kinds of state are views: continuous into `Sim::x`, discrete into
    // `Sim::dis`. Held BY VALUE, so constness propagates from the method.
    rows.clear();
    for (const auto& s : node.states) {
        Row r = field_row(s.name, s.type, "");
        // §6.4a — an array state is a view too, one step further: a pointer
        // plus a length, because the length is a configuration result and a
        // `std::array` member is not available even in principle.
        r.type = s.extent ? "state_arr"
                          : (s.continuous ? "state_ref" : "value_ref<" + s.type.scalar + ">");
        rows.push_back(r);
    }
    emit_struct(o_, "State", rows);

    if (node.has_continuous()) {
        rows.clear();
        for (const auto& s : node.states) {
            if (!s.continuous) continue;
            rows.push_back({s.extent ? "state_arr" : "state_ref", s.name + ";",
                            unit_comment(s.der_unit)});
        }
        emit_struct(o_, "Der  ", rows);
        // §6.4b / §15.5 — `Adj` is a SECOND VIEW OF x, not a fourth block: the
        // same slots `State` binds, handed to a const method as a non-const
        // out-parameter. That aliasing is the feature and the footgun both, and
        // the field set is `Der`'s, so `adjust.<discrete>` is "no such member"
        // exactly as `der.<discrete>` is.
        //
        // Not `Der`'s ROWS, though: these fields hold the state itself, so the
        // comment is the state's declared unit and not the derivative's.
        if (node.method(ast::Method::Which::Adjust)) {
            rows.clear();
            for (const auto& s : node.states) {
                if (!s.continuous) continue;
                rows.push_back({s.extent ? "state_arr" : "state_ref", s.name + ";",
                                unit_comment(s.type.unit)});
            }
            emit_struct(o_, "Adj  ", rows);
        }
    }
    if (node.has_discrete()) {
        // This node's slice of the discrete block. It is also the type of
        // `next()`'s `next` out-parameter — the x+ = h(t,x,u) buffer is the
        // same shape as the storage, which is what makes one whole-block
        // `dis = nxt` a simultaneous update across the entire model (§6.4).
        rows.clear();
        for (const auto& s : node.states) {
            if (s.continuous) continue;
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

    rows.clear();
    for (const Field& f : node.outputs) rows.push_back(field_row(f.name, f.type, "{}"));
    emit_struct(o_, "Out  ", rows);

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
        const std::string name =
            std::string("In_") + (meth.which == W::Output       ? "output"
                                  : meth.which == W::Derivative ? "derivative"
                                  : meth.which == W::Next       ? "next"
                                  : meth.which == W::Adjust     ? "adjust"
                                                                : "step");
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
    // §10.5 - the reflection surface's backing. `se_run_` points at the one
    // RunState the enclosing Sim owns, so `sim.time()` is a member read and not
    // a global: two models in one process do not share a clock.
    mem.push_back({"const char* const", "se_type_;", "// fq node type, §10.5"});
    mem.push_back({"const se_rt::RunState* const", "se_run_;", "// this model's clock"});
    if (!node.states.empty()) mem.push_back({"State", "state;", "// views into x / dis"});
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
    mem.push_back({"const se_rt::TraceLog", "log{se_path_};",
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
                o_ << "    void output(const In_output& in, Out& out) const {\n";
                break;
            case W::Derivative:
                o_ << "    void derivative(const In_derivative& in, Der& der) const {\n";
                break;
            case W::Next:
                // The parameter shadows this member function's own name. Legal,
                // and invisible to authors: §11 copies bodies verbatim, so what a
                // body writes is `next.<state>`, which resolves to the parameter.
                // The accessor token has to be spelled `next` (§6.10).
                o_ << "    void next(const In_next& in, Store& next) const {\n";
                break;
            case W::Adjust:
                // The out-parameter shadows this member function's name, as
                // `next`'s does and for the same reason: the accessor token has
                // to be spelled `adjust` (§6.10) and §11 copies bodies verbatim.
                o_ << "    void adjust(const In_adjust& in, Adj& adjust) const {\n";
                break;
            case W::OnStep:
                o_ << "    void on_step(const In_step& in"
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

// EVERY slow node is gated. This used to exempt a slow node whose `output()`
// read nothing, on the premise that it was "a function of state alone" and so
// gave the same answer on every tick — the branch was pure cost. §10.5 made
// that premise false: `sim.time()` moves every tick, the generator cannot see
// which bodies read it (§11.1 — bodies are verbatim, never parsed), and an
// ungated node reading it would silently stop being zero-order held.
//
// The price is one `k % decim` branch per slow node per tick, and what it buys
// is ZOH by construction rather than by an argument about body contents.
bool Emitter::needs_tick_gate(const Leaf& leaf) const { return dynamic_rate(leaf); }

// §10.5 — whether this leaf's `output()` is re-evaluated at the solver's minor
// steps. A node with `discrete` states or a declared `rate` is zero-order held
// across the step (§9.3), so re-running it there would let `sim.time()` move
// underneath a held output. Everything else — continuous and purely algebraic
// nodes — is re-run, which is exactly what lets a CONTINUOUS source see the
// minor-step time and stop RK4 degrading to first order across a forcing path.
bool Emitter::minor_step_output(const Leaf& leaf) const {
    return !dynamic_rate(leaf) && !leaf.has_discrete();
}

// §9.2 — whether this leaf's decimation can move after the model is compiled.
// A node that declares `rate` owns an ordinary runtime setting (§6.2), so its
// decimation is a member that `resolve_settings()` recomputes, and the tick
// guards have to read it even where elaboration happened to compute 1. A node
// that declares no `rate` runs at the base step and nothing can change that,
// so its guards are not emitted at all.
//
// The test is therefore on the node's SHAPE, not on its elaborated decimation,
// which is no longer the final word on anything.
bool Emitter::dynamic_rate(const Leaf& leaf) const {
    return leaf.node->setting("rate") != nullptr;
}

std::string Emitter::decim_of(const Leaf& leaf) const { return leaf.ident + "_decim"; }

// §6.4b — whether this leaf has a jump map to run. Both halves matter: a node
// with no continuous states has an empty `Adj` and never gets this far
// (SE0346 rejects it), and a node that declares no `adjust()` gets no member,
// no binding and no call — so the feature costs a model that does not use it
// exactly nothing.
bool Emitter::adjusts(const Leaf& leaf) {
    return leaf.has_continuous() && leaf.node->method(ast::Method::Which::Adjust);
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
    o_ << "    //  Together they are a complete checkpoint. The *_map() tables below\n";
    o_ << "    //  give path, offset and unit, so an inspector, a shared-memory host or\n";
    o_ << "    //  a replay tool needs no generated accessor code at all.\n\n";

    if (!m_.dynamic_states) {
        o_ << "    static constexpr std::size_t n_states = " << m_.n_states << ";\n";
        for (const Leaf& leaf : m_.leaves)
            for (const StateSlot& s : leaf.states)
                if (s.continuous)
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
                if (!st.continuous) continue;
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
        o_ << "    Discrete nxt{};   // x+ being written; tick() ends with dis = nxt\n\n";
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
                if (st.continuous || !st.extent.present) continue;
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
    // §10.5 — the reflection surface every leaf points at. Declared ahead of
    // the leaves because their initialisers take its address, and owned by the
    // Sim rather than by the process so two models never share a clock.
    o_ << "    se_rt::RunState se_run_{};   // what `sim.time()` reads, §10.5\n";
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

    bool any_adj = false;
    for (const Leaf& leaf : m_.leaves) any_adj = any_adj || adjusts(leaf);
    if (any_adj) {
        o_ << "\n    // §6.4b - `adj` is a SECOND VIEW OF x, not scratch and not a\n";
        o_ << "    // block of its own: the same slots `state` binds. It lives here\n";
        o_ << "    // rather than in the leaf because the out-parameter has to be\n";
        o_ << "    // non-const at the call site while `adjust()` is a const member.\n";
        std::size_t w = 0;
        for (const Leaf& leaf : m_.leaves)
            if (adjusts(leaf)) w = std::max(w, leaf.node->cpp_name.size() + 5);
        for (const Leaf& leaf : m_.leaves)
            if (adjusts(leaf))
                o_ << "    " << pad(leaf.node->cpp_name + "::Adj", w) << " " << leaf.ident
                   << "_adj;\n";
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

    // §9.2 — the schedule's divisors. Derived from `rate` by resolve_settings()
    // rather than baked into the tick loop, because `rate` is an ordinary
    // runtime setting: an override has to move the SCHEDULE and not only the
    // coefficients that read `sample_rate`. The initialiser is what elaboration
    // computed, and the constructor recomputes it before anything observes it.
    {
        std::size_t w = 0;
        for (const Leaf& leaf : m_.leaves)
            if (dynamic_rate(leaf)) w = std::max(w, decim_of(leaf).size());
        bool any = false;
        for (const Leaf& leaf : m_.leaves) {
            if (!dynamic_rate(leaf)) continue;
            if (!any) o_ << "\n";
            any = true;
            o_ << "    std::uint64_t " << pad(decim_of(leaf), w) << " = " << leaf.decimation
               << ";   // " << leaf.path << "\n";
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
    o_ << "\n    double        se_time_ = 0.0;\n";
    o_ << "    std::uint64_t se_tick_ = 0;\n";
    o_ << "    std::size_t   se_inited_ = 0;\n";
    o_ << "    bool          se_done_ = false;\n";
    if (m_.record.present) o_ << "    se_rt::Recorder se_rec_;\n";

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
    o_ << "\n    void resolve_settings();\n";
    o_ << "\n    void bind();\n";
    o_ << "    void apply_initial_conditions();\n";
    o_ << "    void output_pass(std::uint64_t k);\n";
    o_ << "    void output_pass_minor();\n";
    o_ << "    void next_tick(std::uint64_t k);\n";
    o_ << "    void adjust_tick(std::uint64_t k);\n";
    o_ << "    void on_step_tick(std::uint64_t k);\n";
    o_ << "    void integrate(double t, double h);\n";
    o_ << "    void final_all(const se_rt::RunContext& ctx) const;\n";
    if (m_.record.present) o_ << "    void record_open();\n    void record_row();\n";
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
        const std::string rhs =
            s.scalar == "double"
                ? render_expr(s.expr)
                : "static_cast<" + s.scalar + ">(" + render_expr(s.expr) + ")";
        o_ << "    if (!se_pin_[" << i << "]) " << pad(lhs, w) << " = "
           << rhs << ";"
           << "   // " << s.unit.str() << "\n";
    }

    // §9.2 — the schedule's divisors follow `rate`, in the same pass and by the
    // same rule as every other derived value. SE0450's check comes with them:
    // `sec` rejected a bad elaborated default, and this rejects a bad
    // configured one, which is the only place it can now be caught (§16.4).
    bool any = false;
    for (const Leaf& leaf : m_.leaves) {
        if (!dynamic_rate(leaf)) continue;
        if (!any) {
            o_ << "\n    se_cfg_error_.clear();\n";
            any = true;
        }
        const std::string path = leaf.path.empty() ? std::string("<root>") : leaf.path;
        o_ << "    if (!se_rt::decimation_of(1.0 / step_seconds, " << param_base(leaf.path)
           << ".rate, " << decim_of(leaf) << ") && se_cfg_error_.empty())\n";
        o_ << "        se_cfg_error_ = se_rt::rate_error(" << quote(path + ".rate") << ", "
           << param_base(leaf.path) << ".rate, 1.0 / step_seconds);\n";
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
            if (!st.continuous) continue;
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
                if (st.continuous || !st.extent.present) continue;
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
                if (st.continuous != continuous) continue;
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
                if (st.continuous != continuous) continue;
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
    for (const Leaf& leaf : m_.leaves)
        for (const StateSlot& st : leaf.states) {
            if (!st.continuous) continue;
            const std::string off = "se_x_off_[" + std::to_string(st.off) + "]";
            const std::string len = "se_x_len_[" + std::to_string(st.off) + "]";
            if (st.extent.present) {
                o_ << "    " << leaf.ident << ".state." << st.name << ".bind(x + " << off
                   << ", " << len << ");\n";
                o_ << "    " << leaf.ident << "_der." << st.name << ".bind(xd + " << off
                   << ", " << len << ");\n";
                // Same block, same offset, same length as `state`: the alias IS
                // the lowering of §6.4b.
                if (adjusts(leaf))
                    o_ << "    " << leaf.ident << "_adj." << st.name << ".bind(x + " << off
                       << ", " << len << ");\n";
            } else {
                o_ << "    " << leaf.ident << ".state." << st.name << ".bind(x[" << off
                   << "]);\n";
                o_ << "    " << leaf.ident << "_der." << st.name << ".bind(xd[" << off
                   << "]);\n";
                if (adjusts(leaf))
                    o_ << "    " << leaf.ident << "_adj." << st.name << ".bind(x[" << off
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
            if (st.continuous || !st.extent.present) continue;
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
        }
    o_ << "    se_bound_da_ = true;\n}\n";
}

void Emitter::sim_elaborate() {
    // Settings now arrive through the leaf member initializers, so there is no
    // elaborate() left to emit. What remains is the declarative ICs.
    o_ << "\n// Declarative state defaults (§6.4). External IC overrides folded in\n";
    o_ << "// here too — the single override channel, which is what a discrete state\n";
    o_ << "// buys over a var. A continuous write lands in x through the proxy.\n";
    o_ << "void Sim::Impl::apply_initial_conditions() {\n";
    std::size_t w = 0;
    bool any_discrete = false;
    for (const Leaf& leaf : m_.leaves) {
        any_discrete = any_discrete || leaf.has_discrete();
        for (const StateSlot& s : leaf.states)
            w = std::max(w, leaf.ident.size() + s.name.size() + (s.continuous ? 7u : 5u) +
                                (s.extent.present ? 7u : 0u));
    }
    for (const Leaf& leaf : m_.leaves)
        for (const StateSlot& s : leaf.states) {
            const std::string base = s.continuous ? leaf.ident + ".state." + s.name
                                                  : "dis." + leaf.ident + "." + s.name;
            // §6.4a — the declared default initialises EVERY element. There
            // is no per-element default syntax: non-uniform initialisation is
            // init()'s job, which is where a computed IC already belonged.
            if (s.extent.present) {
                o_ << "    " << pad(base + ".fill(" + literal(s.initial, s.scalar) + ");",
                                    w + 13)
                   << " // " << s.unit.str() << "\n";
                continue;
            }
            o_ << "    " << pad(base, w) << " = "
               << pad(literal(s.initial, s.scalar) + ";", 10) << " // " << s.unit.str()
               << "\n";
        }
    // §13.4 — element ICs from a settings source, after the uniform default
    // they refine. The index was checked at configuration and init() refuses to
    // run while a configuration error stands, so nothing is guarded here.
    bool any_element = false;
    for (const Leaf& leaf : m_.leaves)
        for (const StateSlot& s : leaf.states)
            for (const StateSlot::ElementIC& ic : s.element_ic) {
                if (!any_element) {
                    o_ << "\n    // §13.4 — per-element initial conditions.\n";
                    any_element = true;
                }
                const std::string path =
                    (leaf.path.empty() ? s.name : leaf.path + "." + s.name) + "[" +
                    std::to_string(ic.index) + "]";
                const std::string block = s.continuous ? "x[se_x_off_[" : "dis_arr[se_da_off_[";
                o_ << "    " << block << s.off << "] + " << ic.index << "] = "
                   << literal(ic.value, s.scalar) << ";   // " << path << "\n";
            }
    if (any_discrete)
        o_ << "    nxt = dis;   // so the first `dis = nxt` is a no-op\n";
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
            if (s.continuous) {
                if (m_.dynamic_states) continue;
                o_ << "    " << leaf.ident << ".state." << s.name << ".bind(x[" << s.slot
                   << "]);\n";
                o_ << "    " << leaf.ident << "_der." << s.name << ".bind(xd[" << s.slot
                   << "]);\n";
                if (adjusts(leaf))
                    o_ << "    " << leaf.ident << "_adj." << s.name << ".bind(x[" << s.slot
                       << "]);\n";
            } else {
                if (s.extent.present) continue;
                o_ << "    " << leaf.ident << ".state." << s.name << ".bind(dis."
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
    o_ << "void Sim::Impl::output_pass(std::uint64_t k) {\n";
    for (std::size_t i : m_.order) {
        const Leaf& leaf = m_.leaves[i];
        const ast::Method* out = leaf.node->method(ast::Method::Which::Output);
        if (!out) continue;
        const std::string call = leaf.ident + ".output(" + in_braces(leaf, out) + ", sig." +
                                 leaf.ident + ");";
        if (needs_tick_gate(leaf))
            o_ << "    if (k % " << decim_of(leaf) << " == 0) " << call << "\n";
        else
            o_ << "    " << call << "\n";
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
    for (std::size_t i : m_.order) {
        const Leaf& leaf = m_.leaves[i];
        const ast::Method* out = leaf.node->method(ast::Method::Which::Output);
        if (!out || !minor_step_output(leaf)) continue;
        any = true;
        o_ << "    " << leaf.ident << ".output(" << in_braces(leaf, out) << ", sig."
           << leaf.ident << ");\n";
    }
    if (!any) o_ << "    // every node is zero-order held across the step\n";
    o_ << "}\n";
}

void Emitter::sim_derivatives() {
    o_ << "\n// The external solver's entry point: reads x, fills xd. Sim owns both,\n";
    o_ << "// so a solver with its own vector does one memcpy each way per call.\n";
    o_ << "void Sim::Impl::derivatives(double t) {\n";
    o_ << "    se_run_.now = t;   // the minor-step instant, §10.5\n";
    o_ << "    output_pass_minor();\n";
    for (const Leaf& leaf : m_.leaves) {
        const ast::Method* r = leaf.node->method(ast::Method::Which::Derivative);
        if (!r || !leaf.has_continuous()) continue;
        o_ << "    " << leaf.ident << ".derivative(" << in_braces(leaf, r) << ", "
           << leaf.ident << "_der);\n";
    }
    o_ << "}\n";
}

// x+ = h(t,x,u), once per accepted base step, after outputs have propagated.
void Emitter::sim_next() {
    o_ << "\n// Every next() reads `dis` and writes `nxt`; tick() then does one\n";
    o_ << "// whole-block `dis = nxt`, which is what makes the update simultaneous\n";
    o_ << "// across the MODEL and not merely within a node (§6.4). A node that did\n";
    o_ << "// not sample this tick left its slice of `nxt` equal to `dis`, so the\n";
    o_ << "// assignment is a no-op for it.\n";
    o_ << "void Sim::Impl::next_tick(std::uint64_t k) {\n    (void)k;\n";
    for (const Leaf& leaf : m_.leaves) {
        const ast::Method* s = leaf.node->method(ast::Method::Which::Next);
        if (!s || !leaf.has_discrete()) continue;
        const std::string args = in_braces(leaf, s) + ", nxt." + leaf.ident;
        if (dynamic_rate(leaf))
            o_ << "    if (k % " << decim_of(leaf) << " == 0) " << leaf.ident
               << ".next(" << args << ");"
               << "   // " << leaf.decimation << ":1 by default\n";
        else
            o_ << "    " << leaf.ident << ".next(" << args << ");\n";
    }
    o_ << "}\n";
}

// x+ = j(t,x,u): the jump map, §6.4b. Once per accepted base step at each
// node's sample instants, and BELOW the recording and the exit (§9.6 step 7),
// which is what keeps a recorded row one consistent sample of the entry state
// and keeps a halted run holding the x that row reported.
//
// There is no commit and no buffer. `adj` views the same slots as `state`, so
// the write lands in `x` directly — the one place in the language where two
// accessors alias, which §6.4b documents as a footgun rather than hiding.
void Emitter::sim_adjust() {
    o_ << "\n// The jump map (§6.4b): the only write to `x` outside the solver, and\n";
    o_ << "// the last thing that happens before the state advances. `adj` views the\n";
    o_ << "// SAME slots as `state`, so this is an in-place correction, not a buffer.\n";
    o_ << "void Sim::Impl::adjust_tick(std::uint64_t k) {\n    (void)k;\n";
    for (const Leaf& leaf : m_.leaves) {
        if (!adjusts(leaf)) continue;
        const ast::Method* a = leaf.node->method(ast::Method::Which::Adjust);
        const std::string args = in_braces(leaf, a) + ", " + leaf.ident + "_adj";
        if (dynamic_rate(leaf))
            o_ << "    if (k % " << decim_of(leaf) << " == 0) " << leaf.ident
               << ".adjust(" << args << ");"
               << "   // " << leaf.decimation << ":1 by default\n";
        else
            o_ << "    " << leaf.ident << ".adjust(" << args << ");\n";
    }
    o_ << "}\n";
}

// The mutating per-step hook: `var`, natives, logging. It carries no equation,
// which is why it is named for its moment rather than for an accessor (§8.1).
void Emitter::sim_on_step() {
    o_ << "\n// Once per accepted base step, at each node's sample instants. Runs\n";
    o_ << "// after next(), and like every other method in the tick it observes the\n";
    o_ << "// tick's ENTRY state: both `dis` and `x` advance at the end of tick().\n";
    o_ << "void Sim::Impl::on_step_tick(std::uint64_t k) {\n    (void)k;\n";
    for (const Leaf& leaf : m_.leaves) {
        const ast::Method* s = leaf.node->method(ast::Method::Which::OnStep);
        if (!s) continue;
        std::string args = in_braces(leaf, s);
        if (!leaf.node->vars.empty()) args += ", " + leaf.ident + ".var";
        if (dynamic_rate(leaf))
            o_ << "    if (k % " << decim_of(leaf) << " == 0) " << leaf.ident
               << ".on_step(" << args << ");"
               << "   // " << leaf.decimation << ":1 by default\n";
        else
            o_ << "    " << leaf.ident << ".on_step(" << args << ");\n";
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

void Emitter::sim_record() {
    if (!m_.record.present) return;
    o_ << "\n// Recording is quantitative: an aligned numeric table. The header\n";
    o_ << "// carries units, because the values themselves no longer do.\n";
    o_ << "void Sim::Impl::record_open() {\n";
    o_ << "    if (!se_rec_.open(" << quote(m_.record.file) << ")) {\n";
    o_ << "        se_rt::logs().write(se_rt::Level::Error, \"sim\", 0.0,\n";
    o_ << "                            \"cannot open " << m_.record.file
       << " for writing\");\n        return;\n    }\n";
    o_ << "    se_rec_.header({";
    for (std::size_t i = 0; i < m_.record.signals.size(); ++i) {
        if (i) o_ << ",";
        o_ << "\n                "
           << quote(m_.record.signals[i].path + " [" + m_.record.signals[i].unit.str() + "]");
    }
    o_ << "});\n}\n";

    o_ << "\nvoid Sim::Impl::record_row() {\n    se_rec_.row(se_time_, {";
    for (std::size_t i = 0; i < m_.record.signals.size(); ++i) {
        if (i) o_ << ",";
        // §13.5 — a port path that ended on a setting could not be rendered at
        // elaboration, because a setting reference needs an identifier this
        // stage assigns. Everything else arrives already written.
        const RecordedSignal& s = m_.record.signals[i];
        o_ << "\n                       static_cast<double>("
           << (s.code.empty() ? s.expr : render_expr(s.code)) << ")";
    }
    o_ << "});\n}\n";
}

void Emitter::sim_run() {
    bool any_discrete = false;
    for (const Leaf& leaf : m_.leaves) any_discrete = any_discrete || leaf.has_discrete();
    bool any_adjust = false;
    for (const Leaf& leaf : m_.leaves) any_adjust = any_adjust || adjusts(leaf);

    // ── init ─────────────────────────────────────────────────────────────────
    o_ << "\nvoid Sim::Impl::init() {\n";
    o_ << "    // §6.2b — configuration precedes init(), so a value it refused stops\n";
    o_ << "    // the run here, before any node sees state it was never compiled for.\n";
    o_ << "    if (!se_cfg_error_.empty()) { se_rt::control().fail(se_cfg_error_); return; }\n";
    if (m_.dynamic_states) {
        // §15.5a — the buffers are the host's, so their absence is the one
        // failure the handshake makes possible after a clean configuration.
        o_ << "    if (n_states && !se_bound_x_) {\n";
        o_ << "        se_rt::control().fail(" << quote("bind_state() was not called; "
              "this model's state buffers are the host's (§15.5a)") << ");\n";
        o_ << "        return;\n    }\n";
        if (m_.n_disc_arr_slots) {
            o_ << "    if (n_dstates && !se_bound_da_) {\n";
            o_ << "        se_rt::control().fail("
               << quote("bind_discrete_array() was not called (§15.5a)") << ");\n";
            o_ << "        return;\n    }\n";
        }
    }
    o_ << "    apply_initial_conditions();\n";
    o_ << "    se_tick_ = 0;\n    se_time_ = 0.0;\n    se_done_ = false;\n";
    o_ << "    se_run_ = se_rt::RunState{0.0, step_seconds, 0};   // §10.5\n";
    o_ << "    se_inited_ = 0;\n";
    if (m_.record.present) o_ << "    record_open();\n";
    o_ << "    se_rt::realtime().start();\n";
    for (std::size_t i = 0; i < m_.leaves.size(); ++i) {
        const Leaf& leaf = m_.leaves[i];
        o_ << "    if (se_rt::control().halted()) return;\n";
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
    o_ << "// recorder: one row is one consistent sample of the model at t[k]. Both\n";
    o_ << "// state kinds advance together at the bottom, below the exit, so a run\n";
    o_ << "// that stops is left holding a valid checkpoint (§15.5).\n";
    o_ << "void Sim::Impl::tick() {\n";
    o_ << "    se_time_ = static_cast<double>(se_tick_) * step_seconds;\n";
    o_ << "    se_rt::sim_time() = se_time_;\n";
    o_ << "    se_run_.now = se_time_;   // §10.5 — the tick instant; derivatives()\n";
    o_ << "    se_run_.tick = se_tick_;  // moves it again at every minor step\n";
    o_ << "    output_pass(se_tick_);\n";
    o_ << "    publish_outputs();\n";
    o_ << "    if (!se_rt::control().halted()) {\n";
    if (any_discrete) o_ << "        next_tick(se_tick_);\n";
    o_ << "        on_step_tick(se_tick_);\n";
    o_ << "    }\n";
    if (m_.record.present) {
        if (m_.record.every > 1)
            o_ << "    if (se_tick_ % " << m_.record.every << " == 0) record_row();\n";
        else
            o_ << "    record_row();\n";
    }
    o_ << "    if (se_rt::control().halted() || se_tick_ >= tick_count) {\n";
    o_ << "        se_done_ = true;\n        return;\n    }\n";
    // §9.6 step 7 — below the exit, so a halted run keeps the x its last row
    // reported; above the advance, so the solver starts from the adjusted state.
    if (any_adjust) o_ << "    adjust_tick(se_tick_);\n";
    if (any_discrete)
        o_ << "    dis = nxt;   // simultaneous update, whole model (§6.4)\n";
    o_ << "    integrate(se_time_, step_seconds);\n";
    if (m_.realtime && m_.sync_steps > 0)
        o_ << "    if ((se_tick_ + 1) % " << m_.sync_steps
           << " == 0) se_rt::realtime().frame_boundary();\n";
    o_ << "    ++se_tick_;\n";
    o_ << "}\n";

    // ── finish ───────────────────────────────────────────────────────────────
    o_ << "\nvoid Sim::Impl::finish() {\n";
    o_ << "    se_rt::control().complete();\n";
    o_ << "    const se_rt::RunContext ctx = se_rt::control().context(se_time_);\n";
    o_ << "    final_all(ctx);\n";
    if (m_.record.present) o_ << "    se_rec_.close();\n";
    if (m_.realtime) o_ << "    se_rt::realtime().summary();\n";
    // §10.6 -- after the realtime summary and before the closing line, so
    // the last thing a log carries is the run's outcome and not a contract detail.
    o_ << "    se_rt::checks().summary();\n";
    o_ << "    se_rt::logs().write(se_rt::Level::Info, \"sim\", se_time_,\n";
    o_ << "                        se_rt::format(\"run {}: {}\", ctx.reason,\n";
    o_ << "                                      ctx.message.empty() ? \"-\" "
          ": ctx.message.c_str()));\n";
    o_ << "}\n";

    // ── the exit code, and the reason ────────────────────────────────────────
    // §10.6 -- a run that violated a contract is not a successful run, even
    // when nothing halted it. That is what lets a contract BE a test oracle
    // instead of something a harness has to go grepping the log for.
    o_ << "\n// 0 for a clean run; 1 if it was aborted, failed, or broke a contract (§10.6).\n";
    o_ << "int Sim::Impl::exit_code() const {\n";
    o_ << "    const se_rt::Reason r = se_rt::control().reason();\n";
    o_ << "    const bool failed = r == se_rt::Reason::Aborted || r == se_rt::Reason::Error;\n";
    o_ << "    return failed || se_rt::checks().any() ? 1 : 0;\n";
    o_ << "}\n";

    o_ << "\nconst char* Sim::Impl::error() const {\n";
    o_ << "    const std::string& m = se_rt::control().message();\n";
    o_ << "    return m.empty() ? se_cfg_error_.c_str() : m.c_str();\n";
    o_ << "}\n";

    // ── start ────────────────────────────────────────────────────────────────
    o_ << "\n// Sim::init(): the run configuration from the .sim, the host's settings,\n";
    o_ << "// everything derived from them (§6.2b), then every node's init().\n";
    o_ << "bool Sim::Impl::start() {\n";
    o_ << "    configure_run();\n";
    o_ << "    const std::string refused = accept_settings();\n";
    o_ << (m_.dynamic_states ? "    configure();\n" : "    resolve_settings();\n");
    o_ << "    if (!refused.empty() && se_cfg_error_.empty()) se_cfg_error_ = refused;\n";
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
    o_ << "    return !se_rt::control().halted();\n";
    o_ << "}\n";

    // ── the run configuration the .sim file gave ─────────────────────────────
    o_ << "\n// Logging (§13.7) and the real-time frame (§9.4), as the .sim file set them.\n";
    o_ << "void Sim::Impl::configure_run() {\n";
    o_ << "    se_rt::logs().configure(" << quote(m_.log.file) << ", se_rt::Level::";
    const std::string lvl = m_.log.level;
    o_ << (lvl == "trace"   ? "Trace"
           : lvl == "debug" ? "Debug"
           : lvl == "warn"  ? "Warn"
           : lvl == "error" ? "Error"
                            : "Info")
       << ");\n";
    for (const auto& kv : m_.log.levels) {
        const std::string& l = kv.second;
        o_ << "    se_rt::logs().set_path_level(" << quote(kv.first) << ", se_rt::Level::"
           << (l == "trace"   ? "Trace"
               : l == "debug" ? "Debug"
               : l == "warn"  ? "Warn"
               : l == "error" ? "Error"
                              : "Info")
           << ");\n";
    }
    if (m_.realtime)
        o_ << "    se_rt::realtime().configure(" << dbl(m_.sync) << ", " << dbl(m_.window)
           << ");\n";
    else
        o_ << "    se_rt::realtime().configure(0.0, 0.0);   // mode: batch\n";
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
    sim_adjust();
    sim_on_step();
    sim_integrate();
    sim_record();
    sim_run();
    sim_public_glue();
    sim_maps();
    sim_facade();
    if (!ns.empty()) o_ << "\n}  // namespace " << ns << "\n";
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
    o_ << "bool                Sim::init()             { return impl_->start(); }\n";
    o_ << "const char*         Sim::error() const      { return impl_->error(); }\n";
    o_ << "Sim::Inputs&        Sim::inputs()           { return impl_->sig.in; }\n";
    o_ << "void                Sim::tick()             { impl_->tick(); }\n";
    o_ << "const Sim::Outputs& Sim::outputs() const    { return impl_->se_outputs_; }\n";
    o_ << "bool                Sim::done() const       { return impl_->done(); }\n";
    o_ << "std::uint64_t       Sim::tick_index() const { return impl_->tick_index(); }\n";
    o_ << "double              Sim::time() const       "
          "{ return static_cast<double>(tick_index()) * step_seconds; }\n";
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
            if (s.continuous) continue;
            // §15.5a — an array-shaped discrete state is not in `dis` at
            // all. It lives in the flat fourth block, whose rows are built at
            // configuration because their count is not known before then.
            if (s.extent.present) continue;
            rows.push_back({leaf.path.empty() ? s.name : leaf.path + "." + s.name,
                            leaf.ident + "." + s.name, s.scalar, s.unit.str()});
        }
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
            if (s.continuous != continuous || !s.extent.present) continue;
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
                return literal(m_.settings[i].value, m_.settings[i].scalar);
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
    auto zero = [](const Type& t) { return t.is_record ? std::string("{}") : literal(0.0, t.scalar); };

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
    o_ << "\n";
    const std::string step = dbl(m_.step) + ";", ticks = std::to_string(tick_count()) + ";";
    const std::size_t wv = std::max(step.size(), ticks.size());
    o_ << "    static constexpr double        step_seconds = " << pad(step, wv)
       << "   // s, one tick\n";
    o_ << "    static constexpr std::uint64_t tick_count   = " << pad(ticks, wv)
       << "   // the run's length, in ticks\n\n";

    o_ << "    Sim();\n";
    o_ << "    ~Sim();\n";
    o_ << "    Sim(Sim&&) noexcept;\n";
    o_ << "    Sim& operator=(Sim&&) noexcept;\n\n";

    o_ << "    // ---- 1. configure: before init() " << std::string(40, '-') << "\n";
    o_ << "    Settings& settings();\n\n";

    o_ << "    // ---- 2. start " << std::string(58, '-') << "\n";
    o_ << "    //  Applies settings(), sets every initial condition and runs every\n";
    o_ << "    //  node's init(). False if a setting was refused or a node stopped the\n";
    o_ << "    //  run: error() says why, and finish() is still owed.\n";
    o_ << "    bool        init();\n";
    o_ << "    const char* error() const;\n\n";

    o_ << "    // ---- 3. step " << std::string(59, '-') << "\n";
    o_ << "    //  tick() samples the model at time(), with inputs() as the host left\n";
    o_ << "    //  them, then advances one step. outputs() hold what it sampled.\n";
    o_ << "    Inputs&        inputs();\n";
    o_ << "    void           tick();\n";
    o_ << "    const Outputs& outputs() const;\n";
    o_ << "    bool           done() const;         // the run is over: tick_count, or stopped\n";
    o_ << "    double         time() const;         // s, the instant the next tick() samples\n";
    o_ << "    std::uint64_t  tick_index() const;   // the next tick(); time() / step_seconds\n\n";

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
    o_ << "#include <cstdint>\n#include <memory>\n\n";

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
    o_ << "#include <memory>\n#include <string>\n";
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
    t << "#include <cstdio>\n\n";
    t << "int main() {\n";
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
          << literal(example->value, example->scalar) << ";   // " << example->unit.str()
          << "\n\n";
    else
        t << "\n\n";

    t << "    // 2. Start.\n";
    t << "    if (!sim.init()) {\n";
    t << "        std::fprintf(stderr, \"" << who << ": %s\\n\", sim.error());\n";
    t << "        return sim.finish();\n";
    t << "    }\n\n";

    if (!m_.boundary_in.empty()) {
        std::string names;
        for (const BoundaryIn& b : m_.boundary_in) names += (names.empty() ? "" : ", ") + b.ident;
        t << "    // 3. Step. " << m_.name << ".sim has no way to drive the root inputs, so\n";
        t << "    //    this run holds them at zero; a host of your own writes them before\n";
        t << "    //    every tick().\n";
        t << "    std::fprintf(stderr, \"" << who << ": nothing drives the root inputs, \"\n";
        t << "                         \"so they stay at zero: " << names << "\\n\");\n";
        t << "    while (!sim.done()) {\n";
        auto zero = [](const Type& ty) {
            return (ty.is_record ? std::string("{}") : literal(0.0, ty.scalar)) + ";";
        };
        std::size_t w = 0, wz = 0;
        for (const BoundaryIn& b : m_.boundary_in) {
            w = std::max(w, b.ident.size());
            wz = std::max(wz, zero(b.type).size());
        }
        for (const BoundaryIn& b : m_.boundary_in)
            t << "        sim.inputs()." << pad(b.ident, w) << " = " << pad(zero(b.type), wz)
              << "   // " << type_label(b.type) << "\n";
    } else {
        t << "    // 3. Step.\n";
        t << "    while (!sim.done()) {\n";
    }
    t << "        sim.tick();\n";
    if (!m_.root_outputs.empty()) {
        std::string names;
        for (const RootOutput& r : m_.root_outputs) names += (names.empty() ? "" : ", ") + r.name;
        t << "        // sim.outputs() now holds " << names << "\n";
        t << "        // at the instant this tick() sampled.\n";
    }
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
//  evaporated by this point (§15.2), so a `subgraph` below is the path
//  prefix one left behind and not a runtime object. Drawing the source
//  hierarchy instead would show what was WRITTEN; this shows what got built,
//  which is the thing whose behaviour anyone is trying to understand.
//
//  Mermaid rather than Graphviz because it renders with nothing installed —
//  GitHub, VS Code, any markdown preview — and because it is text, so it
//  diffs and can be a checked expectation exactly as the manifest already is.
//  "Nothing installed" holds only for a fenced block inside a `.md`, not for
//  a bare `.mmd`, so `emit()` writes it inside a ```mermaid fence; this
//  function returns the diagram alone and the fence is added at the write.
//  Both formats are a printer over this one walk, so a `--dot` twin later is
//  a second printer and not a second traversal.

std::string Emitter::topology() const {
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

    // What a wire carries, as a reader wants it: a unit for a scalar, the
    // record's own name for a record (§5.3 — the whole record travels).
    auto type_of = [](const Type& ty) -> std::string {
        if (!ty.is_record) return ty.unit.str();
        const std::size_t dot = ty.record->fq.rfind('.');
        std::string r =
            dot == std::string::npos ? ty.record->fq : ty.record->fq.substr(dot + 1);
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
    };

    // Position in the §8.5 order, so each group lists its nodes in the order
    // the tick actually calls them and Mermaid lays them out left to right.
    std::vector<std::size_t> rank(m_.leaves.size(), 0);
    for (std::size_t k = 0; k < m_.order.size(); ++k) rank[m_.order[k]] = k;

    // ---- the composite tree, recovered from the paths ----------------------
    struct Group {
        std::string name;
        std::vector<std::size_t> leaves;
        std::vector<std::size_t> subs;
    };
    std::vector<Group> groups;
    std::map<std::string, std::size_t> group_of;
    groups.push_back(Group{std::string(), {}, {}});
    group_of[std::string()] = 0;

    std::function<std::size_t(const std::string&)> ensure =
        [&](const std::string& path) -> std::size_t {
        auto it = group_of.find(path);
        if (it != group_of.end()) return it->second;
        const std::size_t dot = path.rfind('.');
        const std::size_t parent =
            ensure(dot == std::string::npos ? std::string() : path.substr(0, dot));
        groups.push_back(
            Group{dot == std::string::npos ? path : path.substr(dot + 1), {}, {}});
        const std::size_t id = groups.size() - 1;
        group_of[path] = id;
        groups[parent].subs.push_back(id);
        return id;
    };

    for (std::size_t i = 0; i < m_.leaves.size(); ++i) {
        const std::string& path = m_.leaves[i].path;
        const std::size_t dot = path.rfind('.');
        groups[ensure(dot == std::string::npos ? std::string() : path.substr(0, dot))]
            .leaves.push_back(i);
    }
    for (Group& g : groups)
        std::sort(g.leaves.begin(), g.leaves.end(),
                  [&](std::size_t a, std::size_t b) { return rank[a] < rank[b]; });

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
    for (const BoundaryIn& b : m_.boundary_in)
        t << "    se_in_" << b.ident << "([" << lab("in." + b.ident + "<br/>" +
                                                    type_of(b.type)) << "])\n";

    // ---- nodes, nested by composite ----------------------------------------
    std::function<void(std::size_t, const std::string&)> draw =
        [&](std::size_t g, const std::string& ind) {
        for (std::size_t i : groups[g].leaves) {
            const Leaf& leaf = m_.leaves[i];
            const std::size_t dot = leaf.path.rfind('.');
            std::string name = leaf.path.empty()
                                   ? std::string("<root>")
                                   : (dot == std::string::npos ? leaf.path
                                                               : leaf.path.substr(dot + 1));
            std::string text = name + "<br/>" + leaf.node->fq;
            // §9.2 — only a node that declares `rate` can be off the base
            // step, and only then is the number worth the ink.
            if (dynamic_rate(leaf) && m_.step > 0.0) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%g Hz",
                              1.0 / (m_.step * static_cast<double>(leaf.decimation)));
                text += "<br/>";
                text += buf;
            }
            t << ind << id(leaf.ident) << "[" << lab(text) << "]\n";
        }
        for (std::size_t sub : groups[g].subs) {
            t << ind << "subgraph sg" << sub << "[" << lab(groups[sub].name) << "]\n";
            t << ind << "    direction LR\n";
            draw(sub, ind + "    ");
            t << ind << "end\n";
        }
    };
    draw(0, "    ");

    // ---- the root outputs the host may read --------------------------------
    //  `boundary_out` is expanded field by field, because that is what a host
    //  wiring itself up needs (§15.6). A diagram wants the PORT, so the rows
    //  are collapsed back onto the producer they all came from.
    std::map<std::string, std::size_t> leaf_by_ident;
    for (std::size_t i = 0; i < m_.leaves.size(); ++i) leaf_by_ident[m_.leaves[i].ident] = i;

    std::vector<std::pair<std::string, std::string>> outs;   // producer ident, port
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
        for (const auto& o : outs) seen = seen || (o.first == ident && o.second == port);
        if (!seen) outs.emplace_back(ident, port);
    }
    for (std::size_t k = 0; k < outs.size(); ++k) {
        auto it = leaf_by_ident.find(outs[k].first);
        if (it == leaf_by_ident.end()) continue;
        const Leaf& leaf = m_.leaves[it->second];
        const std::string path =
            leaf.path.empty() ? outs[k].second : leaf.path + "." + outs[k].second;
        const Field* port = nullptr;
        for (const Field& f : leaf.ports_out)
            if (f.name == outs[k].second) port = &f;
        t << "    se_out" << k << "([" << lab("out " + path + "<br/>" +
                                              (port ? type_of(port->type) : std::string("?")))
          << "])\n";
    }

    // ---- wires -------------------------------------------------------------
    t << "\n";
    std::size_t konst = 0;   // §6.9.2 — numbers the constant-source terminals
    for (std::size_t i = 0; i < m_.leaves.size(); ++i) {
        const Leaf& leaf = m_.leaves[i];
        const ast::Method* out = leaf.node->method(ast::Method::Which::Output);
        for (const Field& port : leaf.ports_in) {
            auto it = leaf.inputs.find(port.name);
            if (it == leaf.inputs.end()) continue;   // unconnected; SE0430 said so
            const InputSource& src = it->second;

            // The SAME question schedule.cpp asks when it builds the sort: only
            // `output()`'s parameter list constrains ordering (§8.3).
            bool feedthrough = false;
            if (out)
                for (const std::string& q : out->params)
                    if (q == port.name) feedthrough = true;

            std::string from, label;
            if (src.kind == InputSource::Kind::Boundary) {
                from = "se_in_" + src.boundary.substr(std::strlen("sig.in."));
                label = port.name;
            } else if (src.kind == InputSource::Kind::Param ||
                       src.kind == InputSource::Kind::Const) {
                // §6.9.2 — a constant source has no producing leaf, so there is
                // no node in the graph to draw the edge from. It gets a
                // terminal of its own, declared inline the way Mermaid allows,
                // rather than being attributed to whichever leaf happens to sit
                // at `producer`'s default index.
                from = "se_k" + std::to_string(konst++) + "([" +
                       lab(src.kind == InputSource::Kind::Const ? dbl(src.value)
                                                                : render_expr(src.expr)) +
                       "])";
                label = port.name;
            } else {
                from = id(m_.leaves[src.producer].ident);
                label = src.port;
            }

            // A conversion folded to a constant at elaboration (§4.2) is
            // invisible in the generated code, so this is the only place it
            // shows: the two declared units, in the direction they convert.
            std::string ty = type_of(port.type);
            if (src.converts() && src.kind == InputSource::Kind::Leaf) {
                const Leaf& prod = m_.leaves[src.producer];
                for (const Field& f : prod.ports_out)
                    if (f.name == src.port && !f.type.is_record)
                        ty = f.type.unit.str() + " to " + ty;
            }
            t << "    " << from << (feedthrough ? " -->|" : " -.->|") << lab(label + " (" + ty + ")")
              << "| " << id(leaf.ident) << "\n";
        }
    }
    for (std::size_t k = 0; k < outs.size(); ++k) {
        auto it = leaf_by_ident.find(outs[k].first);
        if (it == leaf_by_ident.end()) continue;
        t << "    " << id(outs[k].first) << " --> se_out" << k << "\n";
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
            if (!s.continuous) continue;
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
    t << "\n[state.discrete]          # Sim::Impl::dis; offsets from Sim::Impl::discrete_map()\n";
    const std::vector<SlotRow> dis = discrete_rows();
    for (const SlotRow& r : dis)
        t << col("", 10) << col(r[0], 24) << col(r[3], 12) << r[2] << "\n";
    if (dis.empty()) t << "          (none)\n";

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
        t << col("", 10) << col(r[0], 24) << col(r[3], 12) << r[2] << "\n";

    t << "\n[boundary.in]             # root inputs the host must drive, through Sim::inputs()\n";
    for (const BoundaryIn& b : m_.boundary_in)
        t << col("", 10) << col(b.path, 24)
          << (b.type.is_record ? b.type.record->fq : b.type.unit.str()) << "\n";
    if (m_.boundary_in.empty()) t << "          (none)\n";

    t << "\n[boundary.out]            # root outputs the host may read\n";
    for (const BoundaryOut& b : m_.boundary_out)
        t << col("", 10) << col(b.path, 24) << b.type.unit.str() << "\n";
    if (m_.boundary_out.empty()) t << "          (none)\n";

    // §15.6 — the configuration schema (§14.1): every path a settings source may
    // bind, the unit it is checked against, and the value elaboration produced.
    // A derived setting also shows what it is derived FROM, path-addressed, so
    // the file says which knobs move which values without the reader having to
    // open the model.
    t << "\n[settings]                # configurable; value is the elaborated default\n";
    for (const SettingSlot& s : m_.settings) {
        t << col("", 10) << col(s.path, 24) << col(s.unit.str(), 12) << "= "
          << literal(s.value, s.scalar);
        if (!s.constant) {
            std::string e;
            for (const ExprTok& tok : s.expr)
                e += tok.kind == ExprTok::Kind::Param
                         ? (tok.owner.empty() ? tok.text : tok.owner + "." + tok.text)
                         : tok.text;
            t << "   <- " << e;
        }
        t << "\n";
    }

    // Every node that declares `rate`, not only the ones elaboration decimated:
    // the decimation is derived at configuration (§9.2), so a node sitting at
    // the base step today is still a node whose schedule an override can move.
    t << "\n[decimation]              # derived from `rate` at configuration\n";
    for (const Leaf& leaf : m_.leaves) {
        if (!dynamic_rate(leaf)) continue;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%g Hz", 1.0 / (m_.step * leaf.decimation));
        t << col("", 10) << col(leaf.path, 24) << col(buf, 12) << "= every "
          << leaf.decimation << (leaf.decimation == 1 ? " tick" : " ticks") << "\n";
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
              << (f.type.is_record ? f.type.record->fq : f.type.unit.str()) << "\n";
    }

    if (m_.record.present) {
        t << "\n[recorded]                # columns of " << m_.record.file << "\n";
        t << col("", 10) << col("time", 24) << "s\n";
        for (const RecordedSignal& s : m_.record.signals)
            t << col("", 10) << col(s.path, 24) << s.unit.str() << "\n";
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
        t << "#                  [-Sanitize | -NoSanitize] [-StaticCrt]\n";
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
        t << "param(\n";
        t << "    [ValidateSet('run', 'exe', 'lib', 'test', 'clean', 'all')]\n";
        t << "    [string]$Mode = 'run',\n";
        t << "    [ValidateSet('release', 'debug')]\n";
        t << "    [string]$Config = '',\n";
        t << "    [switch]$Sanitize,\n";
        t << "    [switch]$NoSanitize,\n";
        t << "    [switch]$StaticCrt\n";
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
        t << "        & \".\\$out\\bin\\$name.exe\"\n";
        t << "        $code = $LASTEXITCODE\n";
        t << "    } elseif ($Mode -eq 'test') {\n";
        t << "        & \".\\$out\\bin\\$name.exe\"\n";
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
    t << "#                 [--sanitize | --no-sanitize] [--static-crt]\n";
    t << "#\n";
    t << "#  The same modes as build.ps1, into build/<config>/{bin,lib,include};\n";
    t << "#  test sanitizes (address, undefined) unless --no-sanitize, or unless the\n";
    t << "#  compiler cannot. CXX picks the compiler; the default is c++.\n";
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
    t << "        *) echo \"build: unknown argument '$1'\" >&2; exit 2 ;;\n";
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
    t << "    run) build_exe; exec \"./$out/bin/$name\" ;;\n";
    t << "    test)\n";
    t << "        build_exe\n";
    t << "        how=; if [ $asan = 1 ]; then how=\" (AddressSanitizer)\"; fi\n";
    t << "        if \"./$out/bin/$name\"; then echo \"test: passed$how\"; "
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
    if (opt.write_topology)
        ok = write_file(diag, path(stem + ".topology.md"),
                        "```mermaid\n" + e.topology() + "```\n", opt.quiet) && ok;
    if (opt.separate_runtime)
        ok = write_file(diag, path("se_runtime.hpp"), se_runtime_hpp(), opt.quiet) && ok;
    if (opt.write_build && has_main) {
        const std::string exe_main = unity ? opt.unity_main : "main.cpp";
        ok = write_file(diag, path("build.ps1"), e.build_script(Script::Ps1, exe_main, unity),
                        opt.quiet) && ok;
        ok = write_file(diag, path("build.sh"), e.build_script(Script::Sh, exe_main, unity),
                        opt.quiet) && ok;
    }
    return ok;
}

}  // namespace se
