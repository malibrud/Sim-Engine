#include "dump.hpp"

#include <ostream>
#include <string>

namespace se {

namespace {

std::string indent(int n) { return std::string(static_cast<std::size_t>(n) * 2, ' '); }

std::string trim_1line(const std::string& s, std::size_t max = 48) {
    std::string one;
    bool space = false;
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            space = true;
            continue;
        }
        if (space && !one.empty()) one.push_back(' ');
        space = false;
        one.push_back(c);
    }
    if (one.size() > max) {
        one.resize(max - 3);
        one += "...";
    }
    return one;
}

// Renders exactly as the source spells it: `name (unit): type` or `name: Type`.
void dump_type(std::ostream& o, const ast::TypeRef& t) {
    if (t.is_scalar) {
        o << " (" << unit_to_string(t.unit.get()) << "): " << t.scalar;
    } else {
        o << ": " << t.record.str();
    }
}

void dump_expr_field(std::ostream& o, const char* label, const ast::Expr* e) {
    if (!e) return;
    o << " " << label << "=" << expr_to_string(e);
}

void dump_verbatim(std::ostream& o, int d, const char* label, const Verbatim& v) {
    if (!v.present) return;
    o << indent(d) << label << " @line " << v.first_line << " (" << v.text.size()
      << " bytes): \"" << trim_1line(v.text) << "\"\n";
}

void dump_bindings(std::ostream& o, int d, const std::vector<ast::Binding>& bs) {
    for (const ast::Binding& b : bs)
        o << indent(d) << b.name << " = " << expr_to_string(b.value.get()) << "\n";
}

void dump_path_list(std::ostream& o, int d, const char* label,
                    const std::vector<ast::Path>& ps) {
    if (ps.empty()) return;
    o << indent(d) << label << "\n";
    for (const ast::Path& p : ps) o << indent(d + 1) << p.str() << "\n";
}

}  // namespace

// ─── Rendering ───────────────────────────────────────────────────────────────

std::string unit_to_string(const ast::UnitExpr* u) {
    if (!u) return "?";
    using K = ast::UnitExpr::Kind;
    // `*` and `/` are left-associative and equal precedence (§4.1), so only a
    // compound right operand needs parentheses: (1/(kg*m^2)) must not print as
    // (1/kg*m^2), which means something else entirely.
    auto rhs_str = [](const ast::UnitExpr* r) {
        const std::string s = unit_to_string(r);
        return (r && (r->kind == K::Mul || r->kind == K::Div)) ? "(" + s + ")" : s;
    };
    switch (u->kind) {
        case K::Dimensionless: return "-";
        case K::One:           return "1";
        case K::Symbol:        return u->symbol;
        case K::Mul:           return unit_to_string(u->lhs.get()) + "*" + rhs_str(u->rhs.get());
        case K::Div:           return unit_to_string(u->lhs.get()) + "/" + rhs_str(u->rhs.get());
        case K::Pow: {
            std::string s = unit_to_string(u->base.get()) + "^";
            if (u->exp_den != 1)
                s += "(" + std::to_string(u->exp_num) + "/" + std::to_string(u->exp_den) + ")";
            else
                s += std::to_string(u->exp_num);
            return s;
        }
    }
    return "?";
}

std::string expr_to_string(const ast::Expr* e) {
    if (!e) return "?";
    using K = ast::Expr::Kind;
    switch (e->kind) {
        case K::Number: {
            std::string s = e->text;
            if (e->unit) s += " (" + unit_to_string(e->unit.get()) + ")";
            return s;
        }
        case K::Param:  return "param." + e->text;
        case K::Name:   return e->text;
        case K::Unary:  return std::string(1, e->op) + expr_to_string(e->lhs.get());
        case K::Binary: return "(" + expr_to_string(e->lhs.get()) + " " +
                               std::string(1, e->op) + " " +
                               expr_to_string(e->rhs.get()) + ")";
        case K::Call: {
            std::string s = e->text + "(";
            for (std::size_t i = 0; i < e->args.size(); ++i) {
                if (i) s += ", ";
                s += expr_to_string(e->args[i].get());
            }
            return s + ")";
        }
    }
    return "?";
}

// ─── Model file ──────────────────────────────────────────────────────────────

void dump(std::ostream& o, const ast::ModelFile& f) {
    o << "model-file\n";
    if (f.has_package) o << indent(1) << "package " << f.package.str() << "\n";
    for (const ast::UseDecl& u : f.uses) o << indent(1) << "use " << u.path.str() << "\n";

    if (!f.build.empty()) {
        o << indent(1) << "build (package manifest)\n";
        for (const ast::BuildStmt& b : f.build) {
            o << indent(2) << b.primitive << " \"" << b.argument << "\"";
            if (!b.when.empty()) o << " when " << b.when;
            o << "\n";
        }
    }

    for (const ast::TypeDef& t : f.types) {
        o << indent(1) << "type " << t.name << "  [line " << t.loc.line << "]\n";
        for (const ast::FieldDecl& fd : t.fields) {
            o << indent(2) << fd.name;
            dump_type(o, fd.type);
            o << "\n";
        }
    }

    for (const ast::NodeDef& n : f.nodes) {
        o << indent(1) << "node " << n.name << "  [line " << n.loc.line << "]  kind="
          << (n.is_composite() ? "composite" : (n.has_code() ? "leaf" : "stub")) << "\n";

        if (n.sec_units.present) {
            o << indent(2) << "units\n";
            for (const ast::UnitParamDecl& u : n.unit_params)
                o << indent(3) << u.name << "\n";
        }
        if (n.sec_settings.present) {
            o << indent(2) << "settings\n";
            for (const ast::SettingDecl& s : n.settings) {
                o << indent(3) << s.name;
                dump_type(o, s.type);
                if (s.default_value)
                    o << " = " << expr_to_string(s.default_value.get());
                else
                    o << "  [required]";
                o << "\n";
            }
        }
        if (n.sec_inputs.present) {
            o << indent(2) << "inputs\n";
            for (const ast::FieldDecl& p : n.inputs) {
                o << indent(3) << p.name;
                dump_type(o, p.type);
                o << "\n";
            }
        }
        if (n.sec_outputs.present) {
            o << indent(2) << "outputs\n";
            for (const ast::FieldDecl& p : n.outputs) {
                o << indent(3) << p.name;
                dump_type(o, p.type);
                o << "\n";
            }
        }
        if (n.sec_states.present) {
            o << indent(2) << "states\n";
            for (const ast::StateDecl& s : n.states) {
                o << indent(3) << (s.is_continuous ? "continuous " : "discrete   ") << s.name;
                dump_type(o, s.type);
                dump_expr_field(o, "ic", s.initial.get());
                o << "\n";
            }
        }
        if (n.sec_vars.present) {
            o << indent(2) << "vars\n";
            for (const ast::FieldDecl& v : n.vars) {
                o << indent(3) << v.name;
                dump_type(o, v.type);
                o << "\n";
            }
        }
        if (n.sec_native.present) {
            o << indent(2) << "native\n";
            for (const ast::NativeDecl& nd : n.natives)
                o << indent(3) << nd.name << ": `" << nd.cpp_type << "`\n";
        }
        if (n.sec_build.present) {
            o << indent(2) << "build\n";
            for (const ast::BuildStmt& b : n.build) {
                o << indent(3) << b.primitive << " \"" << b.argument << "\"";
                if (!b.when.empty()) o << " when " << b.when;
                o << "\n";
            }
        }
        if (n.sec_declarations.present) dump_verbatim(o, 2, "declarations", n.declarations);

        if (n.sec_structure.present) {
            o << indent(2) << "structure\n";
            for (const ast::Instance& i : n.structure.instances) {
                o << indent(3) << "node " << i.name << " : " << i.definition.str();
                for (std::size_t k = 0; k < i.unit_args.size(); ++k)
                    o << (k ? ", " : " (") << unit_to_string(i.unit_args[k].get());
                if (!i.unit_args.empty()) o << ")";
                o << "\n";
                dump_bindings(o, 4, i.bindings);
            }
            for (const ast::Wire& w : n.structure.wires) {
                o << indent(3) << w.source.str() << " -->";
                for (std::size_t k = 0; k < w.dests.size(); ++k)
                    o << (k ? "," : "") << " " << w.dests[k].str();
                o << "\n";
            }
        }

        for (const ast::Method& m : n.methods) {
            o << indent(2) << m.name() << "(";
            for (std::size_t k = 0; k < m.params.size(); ++k)
                o << (k ? ", " : "") << m.params[k];
            o << ")";
            if (m.which == ast::Method::Which::Output) {
                o << (m.params.empty() ? "   [no feedthrough — sort root]"
                                       : "   [feedthrough]");
            }
            o << "\n";
            dump_verbatim(o, 3, "body", m.body);
        }
        for (const ast::Helper& h : n.helpers) {
            o << indent(2) << "helper `" << trim_1line(h.signature.text) << "`\n";
            dump_verbatim(o, 3, "body", h.body);
        }
    }
}

// ─── Sim file ────────────────────────────────────────────────────────────────

void dump(std::ostream& o, const ast::SimFile& f) {
    o << "sim-file " << f.name << "\n";
    for (const ast::SimEntry& e : f.entries) {
        switch (e.kind) {
            case ast::SimEntry::Kind::Root:
                o << indent(1) << "root: " << e.root.str() << "\n";
                dump_bindings(o, 2, e.bindings);
                break;
            case ast::SimEntry::Kind::Value:
                o << indent(1) << e.key << ": " << expr_to_string(e.value.get()) << "\n";
                break;
            case ast::SimEntry::Kind::Word:
                o << indent(1) << e.key << ": " << e.word << "\n";
                break;
            case ast::SimEntry::Kind::Text:
                o << indent(1) << e.key << ": \"" << e.text << "\"\n";
                break;
            case ast::SimEntry::Kind::Record:
                o << indent(1) << "record\n";
                if (e.record.has_file) o << indent(2) << "file: \"" << e.record.file << "\"\n";
                if (e.record.has_every) o << indent(2) << "every: " << e.record.every << "\n";
                dump_path_list(o, 2, "signals", e.record.signals);
                break;
            case ast::SimEntry::Kind::Log:
                o << indent(1) << "log\n";
                if (e.log.has_file) o << indent(2) << "file: \"" << e.log.file << "\"\n";
                if (e.log.has_level) o << indent(2) << "level: " << e.log.level << "\n";
                if (!e.log.levels.empty()) {
                    o << indent(2) << "levels\n";
                    for (const auto& kv : e.log.levels)
                        o << indent(3) << kv.first.str() << ": " << kv.second << "\n";
                }
                break;
        }
    }
}

// ─── Settings source ─────────────────────────────────────────────────────────

void dump(std::ostream& o, const ast::SettingsFile& f) {
    o << "settings-source\n";
    for (const ast::Override& ov : f.overrides)
        o << indent(1) << ov.path.str() << " = " << expr_to_string(ov.value.get()) << "\n";
}

}  // namespace se
