// ─────────────────────────────────────────────────────────────────────────────
//  Node tests.  SPECIFICATION.md §6.13.  See testgen.hpp.
// ─────────────────────────────────────────────────────────────────────────────
#include "testgen.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "ast.hpp"
#include "elaborate.hpp"
#include "emit.hpp"
#include "parser.hpp"
#include "resolve.hpp"
#include "schedule.hpp"
#include "units.hpp"

namespace se {
namespace {

// ─── AST copies ──────────────────────────────────────────────────────────────
// The synthesised sim file owns its expressions, so the preamble's and the rows'
// are copied into it rather than borrowed. Locations travel with the copy, which
// is what makes a unit error in a cell point at the cell.

ast::UnitPtr clone_unit(const ast::UnitExpr* u) {
    if (!u) return nullptr;
    auto c = std::make_unique<ast::UnitExpr>();
    c->kind = u->kind;
    c->symbol = u->symbol;
    c->lhs = clone_unit(u->lhs.get());
    c->rhs = clone_unit(u->rhs.get());
    c->base = clone_unit(u->base.get());
    c->exp_num = u->exp_num;
    c->exp_den = u->exp_den;
    c->loc = u->loc;
    return c;
}

ast::ExprPtr clone_expr(const ast::Expr* e) {
    if (!e) return nullptr;
    auto c = std::make_unique<ast::Expr>();
    c->kind = e->kind;
    c->loc = e->loc;
    c->number = e->number;
    c->text = e->text;
    c->unit = clone_unit(e->unit.get());
    c->op = e->op;
    c->lhs = clone_expr(e->lhs.get());
    c->rhs = clone_expr(e->rhs.get());
    for (const ast::ExprPtr& a : e->args) c->args.push_back(clone_expr(a.get()));
    c->field_names = e->field_names;
    c->field_locs = e->field_locs;
    return c;
}

// ─── C++ spelling ────────────────────────────────────────────────────────────

std::string cpp_num(double v) {
    if (std::isnan(v)) return "std::numeric_limits<double>::quiet_NaN()";
    if (std::isinf(v)) return v > 0 ? "HUGE_VAL" : "-HUGE_VAL";
    char b[48];
    std::snprintf(b, sizeof b, "%.17g", v);
    std::string s = b;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

std::string cpp_str(const std::string& s) {
    std::string o = "\"";
    for (char ch : s) {
        if (ch == '"' || ch == '\\') o += '\\';
        if (ch == '\n') {
            o += "\\n";
            continue;
        }
        o += ch;
    }
    return o + "\"";
}

std::string cpp_scalar(const std::string& scalar, double v) {
    if (scalar == "int") return "static_cast<int>(" + cpp_num(v) + ")";
    if (scalar == "bool") return v != 0.0 ? "true" : "false";
    if (scalar == "float") return "static_cast<float>(" + cpp_num(v) + ")";
    return cpp_num(v);
}

std::string dots_to_colons(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == '.') o += "::";
        else o += ch;
    }
    return o;
}

// ─── Columns ─────────────────────────────────────────────────────────────────

// One header column, resolved against the node under test.
struct Col {
    const ast::TestColumn* ast = nullptr;
    std::string ns;              // "in", "out", …, or "step"
    std::string key;             // the member path under `ns`: "q" or "q.w"
    Type type;                   // unit-substituted; may be a record
};

// Which method writes each expected namespace, and so which one a set needs.
ast::Method::Which writer_of(const std::string& ns) {
    if (ns == "out") return ast::Method::Which::Output;
    if (ns == "der") return ast::Method::Which::Derivative;
    if (ns == "next") return ast::Method::Which::Next;
    if (ns == "adjust") return ast::Method::Which::Adjust;
    return ast::Method::Which::Init;   // var
}

class TestGen {
public:
    TestGen(Diagnostics& diag, const std::vector<std::string>& roots, const TestGenOptions& opt)
        : diag_(diag), roots_(roots), opt_(opt) {}

    // Elaborates, emits and writes the test program for one node. False if
    // anything was reported.
    bool run(const std::string& fq, const Source& req_src, Loc req_loc);

private:
    bool bind_units(const ast::TestsSection& t);
    bool resolve_column(const ast::TestColumn& c, bool expected, Col& out);
    bool walk(Type t, const ast::TestColumn& c, std::size_t from, Type& out);
    void unsupported(Loc loc, const std::string& what);

    Diagnostics& diag_;
    const std::vector<std::string>& roots_;
    const TestGenOptions& opt_;

    const NodeInfo* info_ = nullptr;
    const Source* src_ = nullptr;
    const Leaf* leaf_ = nullptr;
    std::vector<Unit> binding_;
};

void TestGen::unsupported(Loc loc, const std::string& what) {
    // Not a language error, so it carries no SE code: the column is legal
    // §6.13, and this `sec` does not run it yet.
    diag_.error("", *src_, loc, "`sec --test` does not support " + what + " yet",
                "not implemented",
                {note("this is a limitation of the test runner, not of the test")});
}

bool TestGen::bind_units(const ast::TestsSection& t) {
    binding_.assign(info_->unit_params.size(), unit_one());
    std::vector<bool> seen(info_->unit_params.size(), false);
    bool ok = true;
    for (const ast::UnitBinding& ub : t.units) {
        std::size_t idx = info_->unit_params.size();
        for (std::size_t i = 0; i < info_->unit_params.size(); ++i)
            if (info_->unit_params[i] == ub.name) idx = i;
        if (idx == info_->unit_params.size()) {
            diag_.error("SE0419", *src_, ub.loc,
                        "`" + info_->def->name + "` has no unit parameter `" + ub.name + "`",
                        "unknown unit parameter");
            ok = false;
            continue;
        }
        UnitEval ue(*src_, diag_);
        Unit u;
        if (!ue.eval(ub.unit.get(), u)) {
            ok = false;
            continue;
        }
        binding_[idx] = u;
        seen[idx] = true;
    }
    for (std::size_t i = 0; i < seen.size(); ++i) {
        if (seen[i]) continue;
        diag_.error("SE0419", *src_, info_->def->sec_tests.loc,
                    "unit parameter `" + info_->unit_params[i] + "` is not bound for the tests",
                    "no binding in `units { }`",
                    {help("add `units { " + info_->unit_params[i] +
                          " = <unit>; }` to the tests preamble (\xc2\xa7""6.13)")});
        ok = false;
    }
    return ok;
}

bool TestGen::walk(Type t, const ast::TestColumn& c, std::size_t from, Type& out) {
    for (std::size_t i = from; i < c.segs.size(); ++i) {
        const std::string& seg = c.segs[i];
        const Field* hit = nullptr;
        if (t.is_record && t.record)
            for (const Field& f : t.record->fields)
                if (f.name == seg) hit = &f;
        if (!hit) {
            diag_.error("SE0360", *src_, c.loc,
                        "`" + c.str() + "` names no member of `" + info_->def->name + "`",
                        "`" + seg + "` is not a field here");
            return false;
        }
        t = field_type(t, *hit);
    }
    out = t;
    return true;
}

bool TestGen::resolve_column(const ast::TestColumn& c, bool expected, Col& out) {
    out.ast = &c;
    out.ns = c.is_step ? "step" : c.ns;

    // SE0361 — §6.10's writability column read from the outside: what a body
    // may only write is what a test may only check.
    const bool given_ns = c.is_step || c.ns == "param" || c.ns == "in" || c.ns == "state";
    if (given_ns == expected) {
        diag_.error("SE0361", *src_, c.loc,
                    "`" + c.str() + "` is on the wrong side of the operator",
                    expected ? "a given column, not a checked one"
                             : "a checked column, not a given one",
                    {note("given: param. in. state. step -- checked: out. der. next. adjust. "
                          "var. (\xc2\xa7""6.13)")});
        return false;
    }
    if (c.is_step) {
        unsupported(c.loc, "a `step` column (use the preamble's `step:`)");
        return false;
    }
    for (const std::string& s : c.segs) {
        if (s.find('[') != std::string::npos) {
            unsupported(c.loc, "an element column of an array state");
            return false;
        }
    }
    const std::string& head = c.segs.front();
    for (std::size_t i = 0; i < c.segs.size(); ++i) out.key += (i ? "." : "") + c.segs[i];

    auto none = [&]() {
        const std::string kind = c.ns == "in" ? "input" : c.ns == "out" ? "output"
                                 : c.ns == "param" ? "setting" : c.ns == "var" ? "var"
                                 : "state";
        diag_.error("SE0360", *src_, c.loc,
                    "`" + c.str() + "` names no member of `" + info_->def->name + "`",
                    "no " + kind + " named `" + head + "`");
        return false;
    };

    Type base;
    if (c.ns == "in" || c.ns == "out") {
        const std::vector<Field>& ports = c.ns == "in" ? leaf_->ports_in : leaf_->ports_out;
        const Field* f = nullptr;
        for (const Field& p : ports)
            if (p.name == head) f = &p;
        if (!f) return none();
        base = f->type;
    } else if (c.ns == "param") {
        const NodeInfo::SettingInfo* s = info_->setting(head);
        if (!s) return none();
        base = bind_type(s->type, binding_);
    } else if (c.ns == "var") {
        const Field* f = nullptr;
        for (const Field& v : info_->vars)
            if (v.name == head) f = &v;
        if (!f) return none();
        base = bind_type(f->type, binding_);
    } else {   // state der next adjust
        const StateSlot* st = nullptr;
        for (const StateSlot& s : leaf_->states)
            if (s.name == head) st = &s;
        if (!st) return none();
        if (c.ns == "der" && !st->continuous) {
            diag_.error("SE0360", *src_, c.loc,
                        "`" + head + "` is a discrete state and has no derivative",
                        "not continuous", {help("a discrete state advances through `next.`")});
            return false;
        }
        if (c.ns == "next" && st->continuous) {
            diag_.error("SE0360", *src_, c.loc,
                        "`" + head + "` is a continuous state and has no `next`",
                        "not discrete", {help("a continuous state is checked through `der.`")});
            return false;
        }
        base.unit = c.ns == "der" ? st->der_unit : st->unit;
        base.scalar = st->scalar;
    }
    return walk(base, c, 1, out.type);
}

bool TestGen::run(const std::string& fq, const Source& req_src, Loc req_loc) {
    Resolver resolver(diag_, roots_);
    Elaborator elab(diag_, resolver);

    info_ = resolver.resolve_root(fq, req_src, req_loc);
    if (!info_) return false;
    src_ = info_->file->src.get();
    const ast::NodeDef& def = *info_->def;
    const ast::TestsSection& tests = def.tests;
    const int errors_before = diag_.error_count();

    if (info_->composite) {
        diag_.error("SE0362", *src_, def.sec_tests.loc,
                    "`" + def.name + "` is a composite, and a test calls a leaf's methods",
                    "nothing here to invoke",
                    {note("a test fabricates one point and calls the pure methods on it; a "
                          "composite has none (\xc2\xa7""6.13)")});
        return false;
    }
    if (!bind_units(tests)) return false;

    // ── The synthesised model: the node under test as the root. ────────────
    ast::SimFile sim;
    sim.name = def.name + "_test";
    sim.loc = def.sec_tests.loc;

    ast::SimEntry root;
    root.kind = ast::SimEntry::Kind::Root;
    root.key = "root";
    root.key_loc = root.loc = def.sec_tests.loc;
    {
        std::string seg;
        for (char ch : fq + ".") {
            if (ch == '.') {
                root.root.segs.push_back(seg);
                seg.clear();
            } else {
                seg += ch;
            }
        }
        root.root.loc = def.sec_tests.loc;
    }
    for (const ast::Binding& b : tests.settings)
        root.bindings.push_back(ast::Binding{b.name, clone_expr(b.value.get()), b.loc});
    // A required setting the preamble does not bind still has to be bound for
    // the model to elaborate. A set that supplies it as a column pins it on
    // every row anyway, so its first row's value is as good as any.
    for (const NodeInfo::SettingInfo& s : info_->settings) {
        if (s.default_value) continue;
        bool bound = false;
        for (const ast::Binding& b : tests.settings) bound = bound || b.name == s.name;
        for (const ast::TestSet& set : tests.sets) {
            for (std::size_t i = 0; !bound && i < set.given.size(); ++i) {
                const ast::TestColumn& c = set.given[i];
                if (c.is_step || c.ns != "param" || c.segs.size() != 1 || c.segs[0] != s.name)
                    continue;
                if (set.rows.empty() || set.rows[0].cells.size() <= i) continue;
                root.bindings.push_back(
                    ast::Binding{s.name, clone_expr(set.rows[0].cells[i].get()), c.loc});
                bound = true;
            }
        }
    }
    sim.entries.push_back(std::move(root));

    ast::ExprPtr step;
    if (tests.has_step) {
        step = clone_expr(tests.step.get());
    } else {
        // Never read unless a rate-derived setting is, and that is SE0470 below.
        step = std::make_unique<ast::Expr>();
        step->kind = ast::Expr::Kind::Number;
        step->number = 1.0;
        step->text = "1";
        step->unit = std::make_unique<ast::UnitExpr>();
        step->unit->kind = ast::UnitExpr::Kind::Symbol;
        step->unit->symbol = "ms";
        step->loc = def.sec_tests.loc;
    }
    for (const char* key : {"step", "duration"}) {
        ast::SimEntry e;
        e.kind = ast::SimEntry::Kind::Value;
        e.key = key;
        e.key_loc = e.loc = tests.has_step ? tests.step_loc : def.sec_tests.loc;
        e.value = clone_expr(step.get());
        sim.entries.push_back(std::move(e));
    }

    elab.set_root_units(binding_);
    Model m;
    if (!elab.run(sim, *src_, ".", m)) return false;
    if (!schedule(diag_, m)) return false;
    if (!tests.has_step && elab.rate_reached()) {
        diag_.error("SE0470", *src_, def.sec_tests.loc,
                    "the tests reach a rate-derived setting, and nothing gives them a step",
                    "no `step:` in the preamble",
                    {note("a node test has no sim file, so nothing else establishes the "
                          "clock (\xc2\xa7""6.13)"),
                     help("add `step: <period>;` to the tests preamble")});
        return false;
    }
    if (m.dynamic_states) {
        unsupported(def.sec_tests.loc, "a node with array-shaped states");
        return false;
    }
    leaf_ = &m.leaves.front();

    // ── The sets. ──────────────────────────────────────────────────────────
    const std::string node_t = info_->cpp_name;
    const std::string sim_t = dots_to_colons(m.root_namespace) + "::Sim";
    const std::string ident = leaf_->ident;

    std::ostringstream body;
    for (const ast::TestSet& set : tests.sets) {
        std::vector<Col> given(set.given.size()), expected(set.expected.size());
        bool cols_ok = true;
        for (std::size_t i = 0; i < set.given.size(); ++i)
            cols_ok = resolve_column(set.given[i], false, given[i]) && cols_ok;
        for (std::size_t i = 0; i < set.expected.size(); ++i)
            cols_ok = resolve_column(set.expected[i], true, expected[i]) && cols_ok;
        if (!cols_ok) continue;

        // Which methods run is inferred from the expected columns (SE0362).
        bool call[5] = {false, false, false, false, false};   // out der next adj var
        const ast::Method::Which order[4] = {
            ast::Method::Which::Output, ast::Method::Which::Derivative,
            ast::Method::Which::Next, ast::Method::Which::Adjust};
        bool methods_ok = true;
        for (const Col& c : expected) {
            const ast::Method::Which w = writer_of(c.ns);
            if (c.ns == "var") continue;
            for (int k = 0; k < 4; ++k)
                if (order[k] == w) call[k] = true;
            if (!info_->method(w)) {
                const char* name = c.ns == "out" ? "output" : c.ns == "der" ? "derivative"
                                   : c.ns == "next" ? "next" : "adjust";
                diag_.error("SE0362", *src_, c.ast->loc,
                            "`" + c.ast->str() + "` is written by `" + name + "()`, which `" +
                                def.name + "` does not define",
                            "nothing writes this");
                methods_ok = false;
            }
        }
        if (!methods_ok) continue;

        body << "    {\n"
             << "        const char* set = " << cpp_str(set.claim) << ";\n"
             << "        int rows = 0, failed = 0;\n";

        for (const ast::TestRow& row : set.rows) {
            if (row.cells.size() != given.size() + expected.size()) continue;
            const std::string label = cpp_str(row.label);
            std::ostringstream pre, params, states, calls, checks;
            std::map<std::string, std::vector<std::pair<std::string, std::string>>> ins;
            bool row_ok = true;

            for (std::size_t i = 0; i < given.size(); ++i) {
                const Col& c = given[i];
                std::vector<Elaborator::CellLeaf> leaves;
                if (!elab.eval_cell(row.cells[i].get(), c.type, c.key, *src_, c.ast->loc,
                                    "column `" + c.ast->str() + "`", leaves)) {
                    row_ok = false;
                    continue;
                }
                for (const Elaborator::CellLeaf& lf : leaves) {
                    const std::string v = cpp_scalar(lf.scalar, lf.value);
                    if (c.ns == "param") {
                        std::size_t slot = m.settings.size();
                        for (std::size_t k = 0; k < m.settings.size(); ++k)
                            if (m.settings[k].owner.empty() && m.settings[k].name == lf.key)
                                slot = k;
                        if (slot == m.settings.size() || m.settings[slot].rate ||
                            m.settings[slot].structural) {
                            unsupported(c.ast->loc, "a column for `rate` or a structural "
                                                    "setting");
                            row_ok = false;
                            continue;
                        }
                        params << "            s.se_pin_[" << slot << "] = true;\n"
                               << "            s." << ident << ".param." << lf.key << " = " << v
                               << ";\n";
                    } else if (c.ns == "state") {
                        states << "            s." << ident << ".state." << lf.key << " = " << v
                               << ";\n";
                    } else {   // in
                        ins[c.ast->segs.front()].push_back({lf.key, v});
                    }
                }
            }

            const char* mname[4] = {"output", "derivative", "next", "adjust"};
            for (int k = 0; k < 4; ++k) {
                if (!call[k]) continue;
                const ast::Method* meth = info_->method(order[k]);
                const std::string in = std::string("in_") + mname[k];
                calls << "            " << node_t << "::In_" << mname[k] << " " << in
                      << "{};\n";
                for (const std::string& p : meth->params) {
                    auto it = ins.find(p);
                    if (it == ins.end()) continue;
                    for (const auto& kv : it->second)
                        calls << "            " << in << "." << kv.first << " = " << kv.second
                              << ";\n";
                }
                if (k == 0)
                    calls << "            " << node_t << "::Out out{};\n"
                          << "            n.output(" << in << ", out);\n";
                else if (k == 1)
                    calls << "            n.derivative(" << in << ", s." << ident << "_der);\n";
                else if (k == 2)
                    calls << "            n.next(" << in << ", s.nxt." << ident << ");\n";
                else
                    calls << "            n.adjust(" << in << ", s." << ident << "_adj);\n";
            }

            for (std::size_t j = 0; j < expected.size(); ++j) {
                const Col& c = expected[j];
                std::vector<Elaborator::CellLeaf> leaves;
                if (!elab.eval_cell(row.cells[given.size() + j].get(), c.type, c.key, *src_,
                                    c.ast->loc, "column `" + c.ast->str() + "`", leaves)) {
                    row_ok = false;
                    continue;
                }
                for (const Elaborator::CellLeaf& lf : leaves) {
                    std::string got;
                    if (c.ns == "out") got = "out." + lf.key;
                    else if (c.ns == "der") got = "s." + ident + "_der." + lf.key;
                    else if (c.ns == "next") got = "s.nxt." + ident + "." + lf.key;
                    else if (c.ns == "adjust") got = "n.state." + lf.key;
                    else got = "n.var." + lf.key;

                    double tol = 0.0;
                    if (set.approx && set.within) {
                        Type tt;
                        tt.unit = lf.unit;
                        tt.scalar = "double";
                        std::vector<Elaborator::CellLeaf> t;
                        if (!elab.eval_cell(set.within.get(), tt, lf.key, *src_,
                                            set.within->loc, "the tolerance", t) ||
                            t.empty()) {
                            row_ok = false;
                            continue;
                        }
                        tol = t.front().value;
                    }
                    const std::string col = c.ns + "." + lf.key;
                    checks << "            ok = se_check(" << label << ", " << cpp_str(col)
                           << ", static_cast<double>(" << got << "), " << cpp_num(lf.value)
                           << ", " << (set.approx ? "true" : "false") << ", " << cpp_num(tol)
                           << ", " << cpp_str(lf.unit.str()) << ") && ok;\n";
                }
            }
            if (!row_ok) continue;

            body << "        {   // " << row.label << "\n"
                 << "            ++rows;\n"
                 << "            auto sp = std::make_unique<" << sim_t << ">();\n"
                 << "            " << sim_t << "& s = *sp;\n"
                 << params.str()
                 << "            s.resolve_settings();\n"
                 << "            if (!s.config_error().empty()) {\n"
                 << "                std::printf(\"FAIL %s / %s: configuration: %s\\n\", set, "
                 << label << ", s.config_error().c_str());\n"
                 << "                ++failed;\n"
                 << "            } else {\n"
                 << "            s.apply_initial_conditions();\n"
                 << "            auto& n = s." << ident << ";\n"
                 << states.str();
            if (info_->method(ast::Method::Which::Init)) body << "            n.init();\n";
            body << calls.str()
                 << "            bool ok = true;\n"
                 << checks.str()
                 << "            if (!ok) ++failed;\n"
                 << "            }\n"
                 << "        }\n";
        }
        body << "        report(set, rows, failed);\n    }\n";
    }

    if (diag_.error_count() > errors_before) return false;

    // ── Emit the model, then replace its batch driver with the test program. ─
    EmitOptions eo;
    eo.out_dir = opt_.out_dir + "/" + def.name;
    eo.write_topology = false;
    eo.quiet = true;
    if (!emit(diag_, m, eo)) return false;

    std::ostringstream o;
    o << "// ----------------------------------------------------------------------------\n"
      << "//  GENERATED by `sec --test` -- DO NOT EDIT.\n"
      << "//  node: " << info_->fq << "\n"
      << "//  The tests section of " << info_->file->path << " (SPECIFICATION 6.13): each\n"
      << "//  row fabricates one (settings, states, inputs) point, invokes the node's\n"
      << "//  pure methods on it in the fixed order init, output, derivative, next,\n"
      << "//  adjust, and compares what came back.\n"
      << "// ----------------------------------------------------------------------------\n"
      << "#include \"Sim.generated.hpp\"\n\n"
      << "#include <cmath>\n#include <cstdint>\n#include <cstdio>\n#include <cstring>\n"
      << "#include <limits>\n#include <memory>\n\n"
      << "namespace {\n\n"
      << "const char* const kNode = " << cpp_str(info_->fq) << ";\n"
      << "int g_failed_sets = 0;\n\n"
      << "// `=` is bitwise (6.13): -0.0 is not 0.0, and a NaN equals only itself.\n"
      << "bool se_same(double a, double b) {\n"
      << "    std::uint64_t x = 0, y = 0;\n"
      << "    std::memcpy(&x, &a, sizeof x);\n"
      << "    std::memcpy(&y, &b, sizeof y);\n"
      << "    return x == y;\n"
      << "}\n\n"
      << "bool se_check(const char* row, const char* col, double got, double want, bool approx,\n"
      << "              double tol, const char* unit) {\n"
      << "    const bool ok = approx ? std::fabs(got - want) <= tol : se_same(got, want);\n"
      << "    if (!ok) {\n"
      << "        if (approx)\n"
      << "            std::printf(\"     %s: %s got %.17g, want %.17g (%s), |err| %.3g > %.3g\\n\",\n"
      << "                        row, col, got, want, unit, std::fabs(got - want), tol);\n"
      << "        else\n"
      << "            std::printf(\"     %s: %s got %.17g, want exactly %.17g (%s)\\n\",\n"
      << "                        row, col, got, want, unit);\n"
      << "    }\n"
      << "    return ok;\n"
      << "}\n\n"
      << "void report(const char* set, int rows, int failed) {\n"
      << "    if (failed) {\n"
      << "        ++g_failed_sets;\n"
      << "        std::printf(\"FAIL %s : \\\"%s\\\", %d of %d row(s)\\n\", kNode, set, failed, rows);\n"
      << "    } else {\n"
      << "        std::printf(\"ok   %s : \\\"%s\\\" (%d row%s)\\n\", kNode, set, rows,\n"
      << "                    rows == 1 ? \"\" : \"s\");\n"
      << "    }\n"
      << "}\n\n"
      << "}  // namespace\n\n"
      << "int main() {\n"
      << body.str()
      << "    // A contract a method reports is a failure of the node, whichever row\n"
      << "    // tripped it; the report itself was already written, with the path.\n"
      << "    if (se_rt::checks().any()) {\n"
      << "        std::printf(\"FAIL %s : a contract failed during the tests\\n\", kNode);\n"
      << "        ++g_failed_sets;\n"
      << "    }\n"
      << "    return g_failed_sets ? 1 : 0;\n"
      << "}\n";

    const std::string main_path = eo.out_dir + "/" + eo.stem + ".main.cpp";
    std::ofstream f(main_path, std::ios::binary);
    if (!f) {
        std::cerr << "sec: cannot write " << main_path << "\n";
        return false;
    }
    f << o.str();
    if (!opt_.quiet) std::printf("sec: %s -> %s\n", info_->fq.c_str(), eo.out_dir.c_str());
    return true;
}

}  // namespace

int generate_tests(const std::string& path, Diagnostics& diag,
                   const std::vector<std::string>& roots, const TestGenOptions& opt) {
    Source src;
    std::string error;
    if (!Source::load(path, src, error)) {
        std::cerr << "sec: " << path << ": " << error << "\n";
        return 2;
    }
    Parser parser(src, diag);
    auto file = parser.parse_model_file();
    if (diag.error_count() > 0) return 1;

    bool any = false;
    for (const ast::NodeDef& nd : file->nodes) {
        if (!nd.sec_tests.present) continue;
        any = true;
        const std::string fq = file->package.str() + "." + nd.name;
        TestGen gen(diag, roots, opt);
        gen.run(fq, src, nd.sec_tests.loc);
    }
    if (!any && !opt.quiet) std::printf("sec: %s has no tests\n", path.c_str());
    return diag.error_count() > 0 ? 1 : 0;
}

}  // namespace se
