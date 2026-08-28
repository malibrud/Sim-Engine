// ─────────────────────────────────────────────────────────────────────────────
//  Stage 6 — emission.  SPECIFICATION.md §11, §12.4, §15.
// ─────────────────────────────────────────────────────────────────────────────
#include "emit.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
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

class Emitter {
public:
    Emitter(const Model& m, const std::string& gen_name) : m_(m), gen_(gen_name) {}

    std::string hpp();
    std::string main_cpp(const std::string& header_name);
    std::string manifest();
    std::string topology() const;
    std::string build_script(bool windows, const std::string& header_name);

private:
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
    // True when the leaf's decimation is a runtime value rather than 1.
    bool dynamic_rate(const Leaf& leaf) const;
    std::string decim_of(const Leaf& leaf) const;

    // One row per scalar slot of a block: { model path, C++ member path,
    // scalar type, unit }. Built once and used by BOTH the manifest and the
    // offsetof table, so the two cannot drift apart.
    using SlotRow = std::array<std::string, 4>;
    std::vector<SlotRow> signal_rows() const;
    std::vector<SlotRow> discrete_rows() const;
    std::vector<SlotRow> array_state_rows(bool continuous) const;

    const Model& m_;
    std::string gen_;
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
// nothing at all: the leaf already holds a trace-only `log` member, so their
// bodies are verbatim from brace to brace.
void Emitter::preamble(const NodeInfo& node, ast::Method::Which which, const char* ind) {
    using W = ast::Method::Which;
    const bool mutating = which == W::Init || which == W::OnStep || which == W::Final;
    if (!mutating) return;

    if (which == W::Init || which == W::OnStep)
        o_ << ind << "se_rt::Control& sim = se_rt::control();"
           << "   // sim.stop / sim.abort, §10.2\n";
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

void Emitter::records() {
    // Loading is need-driven, so `record_order` holds exactly the records this
    // model reaches. It is post-order over the field graph, which means a
    // contained record always precedes its container and the emitted sequence
    // compiles without any forward declarations.
    for (const RecordInfo* r : m_.program->record_order) {
        const std::string title = "// -- type " + r->fq + " ";
        o_ << title << std::string(title.size() < 78 ? 78 - title.size() : 1, '-') << "\n";
        const std::size_t colon = r->cpp_name.rfind("::");
        const std::string ns = colon == std::string::npos ? "" : r->cpp_name.substr(0, colon);
        const std::string base =
            colon == std::string::npos ? r->cpp_name : r->cpp_name.substr(colon + 2);
        if (!ns.empty()) o_ << "namespace " << ns << " {\n";
        std::size_t wt = 0, wd = 0;
        for (const Field& f : r->fields) {
            wt = std::max(wt, cpp_type(f.type).size());
            wd = std::max(wd, f.name.size() + 1);
        }
        o_ << "struct " << base << " {\n";
        for (const Field& f : r->fields) {
            o_ << "    " << pad(cpp_type(f.type), wt) << " " << pad(f.name + ";", wd);
            if (!f.type.is_record) o_ << "   " << unit_comment(f.type.unit);
            o_ << "\n";
        }
        o_ << "};\n";
        if (!ns.empty()) o_ << "}  // namespace " << ns << "\n";
        o_ << "\n";
    }
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
    mem.push_back({"Param", "param;", "// written by Sim::resolve_settings"});
    mem.push_back({"const char* const", "se_path_;", "// model path, for the log tag"});
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
    const InputSource& s = it->second;
    // §6.9.2 — a setting source is read where the value is read: the setting is
    // a live member (§6.2b), so an override at configuration reaches the wire
    // without anything being folded, and no `Out` storage is invented.
    std::string expr = s.kind == InputSource::Kind::Boundary ? s.boundary
                       : s.kind == InputSource::Kind::Param
                           ? render_expr(s.expr)
                           : "sig." + m_.leaves[s.producer].ident + "." + s.port;
    if (!s.converts()) return expr;

    // A record converts field by field, so it is rebuilt as a brace initialiser
    // over the same depth-first order the elaborator flattened (§5.2). Walking
    // `ports_in` rather than the definition's ports is what gives the concrete
    // record type after unit substitution (§6.2a).
    if (!s.field_convs.empty()) {
        const Field* pf = nullptr;
        for (const Field& f : leaf.ports_in)
            if (f.name == port) { pf = &f; break; }
        if (pf && pf->type.is_record) {
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
            return build(pf->type, expr);
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

// A slow node whose `output()` reads nothing can be called on every tick: it
// is a function of state alone, and state only moves at its sample instants,
// so the value is identical and the branch is pure cost. A slow node WITH
// feedthrough must be gated, or ZOH would be lost.
bool Emitter::needs_tick_gate(const Leaf& leaf) const {
    if (!dynamic_rate(leaf)) return false;
    const ast::Method* out = leaf.node->method(ast::Method::Which::Output);
    return out && !out->params.empty();
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

// ═════════════════════════════════════════════════════════════════════════════
//  The Sim class
// ═════════════════════════════════════════════════════════════════════════════

void Emitter::sim_members() {
    const double steps = m_.step > 0.0 ? m_.duration / m_.step : 0.0;

    o_ << "    // ---- host API " << std::string(58, '-') << "\n";
    o_ << "    //  A run is `init(); while (!done()) tick(); finish();` — which is\n";
    o_ << "    //  exactly what run() does, so a host driving the boundary and the\n";
    o_ << "    //  generated batch driver use the same calls.\n";
    o_ << "    //  Construction leaves every setting at the value elaboration computed\n";
    o_ << "    //  for it (§6.2b step 1). To change one: load_settings(), then\n";
    o_ << "    //  resolve_settings() to re-run everything derived from it, then init().\n";
    if (!m_.dynamic_states) {
        o_ << "    Sim() { bind(); resolve_settings(); }\n";
    } else {
        // §15.5a — the block sizes are not known until configuration, so
        // the constructor cannot bind what does not exist yet. It configures;
        // the host then allocates and hands the buffers in.
        o_ << "    //  This model has array-shaped states (§6.4a), so its block sizes\n";
        o_ << "    //  are a CONFIGURATION result and the buffers are the host's\n";
        o_ << "    //  (§15.5a):\n";
        o_ << "    //\n";
        o_ << "    //      Sim sim;                       // configures\n";
        o_ << "    //      std::vector<double> x(sim.state_size()), xd(x.size());\n";
        o_ << "    //      sim.bind_state(x.data(), xd.data(), x.size());\n";
        if (m_.n_disc_arr_slots) {
            o_ << "    //      std::vector<double> d(sim.discrete_array_size()), "
                  "n(d.size());\n";
            o_ << "    //      sim.bind_discrete_array(d.data(), n.data(), d.size());\n";
        }
        o_ << "    //      sim.init();\n";
        o_ << "    //\n";
        o_ << "    //  run() and run_main() do all of that themselves.\n";
        o_ << "    Sim() { bind(); configure(); }\n";
        o_ << "    void configure();                        // §6.2b steps 1-4\n";
        o_ << "    std::size_t state_size() const { return n_states; }\n";
        if (m_.n_disc_arr_slots)
            o_ << "    std::size_t discrete_array_size() const { return n_dstates; }\n";
        o_ << "    void bind_state(double* px, double* pxd, std::size_t n);\n";
        if (m_.n_disc_arr_slots)
            o_ << "    void bind_discrete_array(double* pd, double* pn, std::size_t n);\n";
    }
    o_ << "    void init();\n";
    o_ << "    void tick();                             // one base step (§9.6)\n";
    o_ << "    bool done() const { return se_done_; }\n";
    o_ << "    void finish();\n";
    o_ << "    //  Empty unless resolve_settings() rejected a value it was given\n";
    o_ << "    //  (§16.4). init() refuses to start a run while it is set.\n";
    o_ << "    const std::string& config_error() const { return se_cfg_error_; }\n";
    o_ << "    double time() const { return se_time_; }\n";
    o_ << "    std::uint64_t tick_index() const { return se_tick_; }\n";
    o_ << "    int run();                               // init + loop + finish\n";
    o_ << "    static int run_main();                   // run(), configured from the .sim\n\n";
    o_ << "    static constexpr double        step_seconds = " << dbl(m_.step) << ";\n";
    o_ << "    static constexpr std::uint64_t tick_count   = "
       << std::to_string(static_cast<long long>(steps + 0.5)) << ";\n\n";

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
                       << pad(leaf.path + "." + s.name, 24) << s.unit.str() << "\n";
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
                   << pad(p + (st.extent.present ? " " + st.extent.text : ""), 30)
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
    if (!m_.boundary_in.empty()) {
        o_ << "        struct In {\n";
        std::size_t wt = 0, wd = 0;
        for (const BoundaryIn& b : m_.boundary_in) {
            wt = std::max(wt, cpp_type(b.type).size());
            wd = std::max(wd, b.ident.size() + 3);
        }
        for (const BoundaryIn& b : m_.boundary_in)
            o_ << "            " << pad(cpp_type(b.type), wt) << " "
               << pad(b.ident + "{};", wd)
               << (b.type.is_record ? "" : "   " + unit_comment(b.type.unit)) << "\n";
        o_ << "        } in;   // the host writes these\n";
    }
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
                   << pad(p + " " + st.extent.text, 30) << st.unit.str() << "\n";
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
    o_ << "    //  Leaf instances. Their settings are NOT baked in here: they are\n";
    o_ << "    //  assigned by resolve_settings() (§6.2b), so a settings source can\n";
    o_ << "    //  move them in a binary that has already been compiled. Any unit\n";
    o_ << "    //  CONVERSION still folded at elaboration, which is why nothing here\n";
    o_ << "    //  needs conversion machinery.\n";
    for (const Leaf& leaf : m_.leaves) {
        const std::string path = quote(leaf.path.empty() ? "<root>" : leaf.path);
        o_ << "    " << pad(leaf.node->cpp_name, 28) << " " << pad(leaf.ident + "{{}, ", 18)
           << path << "};\n";
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
                   << (r.comment.empty() ? r.decl : pad(r.decl, 20) + r.comment)
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

    // Everything below is defined after the class, so the class body reads as
    // an interface and a data layout rather than as a wall of code.
    o_ << "\n    void resolve_settings();\n";
    o_ << "\n    void bind();\n";
    o_ << "    void apply_initial_conditions();\n";
    o_ << "    void output_pass(std::uint64_t k);\n";
    o_ << "    void next_tick(std::uint64_t k);\n";
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
    o_ << "\n// Settings (§6.2b). Re-runnable: call it after load_settings() and\n";
    o_ << "// every derived value follows, exactly as it does at elaboration.\n";
    o_ << "inline void Sim::resolve_settings() {\n";
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
    o_ << "inline void Sim::configure() {\n";
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

    o_ << "\ninline void Sim::bind_state(double* px, double* pxd, std::size_t n) {\n";
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
            } else {
                o_ << "    " << leaf.ident << ".state." << st.name << ".bind(x[" << off
                   << "]);\n";
                o_ << "    " << leaf.ident << "_der." << st.name << ".bind(xd[" << off
                   << "]);\n";
            }
        }
    o_ << "    se_bound_x_ = true;\n}\n";

    if (!m_.n_disc_arr_slots) return;
    o_ << "\ninline void Sim::bind_discrete_array(double* pd, double* pn, "
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
    o_ << "inline void Sim::apply_initial_conditions() {\n";
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
    o_ << "inline void Sim::bind() {\n";
    for (const Leaf& leaf : m_.leaves) {
        for (const StateSlot& s : leaf.states) {
            if (s.continuous) {
                if (m_.dynamic_states) continue;
                o_ << "    " << leaf.ident << ".state." << s.name << ".bind(x[" << s.slot
                   << "]);\n";
                o_ << "    " << leaf.ident << "_der." << s.name << ".bind(xd[" << s.slot
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
    o_ << "\ninline void Sim::final_all(const se_rt::RunContext& ctx) const {\n";
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
    o_ << "inline void Sim::output_pass(std::uint64_t k) {\n";
    for (std::size_t i : m_.order) {
        const Leaf& leaf = m_.leaves[i];
        const ast::Method* out = leaf.node->method(ast::Method::Which::Output);
        if (!out) continue;
        const std::string call = leaf.ident + ".output(" + in_braces(leaf, out) + ", sig." +
                                 leaf.ident + ");";
        if (needs_tick_gate(leaf))
            o_ << "    if (k % " << decim_of(leaf) << " == 0) " << call << "\n";
        else if (dynamic_rate(leaf))
            o_ << "    " << pad(call, 54) << " // " << leaf.decimation
               << ":1 by default, but a function of state alone\n";
        else
            o_ << "    " << call << "\n";
    }
    o_ << "}\n";
}

void Emitter::sim_derivatives() {
    o_ << "\n// The external solver's entry point: reads x, fills xd. Sim owns both,\n";
    o_ << "// so a solver with its own vector does one memcpy each way per call.\n";
    o_ << "inline void Sim::derivatives(double t) {\n    (void)t;\n";
    o_ << "    output_pass(se_tick_);\n";
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
    o_ << "inline void Sim::next_tick(std::uint64_t k) {\n    (void)k;\n";
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

// The mutating per-step hook: `var`, natives, logging. It carries no equation,
// which is why it is named for its moment rather than for an accessor (§8.1).
void Emitter::sim_on_step() {
    o_ << "\n// Once per accepted base step, at each node's sample instants. Runs\n";
    o_ << "// after next(), and like every other method in the tick it observes the\n";
    o_ << "// tick's ENTRY state: both `dis` and `x` advance at the end of tick().\n";
    o_ << "inline void Sim::on_step_tick(std::uint64_t k) {\n    (void)k;\n";
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
        o_ << "inline void Sim::integrate(double, double) {}   // no continuous states\n";
        return;
    }
    o_ << "inline void Sim::integrate(double t, double h) {\n";
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
        o_ << "inline void Sim::integrate(double, double) {}   // no continuous states\n";
        return;
    }
    o_ << "inline void Sim::integrate(double t, double h) {\n";
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
    o_ << "inline void Sim::record_open() {\n";
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

    o_ << "\ninline void Sim::record_row() {\n    se_rec_.row(se_time_, {";
    for (std::size_t i = 0; i < m_.record.signals.size(); ++i) {
        if (i) o_ << ",";
        o_ << "\n                       static_cast<double>(" << m_.record.signals[i].expr
           << ")";
    }
    o_ << "});\n}\n";
}

void Emitter::sim_run() {
    bool any_discrete = false;
    for (const Leaf& leaf : m_.leaves) any_discrete = any_discrete || leaf.has_discrete();

    // ── init ─────────────────────────────────────────────────────────────────
    o_ << "\ninline void Sim::init() {\n";
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
    o_ << "inline void Sim::tick() {\n";
    o_ << "    se_time_ = static_cast<double>(se_tick_) * step_seconds;\n";
    o_ << "    se_rt::sim_time() = se_time_;\n";
    o_ << "    output_pass(se_tick_);\n";
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
    if (any_discrete)
        o_ << "    dis = nxt;   // simultaneous update, whole model (§6.4)\n";
    o_ << "    integrate(se_time_, step_seconds);\n";
    if (m_.realtime && m_.sync_steps > 0)
        o_ << "    if ((se_tick_ + 1) % " << m_.sync_steps
           << " == 0) se_rt::realtime().frame_boundary();\n";
    o_ << "    ++se_tick_;\n";
    o_ << "}\n";

    // ── finish ───────────────────────────────────────────────────────────────
    o_ << "\ninline void Sim::finish() {\n";
    o_ << "    se_rt::control().complete();\n";
    o_ << "    const se_rt::RunContext ctx = se_rt::control().context(se_time_);\n";
    o_ << "    final_all(ctx);\n";
    if (m_.record.present) o_ << "    se_rec_.close();\n";
    if (m_.realtime) o_ << "    se_rt::realtime().summary();\n";
    o_ << "    se_rt::logs().write(se_rt::Level::Info, \"sim\", se_time_,\n";
    o_ << "                        se_rt::format(\"run {}: {}\", ctx.reason,\n";
    o_ << "                                      ctx.message.empty() ? \"-\" "
          ": ctx.message.c_str()));\n";
    o_ << "}\n";

    // ── run ──────────────────────────────────────────────────────────────────
    o_ << "\ninline int Sim::run() {\n";
    if (!m_.boundary_in.empty()) {
        // A batch run has nothing to drive the root boundary WITH — the sim file
        // has no syntax for it. Silence would make the resulting zeros look like
        // physics, so say it. A host that calls init/tick/finish never sees this.
        std::string names;
        for (const BoundaryIn& b : m_.boundary_in) {
            if (!names.empty()) names += ", ";
            names += b.path;
        }
        o_ << "    se_rt::logs().write(se_rt::Level::Warn, \"sim\", 0.0,\n";
        o_ << "                        \"batch run: nothing drives the root boundary, "
              "so it stays at zero: \"\n";
        o_ << "                        " << quote(names) << ");\n";
    }
    if (m_.dynamic_states) {
        // A batch run is a host like any other, so it does the §15.5a
        // handshake rather than being exempted from it. The vectors outlive the
        // loop below, which is the whole run.
        o_ << "    std::vector<double> se_x(state_size()), se_xd(state_size());\n";
        o_ << "    bind_state(se_x.data(), se_xd.data(), se_x.size());\n";
        if (m_.n_disc_arr_slots) {
            o_ << "    std::vector<double> se_d(discrete_array_size()), "
                  "se_dn(se_d.size());\n";
            o_ << "    bind_discrete_array(se_d.data(), se_dn.data(), se_d.size());\n";
        }
    }
    o_ << "    init();\n";
    o_ << "    while (!done()) tick();\n";
    o_ << "    finish();\n";
    o_ << "    const char* r = se_rt::reason_name(se_rt::control().reason());\n";
    o_ << "    return (std::strcmp(r, \"aborted\") == 0 || std::strcmp(r, \"error\") == 0)\n";
    o_ << "               ? 1\n               : 0;\n";
    o_ << "}\n";

    o_ << "\ninline int Sim::run_main() {\n";
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
    o_ << "    // A large state vector has no business on the stack.\n";
    o_ << "    static Sim instance;\n";
    o_ << "    return instance.run();\n";
    o_ << "}\n";
}

void Emitter::sim_class() {
    const std::string ns = m_.root_namespace;
    o_ << "// -- Sim: the whole model, flat " << std::string(47, '-') << "\n";
    if (!ns.empty()) {
        std::string cpp_ns;
        for (char c : ns) {
            if (c == '.') cpp_ns += "::";
            else cpp_ns += c;
        }
        o_ << "namespace " << cpp_ns << " {\n\n";
    }
    // The class body is the interface and the data layout; every body follows
    // it, so `struct Sim { … };` can be read in one screen.
    o_ << "struct Sim {\n";
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
    sim_on_step();
    sim_integrate();
    sim_record();
    sim_run();
    sim_maps();
    if (!ns.empty()) {
        std::string cpp_ns;
        for (char c : ns) {
            if (c == '.') cpp_ns += "::";
            else cpp_ns += c;
        }
        o_ << "\n}  // namespace " << cpp_ns << "\n";
    }
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
        o_ << "\ninline const se_rt::Slot* Sim::" << fn << "(std::size_t& count) {\n";
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
            o_ << "        {" << pad(quote(r[0]) + ",", wp + 1) << " offsetof(Sim::" << block
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
        o_ << "\ninline const se_rt::Slot* Sim::" << fn
           << "(std::size_t& count) const {\n";
        o_ << "    count = " << store << ".size();\n";
        o_ << "    return " << store << ".empty() ? nullptr : " << store << ".data();\n}\n";
    };
    emit_dyn("state_map", "se_state_slots_");
    if (m_.n_disc_arr_slots) emit_dyn("discrete_array_map", "se_dstate_slots_");
}

// ─── The whole file ──────────────────────────────────────────────────────────

std::string Emitter::hpp() {
    o_ << "// " << std::string(76, '-') << "\n";
    o_ << "//  GENERATED — DO NOT EDIT.\n";
    o_ << "//  sim:  " << m_.name << "\n";
    o_ << "//  root: " << m_.root->fq << "\n";
    o_ << "//  base step: " << dbl(m_.step) << " s";
    if (m_.step > 0.0) o_ << "  (" << dbl(1.0 / m_.step) << " Hz)";
    o_ << "\n//\n";
    o_ << "//  Reading this file: everything is generated EXCEPT the body of each\n";
    o_ << "//  lifecycle method, which is copied byte for byte from the `.se` source\n";
    o_ << "//  named in the `#line` directive above it. The pure methods have\n";
    o_ << "//  no generated lines at all — brace to brace, they are the model author's.\n";
    o_ << "// " << std::string(76, '-') << "\n";
    o_ << "#pragma once\n\n";
    o_ << "#include \"se_runtime.hpp\"\n#include \"state_ref.hpp\"\n\n";
    o_ << "#include <array>\n#include <cstddef>\n#include <cstdint>\n#include <cstring>\n";
    // §6.5 / §15.5a — the two places the lowering owns storage: an array
    // var, and the solver scratch plus slot rows of a dynamic model. Both are
    // sized once at configuration and neither is a published block.
    bool any_array_var = false;
    for (const Leaf& leaf : m_.leaves) any_array_var = any_array_var || !leaf.array_vars.empty();
    if (m_.dynamic_states || any_array_var)
        o_ << "#include <string>\n#include <vector>\n";
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
    o_ << "#pragma warning(disable : 4459)   // local `sim` hides the namespace\n";
    o_ << "#elif defined(__GNUC__)\n";
    o_ << "#pragma GCC diagnostic ignored \"-Wunused-parameter\"\n";
    o_ << "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n";
    o_ << "#pragma GCC diagnostic ignored \"-Wshadow\"\n";
    o_ << "#endif\n\n";
    o_ << "using sim::state_ref;\nusing sim::value_ref;\n";
    if (m_.dynamic_states) o_ << "using sim::state_arr;\n";
    o_ << "\n";

    declarations();
    records();

    std::set<const NodeInfo*> done;
    for (const Leaf& leaf : m_.leaves)
        if (done.insert(leaf.node).second) leaf_class(*leaf.node);

    sim_class();
    return o_.str();
}

std::string Emitter::main_cpp(const std::string& header_name) {
    std::string cpp_ns;
    for (char c : m_.root_namespace) {
        if (c == '.') cpp_ns += "::";
        else cpp_ns += c;
    }
    Out t;
    t << "// " << std::string(76, '-') << "\n";
    t << "//  GENERATED — DO NOT EDIT.\n";
    t << "//  The batch driver for `" << m_.name << "`, and nothing else. It is a\n";
    t << "//  separate translation unit so that a host program can include\n";
    t << "//  " << header_name << " and supply its own main():\n";
    t << "//\n";
    t << "//      " << (cpp_ns.empty() ? "" : cpp_ns + "::") << "Sim sim;\n";
    t << "//      sim.init();\n";
    t << "//      while (!sim.done()) {\n";
    if (!m_.boundary_in.empty()) {
        for (const BoundaryIn& b : m_.boundary_in)
            t << "//          sim.sig.in." << pad(b.ident + " = ...;", 24) << "// "
              << (b.type.is_record ? b.type.record->fq : b.type.unit.str()) << "\n";
    }
    t << "//          sim.tick();\n";
    t << "//      }\n";
    t << "//      sim.finish();\n";
    t << "//\n";
    t << "//  Pass --no-main to `sec --emit` to suppress this file entirely.\n";
    t << "// " << std::string(76, '-') << "\n\n";
    t << "#include " << quote(header_name) << "\n\n";
    t << "int main() {\n    return " << (cpp_ns.empty() ? "" : cpp_ns + "::")
      << "Sim::run_main();\n}\n";
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
    t << "#  Unit manifest for: " << gen_ << "\n";
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
             "#  Sim::state_map() once configure() has run (§15.6).\n";
    for (const Leaf& leaf : m_.leaves)
        for (const StateSlot& s : leaf.states) {
            if (!s.continuous) continue;
            const std::string path = leaf.path.empty() ? s.name : leaf.path + "." + s.name;
            if (!m_.dynamic_states) {
                t << pad("x[" + std::to_string(s.slot) + "]", 10) << pad(path, 24)
                  << s.unit.str() << "\n";
                t << pad("xd[" + std::to_string(s.slot) + "]", 10) << pad(path + "'", 24)
                  << s.der_unit.str() << "\n";
                continue;
            }
            const std::string ext = s.extent.present ? " " + s.extent.text : "";
            t << pad("x[?]", 10) << pad(path + ext, 24)
              << pad(s.unit.str(), 12) << "dynamic\n";
            t << pad("xd[?]", 10) << pad(path + "'" + ext, 24)
              << pad(s.der_unit.str(), 12) << "dynamic\n";
        }
    if (m_.n_cont_slots == 0 && m_.n_states == 0) t << "          (none)\n";

    // §15.6 — `sec` cannot write byte offsets: padding is the C++ compiler's
    // business, and a guessed table would be worse than none. So the manifest
    // names, units and types the slots, and the generated header builds the
    // offsets with offsetof(). Both come from the same row list, so they cannot
    // disagree.
    t << "\n[state.discrete]          # Sim::dis; offsets from Sim::discrete_map()\n";
    const std::vector<SlotRow> dis = discrete_rows();
    for (const SlotRow& r : dis)
        t << pad("", 10) << pad(r[0], 24) << pad(r[3], 12) << r[2] << "\n";
    if (dis.empty()) t << "          (none)\n";

    // §15.5a — the fourth block. An array-shaped discrete state cannot live
    // in `dis` without destroying its offsetof property, so it gets a flat
    // `double` block of its own and the same address-size-table treatment as x.
    if (m_.n_disc_arr_slots) {
        t << "\n[state.discrete.array]    # Sim::dis_arr; rows from "
             "Sim::discrete_array_map()\n";
        for (const SlotRow& r : array_state_rows(false))
            t << pad("dis_arr[?]", 12) << pad(r[0] + " " + r[1], 22) << pad(r[3], 12)
              << "dynamic\n";
    }

    t << "\n[signals]                 # Sim::sig; offsets from Sim::signal_map()\n";
    for (const SlotRow& r : signal_rows())
        t << pad("", 10) << pad(r[0], 24) << pad(r[3], 12) << r[2] << "\n";

    t << "\n[boundary.in]             # root inputs the host must drive, as sig.in.*\n";
    for (const BoundaryIn& b : m_.boundary_in)
        t << pad("", 10) << pad(b.path, 24)
          << (b.type.is_record ? b.type.record->fq : b.type.unit.str()) << "\n";
    if (m_.boundary_in.empty()) t << "          (none)\n";

    t << "\n[boundary.out]            # root outputs the host may read\n";
    for (const BoundaryOut& b : m_.boundary_out)
        t << pad("", 10) << pad(b.path, 24) << b.type.unit.str() << "\n";
    if (m_.boundary_out.empty()) t << "          (none)\n";

    // §15.6 — the configuration schema (§14.1): every path a settings source may
    // bind, the unit it is checked against, and the value elaboration produced.
    // A derived setting also shows what it is derived FROM, path-addressed, so
    // the file says which knobs move which values without the reader having to
    // open the model.
    t << "\n[settings]                # configurable; value is the elaborated default\n";
    for (const SettingSlot& s : m_.settings) {
        t << pad("", 10) << pad(s.path, 24) << pad(s.unit.str(), 12) << "= "
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
        t << pad("", 10) << pad(leaf.path, 24) << pad(buf, 12) << "= every "
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
            t << pad(r->fq, 34) << "units { " << ps << " }\n";
        }
        for (const Field& f : r->fields)
            t << pad(r->fq + "." + f.name, 34)
              << (f.type.is_record ? f.type.record->fq : f.type.unit.str()) << "\n";
    }

    if (m_.record.present) {
        t << "\n[recorded]                # columns of " << m_.record.file << "\n";
        t << pad("", 10) << pad("time", 24) << "s\n";
        for (const RecordedSignal& s : m_.record.signals)
            t << pad("", 10) << pad(s.path, 24) << s.unit.str() << "\n";
    }
    return t.str();
}

// ─── The reference build (§12.4) ─────────────────────────────────────────────

std::string Emitter::build_script(bool windows, const std::string& main_name) {
    const char* platform = windows ? "windows" : "linux";
    Out t;
    if (windows) {
        t << "@echo off\r\n";
        t << "rem " << std::string(74, '-') << "\r\n";
        t << "rem  GENERATED reference build for " << gen_ << ".\r\n";
        t << "rem\r\n";
        t << "rem  A FULL build only: it never tracks changes and never builds\r\n";
        t << "rem  incrementally. That is not an omission. The unity build is the\r\n";
        t << "rem  intended model, so there are no object files and \"incremental\" is a\r\n";
        t << "rem  category that does not exist rather than a feature skipped.\r\n";
        t << "rem " << std::string(74, '-') << "\r\n";
        t << "setlocal\r\n";
    } else {
        t << "#!/bin/sh\n";
        t << "# " << std::string(74, '-') << "\n";
        t << "#  GENERATED reference build for " << gen_ << ".\n";
        t << "#  A full build only — the unity build is the intended model (§12.4).\n";
        t << "# " << std::string(74, '-') << "\n";
        t << "set -e\n";
    }

    std::string includes, defines, cflags, links;
    for (const ast::BuildStmt& b : m_.program->build) {
        if (!b.when.empty() && b.when != platform) continue;
        if (b.primitive == "include_dir")
            includes += windows ? " /I\"" + b.argument + "\"" : " -I'" + b.argument + "'";
        else if (b.primitive == "define")
            defines += windows ? " /D" + b.argument : " -D" + b.argument;
        else if (b.primitive == "cflag")
            cflags += " " + b.argument;
        else if (b.primitive == "link")
            links += windows ? " " + b.argument + ".lib" : " -l" + b.argument;
    }

    const std::string exe = m_.name.empty() ? "sim" : m_.name;
    if (windows) {
        t << "\r\ncl /nologo /std:c++17 /EHsc /W4 /permissive- /utf-8 /O2 /MD"
             " /D_CRT_SECURE_NO_WARNINGS"
          << cflags << defines
          << includes << " /Fe:" << exe << ".exe " << main_name;
        if (!links.empty()) t << " /link" << links;
        t << "\r\nif errorlevel 1 exit /b 1\r\n";
        t << "echo build: " << exe << ".exe\r\n";
        t << "endlocal\r\n";
    } else {
        t << "\n${CXX:-c++} -std=c++17 -O2 -Wall -Wextra" << cflags << defines << includes
          << " -o " << exe << " " << gen_ << links << "\n";
        t << "echo \"build: " << exe << "\"\n";
    }
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
    // The model is a header so a host can include it; the batch driver is its
    // own translation unit so it can be left out (`--no-main`) without editing
    // a file stamped DO NOT EDIT.
    const std::string header_name = opt.stem + ".generated.hpp";
    const std::string main_name = opt.stem + ".main.cpp";
    const std::string dir = opt.out_dir.empty() ? "." : opt.out_dir;
    auto path = [&](const std::string& name) {
        return dir == "." ? name : dir + "/" + name;
    };

    Emitter e(m, header_name);
    bool ok = true;
    ok = write_file(diag, path(header_name), e.hpp(), opt.quiet) && ok;
    if (opt.write_main)
        ok = write_file(diag, path(main_name), e.main_cpp(header_name), opt.quiet) && ok;
    ok = write_file(diag, path(opt.stem + ".units.txt"), e.manifest(), opt.quiet) && ok;
    if (opt.write_topology)
        ok = write_file(diag, path(opt.stem + ".topology.mmd"), e.topology(), opt.quiet) && ok;
    if (opt.write_runtime) {
        ok = write_file(diag, path("state_ref.hpp"), state_ref_hpp(), opt.quiet) && ok;
        ok = write_file(diag, path("se_runtime.hpp"), se_runtime_hpp(), opt.quiet) && ok;
    }
    if (opt.write_build && opt.write_main) {
        ok = write_file(diag, path("build.bat"), e.build_script(true, main_name), opt.quiet) && ok;
        ok = write_file(diag, path("build.sh"), e.build_script(false, main_name), opt.quiet) && ok;
    }
    return ok;
}

}  // namespace se
