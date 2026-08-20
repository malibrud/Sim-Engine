// ─────────────────────────────────────────────────────────────────────────────
//  Stage 4 — elaboration.  SPECIFICATION.md §7, §9.2, §13, §14.
// ─────────────────────────────────────────────────────────────────────────────
#include "elaborate.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

#include "dump.hpp"
#include "parser.hpp"

namespace se {
namespace {

std::string sub(const std::string& path, const std::string& name) {
    return path.empty() ? name : path + "." + name;
}

std::string ident_of(const std::string& path) {
    std::string s = path;
    for (char& c : s)
        if (c == '.') c = '_';
    return s;
}

// A rational approximation with a small denominator, for `^` exponents (§7.2).
bool as_rational(double x, Rat& out) {
    for (long d = 1; d <= 12; ++d) {
        const double n = x * static_cast<double>(d);
        const double r = std::floor(n + 0.5);
        if (std::fabs(n - r) < 1e-9) {
            out = rat(static_cast<long>(r), d);
            return true;
        }
    }
    return false;
}

// Levenshtein, only ever run on a path that already failed to resolve.
int path_distance(const std::string& a, const std::string& b) {
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j)
            cur[j] = std::min({cur[j - 1] + 1, prev[j] + 1,
                               prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        prev = cur;
    }
    return prev[b.size()];
}

std::string join_file(const std::string& dir, const std::string& rel) {
    if (dir.empty()) return rel;
    if (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) return rel;
    if (rel.size() > 1 && rel[1] == ':') return rel;   // absolute on Windows
    return dir + "/" + rel;
}

const char* const kSolvers[] = {"euler", "rk2", "rk4"};
const char* const kLevels[] = {"trace", "debug", "info", "warn", "error"};

bool one_of(const std::string& s, const char* const* set, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (s == set[i]) return true;
    return false;
}

}  // namespace

Elaborator::Elaborator(Diagnostics& diag, Resolver& resolver)
    : diag_(diag), resolver_(resolver) {}

// ═════════════════════════════════════════════════════════════════════════════
//  The §7 expression language
// ═════════════════════════════════════════════════════════════════════════════

namespace {

// §15.5 — a double as a C++ literal that reads back bit-identical. The settings
// program is generated source, so a lossy rendering here would be a silent
// numerical difference between what `sec` computed and what the binary runs.
std::string cpp_number(double v) {
    if (std::isinf(v)) return v < 0 ? "-HUGE_VAL" : "HUGE_VAL";
    std::ostringstream os;
    os << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
    std::string s = os.str();
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

ExprCode ec_lit(std::string s) {
    ExprCode c;
    ExprTok t;
    t.kind = ExprTok::Kind::Text;
    t.text = std::move(s);
    c.push_back(std::move(t));
    return c;
}

ExprCode ec_num(double v) { return ec_lit(cpp_number(v)); }

ExprCode ec_param(std::string owner, std::string name) {
    ExprCode c;
    ExprTok t;
    t.kind = ExprTok::Kind::Param;
    t.owner = std::move(owner);
    t.text = std::move(name);
    c.push_back(std::move(t));
    return c;
}

// Whether the code can be used as an operand without parentheses. A single
// reference or a single literal can; anything already carrying an operator
// cannot, and rather than track precedence we parenthesise. The generated
// program is read by people, but it is not hand-maintained, so a few redundant
// parentheses cost less than a precedence bug would.
bool ec_atomic(const ExprCode& c) {
    if (c.size() != 1) return false;
    if (c[0].kind == ExprTok::Kind::Param) return true;
    const std::string& s = c[0].text;
    return !s.empty() && s.find_first_of("+-*/ ", 1) == std::string::npos;
}

ExprCode ec_atom(ExprCode c) {
    if (ec_atomic(c)) return c;
    ExprCode r = ec_lit("(");
    r.insert(r.end(), c.begin(), c.end());
    r.push_back(ec_lit(")")[0]);
    return r;
}

// a <op> b, each operand parenthesised only if it needs to be.
ExprCode ec_bin(const ExprCode& a, const char* op, const ExprCode& b) {
    ExprCode r = ec_atom(a);
    r.push_back(ec_lit(std::string(" ") + op + " ")[0]);
    ExprCode rb = ec_atom(b);
    r.insert(r.end(), rb.begin(), rb.end());
    return r;
}

// fn(a, b) — arguments are already delimited, so they need no parentheses.
ExprCode ec_call(const char* fn, const std::vector<ExprCode>& args) {
    ExprCode r = ec_lit(std::string(fn) + "(");
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) r.push_back(ec_lit(", ")[0]);
        r.insert(r.end(), args[i].begin(), args[i].end());
    }
    r.push_back(ec_lit(")")[0]);
    return r;
}

// The affine conversion `convert()` performs, applied to an expression instead
// of to a number (§4.2 units carry a scale and an offset). Emitted only when it
// would change the value, so an in-unit expression stays untouched — which is
// why the common case generates no conversion arithmetic at all.
ExprCode ec_convert(const ExprCode& c, const Unit& from, const Unit& to) {
    if (from.scale == to.scale && from.offset == to.offset) return c;
    ExprCode r = c;
    if (from.scale != 1.0) r = ec_bin(r, "*", ec_num(from.scale));
    if (from.offset != 0.0) r = ec_bin(r, "+", ec_num(from.offset));
    if (to.offset != 0.0) r = ec_bin(r, "-", ec_num(to.offset));
    if (to.scale != 1.0) r = ec_bin(r, "/", ec_num(to.scale));
    return r;
}

}  // namespace

bool Elaborator::eval(const ast::Expr* e, const Source& src, const Env& env, Value& out) {
    if (!e) return false;
    using K = ast::Expr::Kind;

    switch (e->kind) {
        case K::Number: {
            out.v = e->number;
            out.bare = (e->unit == nullptr);
            out.code = ec_num(e->number);
            out.constant = true;
            UnitEval ue(src, diag_);
            return ue.eval(e->unit.get(), out.unit);
        }

        case K::Param: {
            auto it = env.find(e->text);
            if (it == env.end()) {
                std::vector<Attachment> att;
                att.push_back(note("a setting expression may reference other settings "
                                   "OF THE SAME NODE, through `param.` (§6.2)"));
                if (!env.empty()) {
                    std::string have = "in scope here: ";
                    bool first = true;
                    for (const auto& kv : env) {
                        if (!first) have += ", ";
                        have += "`" + kv.first + "`";
                        first = false;
                    }
                    att.push_back(help(have));
                }
                diag_.error("SE0304", src, e->loc,
                            "no setting named `" + e->text + "` here", "unresolved",
                            std::move(att));
                return false;
            }
            // `cpp` and `constant` ride along from the environment: the binding
            // that put this name in scope already knows whether it is a literal
            // or an lvalue a configuration override can move (§6.2b).
            out = it->second;
            out.bare = false;
            return true;
        }

        case K::Name: {
            if (e->text == "pi") {
                out = Value{3.14159265358979323846, unit_one(), true};
                out.code = ec_num(out.v);
                return true;
            }
            if (e->text == "e") {
                out = Value{2.71828182845904523536, unit_one(), true};
                out.code = ec_num(out.v);
                return true;
            }
            if (e->text == "inf") {
                out = Value{HUGE_VAL, unit_one(), true};
                out.code = ec_lit("HUGE_VAL");
                return true;
            }
            // §10.4 — the node's own effective sample rate, resolved at
            // elaboration: its declared `rate` if it has one, the base step
            // otherwise. NOT runtime ambient context (§10.2) — these are
            // compile-time constants, which is why they carry none of the
            // "which fields are live when" baggage that closed `sim.*`.
            //
            // Both spellings ship because coefficient design wants a rate
            // (`K = 2 * sample_rate`) while anything expressed as a period
            // wants the reciprocal; deriving both from one number means they
            // cannot disagree.
            if (e->text == "sample_rate" || e->text == "time_step") {
                if (effective_rate_ <= 0.0) {
                    diag_.error("SE0413", src, e->loc,
                                "`" + e->text + "` has no value here",
                                "no sample rate in scope",
                                {note("it resolves to the node's `rate`, or the base "
                                      "step when the node declares none — neither is "
                                      "known outside a node's settings (§10.4)")});
                    return false;
                }
                Unit u;
                lookup_unit_symbol(e->text == "sample_rate" ? "Hz" : "s", u);
                out = Value{e->text == "sample_rate" ? effective_rate_
                                                     : 1.0 / effective_rate_,
                            u, false};
                // Both spellings come off one number, so they cannot disagree
                // after an override any more than they can before one.
                out.code = e->text == "sample_rate"
                               ? effective_rate_code_
                               : ec_bin(ec_num(1.0), "/", effective_rate_code_);
                out.constant = effective_rate_code_.size() == 1 &&
                               effective_rate_code_[0].kind == ExprTok::Kind::Text;
                return true;
            }
            // The grammar has no other way to spell a bare name here, so this
            // is where `mass = fl.mass` lands.
            diag_.error("SE0440", src, e->loc,
                        "`" + e->text + "` is not a constant or a setting of this node",
                        "unresolved name",
                        {note("flow-down goes strictly parent -> child: there are no "
                              "cross-instance references in a setting binding (§6.9.1)"),
                         help("the constants are `pi`, `e` and `inf`; other settings of "
                              "this node are reached as `param." + e->text + "`")});
            return false;
        }

        case K::Unary: {
            if (!eval(e->lhs.get(), src, env, out)) return false;
            if (e->op == '-') {
                out.v = -out.v;
                ExprCode neg = ec_lit("-");
                ExprCode a = ec_atom(out.code);
                neg.insert(neg.end(), a.begin(), a.end());
                out.code = std::move(neg);
            }
            return true;
        }

        case K::Binary: {
            Value a, b;
            if (!eval(e->lhs.get(), src, env, a)) return false;
            if (!eval(e->rhs.get(), src, env, b)) return false;

            if (e->op == '^') {
                if (!b.unit.dimensionless()) {
                    diag_.error("SE0412", src, e->rhs->loc,
                                "the exponent of `^` must be dimensionless",
                                "`(" + b.unit.str() + ")`");
                    return false;
                }
                // §6.2b — an exponent that a configuration override could move
                // would move the DIMENSION of the result with it, and dimensions
                // are shape. §7.2 already asks for a literal here; this is where
                // that requirement becomes load-bearing rather than stylistic.
                if (!b.constant) {
                    diag_.error("SE0412", src, e->rhs->loc,
                                "the exponent of `^` must not depend on a setting",
                                "this can change after the binary is built",
                                {note("`^` multiplies the exponent vector, so an "
                                      "overridable exponent would change the result's "
                                      "dimension at configuration (§6.2b)"),
                                 help("settings are values, not shape (§6.2) — write the "
                                      "exponent as a literal")});
                    return false;
                }
                const double exp = to_si(b.v, b.unit);
                Rat r;
                if (as_rational(exp, r)) {
                    // A value `v` in unit `u` is `v * u.scale` in SI, so
                    // raising it leaves the number alone and raises the unit.
                    out.unit = unit_pow(a.unit, r);
                    out.v = std::pow(a.v, exp);
                    out.code = ec_call("std::pow", {a.code, ec_num(exp)});
                } else if (a.unit.dimensionless()) {
                    out.unit = unit_one();
                    out.v = std::pow(to_si(a.v, a.unit), exp);
                    out.code = ec_call(
                        "std::pow", {ec_convert(a.code, a.unit, unit_one()), ec_num(exp)});
                } else {
                    diag_.error("SE0412", src, e->rhs->loc,
                                "a dimensioned base needs an integer or rational exponent",
                                "not a simple rational",
                                {note("`^` multiplies the exponent vector, so the result "
                                      "must still have rational dimensions (§7.2)")});
                    return false;
                }
                out.bare = a.bare && b.bare;
                out.constant = a.constant;   // b is constant, checked above
                break;
            }

            if (e->op == '*' || e->op == '/') {
                if (e->op == '/' && b.v == 0.0) {
                    diag_.error("SE0413", src, e->loc, "division by zero at elaboration",
                                "the divisor is zero",
                                {note("the expression language is total by rejection, "
                                      "not by producing NaN (§7.3)")});
                    return false;
                }
                out.v = (e->op == '*') ? a.v * b.v : a.v / b.v;
                out.unit = (e->op == '*') ? unit_mul(a.unit, b.unit)
                                          : unit_div(a.unit, b.unit);
                out.bare = a.bare && b.bare;
                out.code = ec_bin(a.code, e->op == '*' ? "*" : "/", b.code);
                out.constant = a.constant && b.constant;
                break;
            }

            // `+` and `-` require compatible dimensions and convert the right
            // operand to the left's unit. A bare literal on either side adopts
            // the other's unit, which is the same rule as at a declarative site.
            if (a.bare && !b.bare) a.unit = b.unit;
            if (b.bare && !a.bare) b.unit = a.unit;
            if (!compatible(a.unit, b.unit)) {
                diag_.error("SE0410", src, e->loc,
                            "incompatible units in `" + std::string(1, e->op) + "`",
                            "`(" + a.unit.str() + ")` and `(" + b.unit.str() + ")`",
                            {note("left is " + a.unit.dim_str()),
                             note("right is " + b.unit.dim_str())});
                return false;
            }
            const double rhs = convert(b.v, b.unit, a.unit);
            out.v = (e->op == '+') ? a.v + rhs : a.v - rhs;
            out.unit = a.unit;
            out.bare = a.bare && b.bare;
            // The conversion the line above performs on the number, performed on
            // the expression — the two operands may be in different units and
            // only the left one's is kept.
            out.code = ec_bin(a.code, e->op == '+' ? "+" : "-",
                              ec_convert(b.code, b.unit, a.unit));
            out.constant = a.constant && b.constant;
            break;
        }

        case K::Call:
            if (!eval_call(e, src, env, out)) return false;
            break;
    }

    if (!std::isfinite(out.v) && !(e->kind == K::Name && e->text == "inf")) {
        diag_.error("SE0413", src, e->loc, "elaboration produced a non-finite value",
                    "not finite",
                    {note("division by zero, a domain error, or an overflow (§7.3)")});
        return false;
    }
    return true;
}

bool Elaborator::eval_call(const ast::Expr* e, const Source& src, const Env& env,
                           Value& out) {
    const std::string& f = e->text;
    std::vector<Value> a;
    for (const ast::ExprPtr& arg : e->args) {
        Value v;
        if (!eval(arg.get(), src, env, v)) return false;
        a.push_back(v);
    }

    auto arity = [&](std::size_t n) {
        if (a.size() == n) return true;
        diag_.error("SE0412", src, e->loc,
                    "`" + f + "` takes " + std::to_string(n) + " argument" +
                        (n == 1 ? "" : "s"),
                    "given " + std::to_string(a.size()));
        return false;
    };
    auto want_dimensionless = [&](std::size_t i) {
        if (a[i].unit.dimensionless()) return true;
        diag_.error("SE0412", src, e->args[i]->loc,
                    "`" + f + "` requires a dimensionless argument",
                    "`(" + a[i].unit.str() + ")`",
                    {note("`asin`/`acos`/`atan` take a ratio and RETURN an angle; "
                          "`log*` and `exp` are dimensionless in and out (§7.3)")});
        return false;
    };
    // §4.5 made angle a base dimension, so `sin` now takes an angle rather than
    // a bare ratio. A bare value is still accepted and read as radians — `pi/4`
    // is the canonical argument and carries no unit of its own.
    auto want_angle = [&](std::size_t i) {
        if (a[i].bare || compatible(a[i].unit, unit_rad())) return true;
        diag_.error("SE0412", src, e->args[i]->loc,
                    "`" + f + "` requires an angle",
                    "`(" + a[i].unit.str() + ")`",
                    {note("the argument is " + a[i].unit.dim_str()),
                     help("angle is a base dimension (§4.5) — write `(rad)`, or a "
                          "`(deg)` literal, which converts here")});
        return false;
    };
    // Two arguments that must agree; the right is converted to the left's unit.
    auto align = [&]() {
        if (a[0].bare && !a[1].bare) a[0].unit = a[1].unit;
        if (a[1].bare && !a[0].bare) a[1].unit = a[0].unit;
        if (!compatible(a[0].unit, a[1].unit)) {
            diag_.error("SE0412", src, e->loc,
                        "`" + f + "` requires compatible arguments",
                        "`(" + a[0].unit.str() + ")` and `(" + a[1].unit.str() + ")`",
                        {note("left is " + a[0].unit.dim_str()),
                         note("right is " + a[1].unit.dim_str())});
            return false;
        }
        a[1].v = convert(a[1].v, a[1].unit, a[0].unit);
        a[1].code = ec_convert(a[1].code, a[1].unit, a[0].unit);
        a[1].unit = a[0].unit;
        return true;
    };

    out.bare = true;
    out.constant = true;
    for (const Value& v : a) {
        out.bare = out.bare && v.bare;
        out.constant = out.constant && v.constant;
    }
    out.unit = unit_one();

    // An argument that a builtin consumes in SI — `sin` wants radians whatever
    // the caller wrote, `log` wants a pure ratio. Mirrors the `to_si` the value
    // path applies a few lines below each use.
    auto si = [&](std::size_t i) { return ec_convert(a[i].code, a[i].unit, unit_one()); };
    auto code1 = [&](const char* fn) { out.code = ec_call(fn, {a[0].code}); };
    auto code1_si = [&](const char* fn) { out.code = ec_call(fn, {si(0)}); };
    auto code2 = [&](const char* fn) { out.code = ec_call(fn, {a[0].code, a[1].code}); };

    if (f == "abs" || f == "floor" || f == "ceil" || f == "round") {
        if (!arity(1)) return false;
        out.unit = a[0].unit;
        out.v = f == "abs"     ? std::fabs(a[0].v)
                : f == "floor" ? std::floor(a[0].v)
                : f == "ceil"  ? std::ceil(a[0].v)
                               : std::floor(a[0].v + 0.5);
        if (f == "abs") code1("std::fabs");
        else if (f == "floor") code1("std::floor");
        else if (f == "ceil") code1("std::ceil");
        else out.code = ec_call("std::floor", {ec_bin(a[0].code, "+", ec_num(0.5))});
        return true;
    }
    if (f == "sign") {
        if (!arity(1)) return false;
        out.v = a[0].v > 0 ? 1.0 : (a[0].v < 0 ? -1.0 : 0.0);
        out.bare = true;
        // No `<cmath>` equivalent, so it emits as the conditional it is. This is
        // generated code, not §7 source: §7.4's ban on conditionals constrains
        // what a user may write, not what the emitter may produce for it.
        out.code = ec_atom(ec_bin(ec_bin(a[0].code, ">", ec_num(0.0)), "? 1.0 :",
                                  ec_atom(ec_bin(ec_bin(a[0].code, "<", ec_num(0.0)),
                                                 "? -1.0 :", ec_num(0.0)))));
        return true;
    }
    if (f == "min" || f == "max" || f == "mod" || f == "hypot") {
        if (!arity(2) || !align()) return false;
        out.unit = a[0].unit;
        out.v = f == "min"     ? std::min(a[0].v, a[1].v)
                : f == "max"   ? std::max(a[0].v, a[1].v)
                : f == "hypot" ? std::hypot(a[0].v, a[1].v)
                               : std::fmod(a[0].v, a[1].v);
        if (f == "mod" && a[1].v == 0.0) {
            diag_.error("SE0413", src, e->loc, "`mod` by zero at elaboration", "");
            return false;
        }
        code2(f == "min"     ? "std::min"
              : f == "max"   ? "std::max"
              : f == "hypot" ? "std::hypot"
                             : "std::fmod");
        return true;
    }
    if (f == "atan2") {
        if (!arity(2) || !align()) return false;
        // The arguments' shared unit cancels in the ratio; the result is an angle.
        out.v = std::atan2(a[0].v, a[1].v);
        out.unit = unit_rad();
        out.bare = false;
        code2("std::atan2");
        return true;
    }
    if (f == "sqrt" || f == "cbrt") {
        if (!arity(1)) return false;
        if (a[0].v < 0.0) {
            diag_.error("SE0413", src, e->loc, "`" + f + "` of a negative value", "");
            return false;
        }
        const Rat r = (f == "sqrt") ? rat(1, 2) : rat(1, 3);
        out.unit = unit_pow(a[0].unit, r);
        out.v = (f == "sqrt") ? std::sqrt(a[0].v) : std::cbrt(a[0].v);
        code1(f == "sqrt" ? "std::sqrt" : "std::cbrt");
        return true;
    }
    if (f == "pow") {
        if (!arity(2) || !want_dimensionless(1)) return false;
        // Same rule as `^`: the exponent decides the result's dimension, and a
        // dimension is shape, so it may not move at configuration (§6.2b).
        if (!a[1].constant) {
            diag_.error("SE0412", src, e->args[1]->loc,
                        "the exponent of `pow` must not depend on a setting",
                        "this can change after the binary is built",
                        {note("it multiplies the exponent vector, so an overridable "
                              "exponent would change the result's dimension (§6.2b)"),
                         help("settings are values, not shape (§6.2)")});
            return false;
        }
        const double exp = to_si(a[1].v, a[1].unit);
        Rat r;
        if (as_rational(exp, r)) {
            out.unit = unit_pow(a[0].unit, r);
            out.v = std::pow(a[0].v, exp);
            out.code = ec_call("std::pow", {a[0].code, ec_num(exp)});
        } else if (a[0].unit.dimensionless()) {
            out.v = std::pow(to_si(a[0].v, a[0].unit), exp);
            out.code = ec_call("std::pow", {si(0), ec_num(exp)});
        } else {
            diag_.error("SE0412", src, e->args[1]->loc,
                        "a dimensioned base needs an integer or rational exponent",
                        "not a simple rational");
            return false;
        }
        return true;
    }
    if (f == "exp" || f == "log" || f == "log2" || f == "log10") {
        if (!arity(1) || !want_dimensionless(0)) return false;
        const double x = to_si(a[0].v, a[0].unit);
        if (f != "exp" && x <= 0.0) {
            diag_.error("SE0413", src, e->loc, "`" + f + "` of a non-positive value", "");
            return false;
        }
        out.v = f == "exp"    ? std::exp(x)
                : f == "log"  ? std::log(x)
                : f == "log2" ? std::log2(x)
                              : std::log10(x);
        out.bare = true;
        code1_si(f == "exp" ? "std::exp" : f == "log" ? "std::log"
                          : f == "log2" ? "std::log2"
                                        : "std::log10");
        return true;
    }
    // Angle in, ratio out.
    if (f == "sin" || f == "cos" || f == "tan") {
        if (!arity(1) || !want_angle(0)) return false;
        const double x = to_si(a[0].v, a[0].unit);
        out.v = f == "sin" ? std::sin(x) : f == "cos" ? std::cos(x) : std::tan(x);
        out.bare = true;
        code1_si(f == "sin" ? "std::sin" : f == "cos" ? "std::cos" : "std::tan");
        return true;
    }
    // Ratio in, angle out. `to_si` is a no-op on a dimensionless argument, and
    // `std::asin` already returns radians, so the value needs no conversion —
    // only the unit is attached.
    if (f == "asin" || f == "acos" || f == "atan") {
        if (!arity(1) || !want_dimensionless(0)) return false;
        const double x = to_si(a[0].v, a[0].unit);
        if ((f == "asin" || f == "acos") && (x < -1.0 || x > 1.0)) {
            diag_.error("SE0413", src, e->loc, "`" + f + "` argument outside [-1, 1]", "");
            return false;
        }
        out.v = f == "asin" ? std::asin(x) : f == "acos" ? std::acos(x) : std::atan(x);
        out.unit = unit_rad();
        out.bare = false;
        code1_si(f == "asin" ? "std::asin" : f == "acos" ? "std::acos" : "std::atan");
        return true;
    }

    diag_.error("SE0304", src, e->loc, "unknown builtin function `" + f + "`",
                "not a builtin",
                {note("the set is fixed and small so that elaboration is guaranteed to "
                      "terminate (§7.3, §7.4)")});
    return false;
}

bool Elaborator::coerce(const Value& v, const Type& target, const Source& src, Loc loc,
                        const char* what, double& out, ExprCode* out_code) {
    if (target.is_record) {
        diag_.error("SE0410", src, loc,
                    std::string(what) + " has a record type and cannot take a value", "");
        return false;
    }
    if (v.bare) {
        // §7.2 — a bare literal takes the unit of the site it initialises, so
        // there is nothing to convert on either the number or the expression.
        out = v.v;
        if (out_code) *out_code = v.code;
    } else {
        if (!compatible(v.unit, target.unit)) {
            diag_.error("SE0410", src, loc,
                        std::string("incompatible units for ") + what,
                        "`(" + v.unit.str() + ")` where `(" + target.unit.str() +
                            ")` is declared",
                        {note("value is " + v.unit.dim_str()),
                         note("target is " + target.unit.dim_str())});
            return false;
        }
        out = convert(v.v, v.unit, target.unit);
        if (out_code) *out_code = ec_convert(v.code, v.unit, target.unit);
    }
    if (target.scalar == "int") {
        const double r = std::floor(out + 0.5);
        if (std::fabs(out - r) > 1e-9) {
            diag_.error("SE0402", src, loc,
                        "lossy conversion to an `int` target",
                        "the value is " + std::to_string(out),
                        {note("a literal has no inherent type: it is converted to the "
                              "declared type of the site, and rejected if that is lossy "
                              "(§3.5)")});
            return false;
        }
        out = r;
        // The check above covers the elaborated value; an override reaching an
        // `int` target at configuration has not been through it, so the emitted
        // form rounds rather than letting C++ truncate toward zero. A constant
        // expression is replaced by its literal downstream and needs no rounding.
        if (out_code && !v.constant)
            *out_code = ec_call("std::floor", {ec_bin(*out_code, "+", ec_num(0.5))});
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  The sim file
// ═════════════════════════════════════════════════════════════════════════════

bool Elaborator::time_value(const ast::Expr* e, const Source& src, const char* key,
                            double& out) {
    Value v;
    if (!eval(e, src, Env{}, v)) return false;
    Unit seconds;
    lookup_unit_symbol("s", seconds);
    if (v.bare) {
        diag_.error("SE0410", src, e->loc,
                    std::string("`") + key + ":` needs a unit",
                    "no unit given",
                    {help(std::string("write `") + key + ": 1 (ms);` — there is no "
                          "implicit time unit")});
        return false;
    }
    if (!compatible(v.unit, seconds)) {
        diag_.error("SE0410", src, e->loc,
                    std::string("`") + key + ":` must be a time",
                    "`(" + v.unit.str() + ")` is " + v.unit.dim_str());
        return false;
    }
    out = to_si(v.v, v.unit);
    return true;
}

bool Elaborator::load_settings_file(const std::string& path, const Source& from, Loc loc) {
    auto src = std::make_unique<Source>();
    std::string error;
    if (!Source::load(path, *src, error)) {
        diag_.error("SE0304", from, loc, "cannot read settings source `" + path + "`",
                    error);
        return false;
    }
    Parser parser(*src, diag_);
    auto ast = parser.parse_settings_file();
    for (const ast::Override& o : ast->overrides) {
        OverrideEntry e;
        e.path = o.path.str();
        e.value = o.value.get();
        e.src = src.get();
        e.loc = o.loc;
        overrides_.push_back(e);
    }
    settings_srcs_.push_back(std::move(src));
    settings_asts_.push_back(std::move(ast));
    return true;
}

bool Elaborator::read_sim_entries(const ast::SimFile& sim, const Source& src, Model& m) {
    bool have_step = false, have_duration = false, have_root = false, have_sync = false;
    bool have_window = false;
    Loc sync_loc, window_loc;

    for (const ast::SimEntry& e : sim.entries) {
        if (e.key == "step") have_step = time_value(e.value.get(), src, "step", m.step);
        else if (e.key == "duration")
            have_duration = time_value(e.value.get(), src, "duration", m.duration);
        else if (e.key == "sync") {
            have_sync = time_value(e.value.get(), src, "sync", m.sync);
            sync_loc = e.loc;
        } else if (e.key == "window") {
            have_window = time_value(e.value.get(), src, "window", m.window);
            window_loc = e.loc;
        } else if (e.key == "root")
            have_root = true;
        else if (e.key == "solver") {
            m.solver = e.word;
            if (!one_of(e.word, kSolvers, 3))
                diag_.error("SE0264", src, e.loc, "unknown solver `" + e.word + "`",
                            "not a solver",
                            {note("the solver is global, not per-node: `euler`, `rk2` "
                                  "or `rk4` (§9.5)")});
        } else if (e.key == "mode") {
            if (e.word == "realtime") m.realtime = true;
            else if (e.word != "batch")
                diag_.error("SE0264", src, e.loc, "unknown mode `" + e.word + "`",
                            "not a mode", {note("`batch` or `realtime` (§13.1)")});
        }
    }

    if (!have_root)
        diag_.error("SE0262", src, sim.loc, "`sim` has no `root:` entry", "required here",
                    {note("the `root` entry is what makes a model runnable (§13.1)")});
    if (!have_step)
        diag_.error("SE0262", src, sim.loc, "`sim` has no `step:` entry", "required here",
                    {note("one global fixed base step; every rate is an integer "
                          "decimation of it (§9.1)")});
    if (!have_duration)
        diag_.error("SE0262", src, sim.loc, "`sim` has no `duration:` entry",
                    "required here");

    if (m.realtime && !have_sync)
        diag_.error("SE0262", src, sim.loc, "`mode: realtime` requires a `sync:` entry",
                    "required here",
                    {note("the frame is the third integer quantity alongside the base "
                          "step and the rate decimations (§9.4)")});
    if (!m.realtime && have_sync)
        diag_.error("SE0263", src, sync_loc, "`sync:` requires `mode: realtime`",
                    "no frame to synchronise to");

    // §13.6 — `window` scales real-time instrumentation and nothing else, so in
    // a batch run it would be read and then ignored. Saying so beats leaving a
    // recorded `sim.utilization` column of zeros to be puzzled over.
    if (!m.realtime && have_window)
        diag_.error("SE0263", src, window_loc, "`window:` requires `mode: realtime`",
                    "no frames to average over",
                    {note("`window` sets the rolling window for `sim.utilization` and "
                          "`sim.overrun_pct`, which only exist in real time (§13.6)"),
                     note("a batch run has no frame boundaries, so both metrics would "
                          "record as zero")});

    // §9.4 — the same error class as a non-divisor rate.
    if (have_sync && have_step && m.step > 0.0) {
        const double ratio = m.sync / m.step;
        const double r = std::floor(ratio + 0.5);
        if (std::fabs(ratio - r) > 1e-9 || r < 1.0) {
            diag_.error("SE0451", src, sync_loc,
                        "`sync` is not an integer multiple of `step`",
                        "sync/step = " + std::to_string(ratio),
                        {note("the frame free-runs a whole number of base steps, then "
                              "spins to the boundary (§9.4)")});
        } else {
            m.sync_steps = static_cast<long>(r);
        }
    }

    // §13.6 — the window is a whole number of frames, and rounding to one is
    // honest. Rounding to ZERO is not: the metrics would then report the latest
    // frame rather than a rolling average, which is a different quantity
    // wearing the same name.
    if (have_sync && have_window && m.sync > 0.0) {
        const double frames = m.window / m.sync;
        if (frames < 0.5) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "window/sync = %g, which rounds to %ld frames",
                          frames, static_cast<long>(std::floor(frames + 0.5)));
            diag_.error("SE0264", src, window_loc,
                        "`window` is shorter than one `sync` frame", buf,
                        {note("the metrics are averaged over whole frames, so a window "
                              "this short would report the latest frame, not a rolling "
                              "average (§13.6)"),
                         note("the window also sets the resolution of `sim.overrun_pct`: "
                              "N frames means steps of 100/N percent"),
                         help("`window` must be at least `sync`")});
        }
    }
    // §10.4 — the base rate every node's `sample_rate` falls back to.
    base_rate_ = m.step > 0.0 ? 1.0 / m.step : 0.0;
    return have_root && have_step && have_duration;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Instantiation
// ═════════════════════════════════════════════════════════════════════════════

// §13.3 — last writer wins. Every match is marked used, not just the winner,
// so a superseded duplicate is not then reported as having hit nothing.
OverrideEntry* Elaborator::find_override(const std::string& path) {
    OverrideEntry* found = nullptr;
    for (OverrideEntry& e : overrides_) {
        if (e.path != path) continue;
        e.used = true;
        found = &e;
    }
    return found;
}

void Elaborator::apply_overrides(const NodeInfo* def, const std::string& path,
                                 std::map<std::string, Pin>& pinned) {
    for (const auto& s : def->settings) {
        OverrideEntry* o = find_override(sub(path, s.name));
        if (!o) continue;
        Value v;
        if (!eval(o->value, *o->src, Env{}, v)) continue;
        double d = 0.0;
        if (coerce(v, s.type, *o->src, o->loc, ("setting `" + o->path + "`").c_str(), d)) {
            // §13.3 — an override detaches from the flow-down, so it emits as a
            // literal and the binding that would otherwise drive this setting is
            // simply not run. Evaluated in an empty environment, so there is
            // nothing it could depend on anyway.
            pinned[s.name] = Pin{d, ec_num(d), true};
        }
    }
}

bool Elaborator::settings_of(const NodeInfo* def,
                             const std::map<std::string, Pin>& pinned,
                             const Source& src, Loc loc, std::vector<SettingValue>& out) {
    const Source& def_src = *def->file->src;
    Env env;
    // What each setting will be assigned in the configuration program: a pinned
    // one keeps the expression that pinned it, a derived one gets its coerced
    // default below. Collected here rather than in `out` because the topological
    // walk decides the order and this map preserves it by name.
    std::map<std::string, Pin> resolved;

    // Every setting in scope reads as its own `param` member, whatever produced
    // its value: at configuration the member is what a sibling expression sees,
    // so an override of this setting reaches everything derived from it.
    for (const auto& s : def->settings) {
        auto it = pinned.find(s.name);
        if (it == pinned.end()) continue;
        resolved[s.name] = it->second;
        Value v{it->second.value, s.type.unit, false};
        v.code = ec_param(settings_path_, s.name);
        v.constant = false;
        env[s.name] = std::move(v);
    }

    // Topological evaluation of the unpinned defaults (§6.2). The graph is
    // walked depth-first; a back edge is a cycle.
    enum class Mark { White, Grey, Black };
    std::map<std::string, Mark> mark;
    for (const auto& s : def->settings)
        mark[s.name] = env.count(s.name) ? Mark::Black : Mark::White;

    std::vector<std::string> stack;
    std::function<bool(const NodeInfo::SettingInfo&)> visit =
        [&](const NodeInfo::SettingInfo& s) -> bool {
        if (mark[s.name] == Mark::Black) return true;
        if (mark[s.name] == Mark::Grey) {
            std::string cycle;
            for (const std::string& n : stack) cycle += n + " -> ";
            cycle += s.name;
            diag_.error("SE0421", def_src, s.loc,
                        "cycle among derived settings of `" + def->def->name + "`",
                        "`" + s.name + "` depends on itself",
                        {note("cycle: " + cycle),
                         note("settings are a dependency graph requiring a topological "
                              "sort, so a cycle has no value (§6.2)")});
            mark[s.name] = Mark::Black;
            return false;
        }
        mark[s.name] = Mark::Grey;
        stack.push_back(s.name);

        bool ok = true;
        if (!s.default_value) {
            // §6.2 — no default means required, and every instantiation must
            // bind it. Checked statically, here.
            diag_.error("SE0420", src, loc,
                        "required setting `" + s.name + "` is not bound",
                        "instantiated here",
                        {note("declared with no default, so every instantiation must "
                              "bind it (§6.2)",
                              def->file->path + ":" + std::to_string(s.loc.line))});
            ok = false;
        } else {
            // Resolve dependencies first.
            std::vector<const ast::Expr*> work{s.default_value};
            while (!work.empty()) {
                const ast::Expr* e = work.back();
                work.pop_back();
                if (!e) continue;
                if (e->kind == ast::Expr::Kind::Param) {
                    for (const auto& other : def->settings)
                        if (other.name == e->text && mark[other.name] != Mark::Black)
                            ok = visit(other) && ok;
                }
                work.push_back(e->lhs.get());
                work.push_back(e->rhs.get());
                for (const ast::ExprPtr& arg : e->args) work.push_back(arg.get());
            }
            Value v;
            if (ok && eval(s.default_value, def_src, env, v)) {
                // §6.2a — a setting may be declared in a unit parameter
                // (`limit (U)`), so coerce against this instance's substituted
                // unit rather than the definition's `(U)`.
                Type target = s.type;
                target.unit = bound(s.type.unit);
                double d = 0.0;
                ExprCode code;
                if (coerce(v, target, def_src, s.default_value->loc,
                           ("setting `" + s.name + "`").c_str(), d, &code)) {
                    resolved[s.name] = Pin{d, std::move(code), v.constant};
                    Value bound_v{d, target.unit, false};
                    bound_v.code = ec_param(settings_path_, s.name);
                    bound_v.constant = false;
                    env[s.name] = std::move(bound_v);
                } else {
                    ok = false;
                }
            } else {
                ok = false;
            }
        }
        stack.pop_back();
        mark[s.name] = Mark::Black;
        return ok;
    };

    bool ok = true;

    // §10.4 — `sample_rate` and `time_step` are this node's EFFECTIVE values,
    // so the reserved `rate` setting has to be resolved before any other
    // setting can read them. Seeded with the base rate first, so that a `rate`
    // default which itself mentions `sample_rate` reads the base step rather
    // than an undefined value.
    effective_rate_ = base_rate_;
    effective_rate_code_ = ec_num(base_rate_);
    for (const auto& s : def->settings) {
        if (s.name != "rate") continue;
        ok = visit(s) && ok;
        auto it = env.find("rate");
        if (it != env.end() && it->second.v > 0.0) effective_rate_ = it->second.v;
        // Only now, so that a `rate` default which itself mentions `sample_rate`
        // reads the base step rather than referring to itself.
        effective_rate_code_ = ec_param(settings_path_, "rate");
        break;
    }

    for (const auto& s : def->settings) ok = visit(s) && ok;

    slots_.clear();
    for (const auto& s : def->settings) {
        SettingValue sv;
        sv.name = s.name;
        // §15.6 promises the manifest reader that `value` is already in `unit`,
        // so this has to be the substituted unit, not the definition's `(U)`.
        sv.unit = bound(s.type.unit);
        sv.scalar = s.type.scalar;
        auto it = env.find(s.name);
        sv.value = it == env.end() ? 0.0 : it->second.v;

        // §6.2b — the same setting as one assignment in the configuration
        // program. `def->settings` is already in the order the topological walk
        // above settled, so appending here preserves it.
        SettingSlot slot;
        slot.path = sub(settings_path_, s.name);
        slot.owner = settings_path_;
        slot.name = s.name;
        slot.unit = sv.unit;
        slot.scalar = sv.scalar;
        slot.value = sv.value;
        slot.rate = (s.name == "rate");
        auto r = resolved.find(s.name);
        if (r != resolved.end()) {
            slot.expr = r->second.code;
            slot.constant = r->second.constant;
        } else {
            // A required setting nobody bound: already reported as SE0420, and
            // the program still needs a well-formed entry to stay emittable.
            slot.expr = ec_num(sv.value);
        }
        slots_.push_back(std::move(slot));

        out.push_back(std::move(sv));
    }
    return ok;
}

std::vector<Unit> Elaborator::unit_args_of(const NodeInfo* child, const ast::Instance& inst,
                                           const Source& src) {
    const std::size_t want = child->unit_params.size();
    const std::size_t got = inst.unit_args.size();
    std::vector<Unit> out;

    if (want != got) {
        std::string names;
        for (const std::string& p : child->unit_params) {
            if (!names.empty()) names += ", ";
            names += p;
        }
        std::vector<Attachment> att;
        if (want == 0)
            att.push_back(note("`" + child->def->name + "` declares no `units` section"));
        else
            att.push_back(note("`" + child->def->name + "` declares " + std::to_string(want) +
                               " unit parameter" + (want == 1 ? "" : "s") + ": " + names));
        att.push_back(help("unit arguments are positional, after the definition — "
                           "`node x : " + child->def->name + " (m/s^2) { … };`"));
        diag_.error("SE0419", src, got ? inst.unit_args_loc : inst.loc,
                    "`" + child->def->name + "` takes " + std::to_string(want) +
                        " unit argument" + (want == 1 ? "" : "s") + ", given " +
                        std::to_string(got),
                    "wrong number of unit arguments", std::move(att));
        failed_ = true;
        return out;
    }

    for (const ast::UnitPtr& u : inst.unit_args) {
        // Evaluated with NO parameter scope: a unit argument is a concrete
        // unit, so `(U)` here would have to name a real Appendix A symbol.
        UnitEval ue(src, diag_);
        Unit bound_unit;
        if (!ue.eval(u.get(), bound_unit)) {
            failed_ = true;
            bound_unit = unit_one();
        } else if (bound_unit.nonradian_angle) {
            // Same reason as `SE0415`: this unit becomes a declared unit of
            // every port that mentions the parameter.
            diag_.error("SE0415", src, inst.unit_args_loc,
                        "`(" + bound_unit.str() + ")` may not be a unit argument",
                        "declare this in radians",
                        {note("a unit argument becomes the declared unit of every port "
                              "that names the parameter (§4.5, §6.2a)")});
            failed_ = true;
        }
        out.push_back(std::move(bound_unit));
    }
    return out;
}

void Elaborator::instantiate(const NodeInfo* def, const std::string& path,
                             std::map<std::string, Pin> pinned, const Source& src,
                             Loc loc, int depth, Model& m,
                             std::vector<Unit> unit_binding) {
    if (depth > 64) {
        diag_.error("SE0312", src, loc,
                    "recursive instantiation of `" + def->fq + "`",
                    "nesting is unbounded here",
                    {note("a composite is flattened at elaboration, so a node that "
                          "transitively contains itself has no finite runtime form "
                          "(§15.2)")});
        failed_ = true;
        return;
    }
    instances_[path] = def;

    // Swapped in for the duration of this node, and restored on the way out so
    // the recursion into children does not inherit the parent's binding.
    std::vector<Unit> saved_binding;
    saved_binding.swap(unit_binding_);
    unit_binding_ = std::move(unit_binding);
    struct RestoreBinding {
        std::vector<Unit>& live;
        std::vector<Unit>& saved;
        ~RestoreBinding() { live.swap(saved); }
    } restore_binding{unit_binding_, saved_binding};

    apply_overrides(def, path, pinned);
    std::vector<SettingValue> settings;
    settings_path_ = path;
    if (!settings_of(def, pinned, src, loc, settings)) failed_ = true;

    // §6.2b — this node's slice of the configuration program, appended in walk
    // order. Parents are instantiated before their children and a node's own
    // settings are topologically sorted within it, so appending as we go yields
    // a globally valid order with no second sort.
    for (SettingSlot& slot : slots_) m.settings.push_back(std::move(slot));
    slots_.clear();

    for (const SettingValue& s : settings) known_setting_paths_.push_back(sub(path, s.name));

    // A child binding is evaluated in the parent's scope, so `param.*` here
    // reads the PARENT's members — the second of the two owners an ExprTok can
    // name, and what makes an override of a composite setting flow downward.
    Env env;
    for (const SettingValue& s : settings) {
        Value v{s.value, s.unit, false};
        v.code = ec_param(path, s.name);
        v.constant = false;
        env[s.name] = std::move(v);
    }

    if (def->composite) {
        const Source& def_src = *def->file->src;

        // §6.2a — evaluated once per composite and shared with `collect_wires`,
        // which needs them to make endpoint units concrete. Computing them in
        // both places would report every arity error twice.
        std::map<std::string, std::vector<Unit>> child_bindings;
        for (const ast::Instance& inst : def->def->structure.instances) {
            auto it = def->children.find(inst.name);
            if (it == def->children.end()) continue;
            if (it->second->unit_params.empty() && inst.unit_args.empty()) continue;
            child_bindings[inst.name] = unit_args_of(it->second, inst, def_src);
        }

        collect_wires(def, path, m, child_bindings);
        for (const ast::Instance& inst : def->def->structure.instances) {
            auto child_it = def->children.find(inst.name);
            if (child_it == def->children.end()) continue;
            const NodeInfo* child = child_it->second;

            std::map<std::string, Pin> child_pinned;
            std::set<std::string> bound;
            for (const ast::Binding& b : inst.bindings) {
                const NodeInfo::SettingInfo* target = child->setting(b.name);
                if (!target) {
                    std::vector<Attachment> att;
                    std::string have;
                    for (const auto& s : child->settings) {
                        if (!have.empty()) have += ", ";
                        have += "`" + s.name + "`";
                    }
                    if (!have.empty()) att.push_back(help("`" + child->def->name +
                                                          "` declares " + have));
                    diag_.error("SE0441", def_src, b.loc,
                                "`" + child->def->name + "` has no setting `" + b.name +
                                    "`",
                                "unknown setting", std::move(att));
                    failed_ = true;
                    continue;
                }
                if (!bound.insert(b.name).second) {
                    diag_.error("SE0441", def_src, b.loc,
                                "setting `" + b.name + "` is bound twice", "already bound");
                    failed_ = true;
                    continue;
                }
                Value v;
                if (!eval(b.value.get(), def_src, env, v)) {
                    failed_ = true;
                    continue;
                }
                // §6.2a — the child's setting may be declared in one of the
                // child's unit parameters, so the binding is checked against
                // the CHILD's substitution, not this composite's.
                Type child_target = target->type;
                auto cb = child_bindings.find(inst.name);
                if (cb != child_bindings.end())
                    child_target.unit = unit_bind(target->type.unit, cb->second);
                double d = 0.0;
                ExprCode code;
                if (coerce(v, child_target, def_src, b.loc,
                           ("setting `" + b.name + "`").c_str(), d, &code))
                    child_pinned[b.name] = Pin{d, std::move(code), v.constant};
                else
                    failed_ = true;
            }
            auto bit = child_bindings.find(inst.name);
            instantiate(child, sub(path, inst.name), child_pinned, def_src, inst.loc,
                        depth + 1, m,
                        bit == child_bindings.end() ? std::vector<Unit>{} : bit->second);
        }
        return;
    }

    // ─── A leaf ──────────────────────────────────────────────────────────────
    Leaf leaf;
    leaf.path = path;
    leaf.node = def;
    leaf.loc = loc;
    leaf.settings = std::move(settings);

    // §9.2 — the elaborated decimation. It is a DEFAULT, not the final word:
    // `rate` is an ordinary runtime setting, so the emitted model recomputes
    // this in resolve_settings() and re-runs the divisor check there. What is
    // checked here is what `sec` can see, which is the default.
    for (const SettingValue& s : leaf.settings) {
        if (s.name != "rate") continue;
        const double base_rate = m.step > 0.0 ? 1.0 / m.step : 0.0;
        const double ratio = s.value > 0.0 ? base_rate / s.value : 0.0;
        const double r = std::floor(ratio + 0.5);
        if (s.value <= 0.0 || std::fabs(ratio - r) > 1e-9 || r < 1.0) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%g", s.value);
            std::vector<Attachment> att;
            char note_buf[160];
            std::snprintf(note_buf, sizeof note_buf,
                          "%g / %g = %.4f; the nearest legal rates are %g Hz and %g Hz",
                          base_rate, s.value, ratio, base_rate / std::max(1.0, std::floor(ratio)),
                          base_rate / std::max(1.0, std::ceil(ratio)));
            att.push_back(note(note_buf));
            diag_.error("SE0450", src, loc,
                        std::string("rate ") + buf +
                            " Hz is not an integer divisor of the base rate",
                        "instantiated here", std::move(att));
            failed_ = true;
        } else {
            leaf.decimation = static_cast<long>(r);
        }
    }

    const Source& def_src = *def->file->src;
    for (const auto& st : def->states) {
        StateSlot slot;
        slot.name = st.name;
        slot.continuous = st.continuous;
        slot.unit = bound(st.type.unit);
        slot.der_unit = bound(st.der_unit);
        slot.scalar = st.type.scalar;
        if (st.initial) {
            Value v;
            if (eval(st.initial, def_src, env, v))
                coerce(v, st.type, def_src, st.initial->loc,
                       ("state `" + st.name + "`").c_str(), slot.initial);
        }
        // §13.4 — an IC override rides the SAME channel as a setting.
        known_setting_paths_.push_back(sub(path, st.name));
        if (OverrideEntry* o = find_override(sub(path, st.name))) {
            Value v;
            if (eval(o->value, *o->src, Env{}, v))
                coerce(v, st.type, *o->src, o->loc,
                       ("initial condition `" + o->path + "`").c_str(), slot.initial);
        }
        leaf.states.push_back(std::move(slot));
    }

    // §6.2a — the ports as this instance actually sees them. For a
    // non-parametric node `bound` is the identity, so this is a copy.
    for (const Field& f : def->inputs) {
        Field g = f;
        g.type = bound_type(f.type);
        leaf.ports_in.push_back(std::move(g));
    }
    for (const Field& f : def->outputs) {
        Field g = f;
        g.type = bound_type(f.type);
        leaf.ports_out.push_back(std::move(g));
    }

    leaf_by_path_[path] = m.leaves.size();
    for (const Field& f : def->outputs)
        leaf_outputs_[sub(path, f.name)] = {m.leaves.size(), f.name};
    m.leaves.push_back(std::move(leaf));
}

// ═════════════════════════════════════════════════════════════════════════════
//  Wires
// ═════════════════════════════════════════════════════════════════════════════

bool Elaborator::port_conversion(const Field& from, const Field& to, const Source& src,
                                 Loc loc, double& scale, double& offset) {
    scale = 1.0;
    offset = 0.0;
    if (from.type.is_record != to.type.is_record) {
        diag_.error("SE0410", src, loc, "a record and a scalar cannot be wired together",
                    std::string(from.type.is_record ? "record -> scalar"
                                                    : "scalar -> record"));
        return false;
    }
    if (from.type.is_record) {
        // A record travels as one value, so the two ports must be the same
        // record: per-field conversion would need a copy, and §15.3 says a
        // wire is not its own variable.
        if (from.type.record != to.type.record) {
            diag_.error("SE0410", src, loc, "record types do not match",
                        "`" + from.type.record->fq + "` -> `" + to.type.record->fq + "`",
                        {note("a wire carries the whole record and coincides with the "
                              "producer's storage (§5.3, §15.3)")});
            return false;
        }
        // §5.2 — two references to one parametric record are the same type only
        // if they bound it the same way. `Vec3(m)` into `Vec3(m/s)` is the same
        // mistake as `(m)` into `(m/s)` on a scalar port, and gets the same
        // error: unlike a scalar it cannot be converted, because a record wire
        // coincides with the producer's storage.
        for (std::size_t i = 0; i < from.type.unit_args.size(); ++i) {
            const Unit& a = from.type.unit_args[i];
            const Unit& b = to.type.unit_args[i];
            if (compatible(a, b) && a.scale == b.scale) continue;
            const std::string& pname = from.type.record->unit_params[i];
            diag_.error("SE0410", src, loc,
                        "incompatible unit arguments across a wire",
                        "`(" + a.str() + ")` -> `(" + b.str() + ")`",
                        {note("both ports are `" + from.type.record->fq +
                              "`, but bound `" + pname + "` differently"),
                         note("a record travels as one value and is not copied "
                              "(§15.3), so its fields cannot be converted per wire "
                              "the way a scalar's can")});
            return false;
        }
        return true;
    }
    if (!compatible(from.type.unit, to.type.unit)) {
        diag_.error("SE0410", src, loc, "incompatible units across a wire",
                    "`(" + from.type.unit.str() + ")` -> `(" + to.type.unit.str() + ")`",
                    {note("source is " + from.type.unit.dim_str()),
                     note("destination is " + to.type.unit.dim_str())});
        return false;
    }
    // The conversion folds to a constant and is applied where the value is
    // read into the consumer's `In` view, which is a copy anyway.
    scale = from.type.unit.scale / to.type.unit.scale;
    offset = (from.type.unit.offset - to.type.unit.offset) / to.type.unit.scale;
    return true;
}

void Elaborator::collect_wires(const NodeInfo* def, const std::string& path, Model& m,
                               const std::map<std::string, std::vector<Unit>>& child_binding) {
    (void)m;
    const Source& src = *def->file->src;

    // Resolves one endpoint to its port and its addressable path. `as_source`
    // flips the polarity of a `self` endpoint, which is the whole content of
    // §6.9.2's rule about the boundary being inverted from the inside.
    auto endpoint = [&](const ast::Endpoint& e, bool as_source, Field& out_field,
                        std::string& key) -> bool {
        const Field* field = nullptr;
        if (e.is_self) {
            if (e.segs.empty()) return false;
            const std::string& n = e.segs[0];
            field = as_source ? def->input(n) : def->output(n);
            if (!field) {
                const Field* other = as_source ? def->output(n) : def->input(n);
                if (other)
                    diag_.error("SE0443", src, e.loc,
                                std::string("`self.") + n + "` is " +
                                    (as_source ? "an output" : "an input") +
                                    " and cannot be a wire " +
                                    (as_source ? "source" : "destination"),
                                "wrong direction",
                                {note("seen from the inside, the boundary's polarity is "
                                      "inverted: `self.<input>` is a source and "
                                      "`self.<output>` is a destination (§6.9.2)")});
                else
                    diag_.error("SE0304", src, e.loc,
                                "`" + def->def->name + "` has no port `" + n + "`",
                                "unresolved port");
                return false;
            }
            // A composite may not be unit-parametric (`SE0418`), so its own
            // binding is empty and this is a copy.
            out_field = *field;
            out_field.type.unit = bound(field->type.unit);
            key = sub(path, n);
            return true;
        }
        if (e.segs.size() < 2) return false;
        const std::string& inst = e.segs[0];
        const std::string& port = e.segs[1];
        auto it = def->children.find(inst);
        if (it == def->children.end()) {
            diag_.error("SE0304", src, e.loc,
                        "`" + def->def->name + "` has no child instance `" + inst + "`",
                        "unresolved instance");
            return false;
        }
        const NodeInfo* child = it->second;
        field = as_source ? child->output(port) : child->input(port);
        if (!field) {
            const Field* other = as_source ? child->input(port) : child->output(port);
            if (other)
                diag_.error("SE0443", src, e.loc,
                            "`" + inst + "." + port + "` is " +
                                (as_source ? "an input" : "an output") +
                                " and cannot be a wire " +
                                (as_source ? "source" : "destination"),
                            "wrong direction",
                            {note("direction is always source -> destination (§6.9.2)")});
            else
                diag_.error("SE0304", src, e.loc,
                            "`" + child->def->name + "` has no port `" + port + "`",
                            "unresolved port");
            return false;
        }
        out_field = *field;
        auto bit = child_binding.find(inst);
        if (bit != child_binding.end())
            out_field.type = bind_type(field->type, bit->second);
        key = sub(sub(path, inst), port);
        return true;
    };

    for (const ast::Wire& w : def->def->structure.wires) {
        Field sf;
        std::string skey;
        if (!endpoint(w.source, /*as_source=*/true, sf, skey)) {
            failed_ = true;
            continue;
        }
        for (const ast::Endpoint& d : w.dests) {
            Field df;
            std::string dkey;
            if (!endpoint(d, /*as_source=*/false, df, dkey)) {
                failed_ = true;
                continue;
            }
            Hop hop;
            hop.source = skey;
            hop.loc = d.loc;
            hop.src = &src;
            if (!port_conversion(sf, df, src, d.loc, hop.scale, hop.offset))
                failed_ = true;

            auto existing = producers_.find(dkey);
            if (existing != producers_.end()) {
                // §6.9.2 — fan-in is what would make the single-writer wire
                // lowering unsound, so it is rejected rather than summed.
                diag_.error("SE0442", src, d.loc,
                            "two sources drive `" + dkey + "`", "second source here",
                            {note("already driven by `" + existing->second.source + "`"),
                             note("fan-out is free, fan-in is forbidden: a wire "
                                  "coincides with the producer's storage (§15.3)"),
                             help("insert an explicit `Sum` node")});
                failed_ = true;
                continue;
            }
            producers_[dkey] = hop;
        }
    }
}

void Elaborator::resolve_inputs(Model& m) {
    std::set<std::string> root_inputs;
    for (const Field& f : m.root->inputs) root_inputs.insert(f.name);

    for (std::size_t i = 0; i < m.leaves.size(); ++i) {
        Leaf& leaf = m.leaves[i];
        for (const Field& port : leaf.node->inputs) {
            const std::string key = sub(leaf.path, port.name);

            std::vector<const Hop*> chain;
            std::string k = key;
            for (std::size_t guard = 0; guard <= producers_.size(); ++guard) {
                auto it = producers_.find(k);
                if (it == producers_.end()) break;
                chain.push_back(&it->second);
                k = it->second.source;
            }

            InputSource in;
            auto out_it = leaf_outputs_.find(k);
            if (out_it != leaf_outputs_.end()) {
                in.kind = InputSource::Kind::Leaf;
                in.producer = out_it->second.first;
                in.port = out_it->second.second;
            } else if (chain.empty() ? root_inputs.count(key) != 0
                                     : root_inputs.count(k) != 0) {
                in.kind = InputSource::Kind::Boundary;
                in.boundary = "sig.in." + (chain.empty() ? key : k);
            } else {
                // §6.3 — there is no implicit zero.
                std::vector<Attachment> att;
                if (!chain.empty())
                    att.push_back(note("the wire reaches `" + k +
                                       "`, which nothing drives"));
                att.push_back(note("an unconnected input is an error: there is no "
                                   "implicit zero (§6.3)"));
                diag_.error("SE0430", *leaf.node->file->src, port.loc,
                            "input `" + key + "` is not connected", "declared here", att);
                failed_ = true;
                continue;
            }

            double s = 1.0, o = 0.0;
            for (std::size_t j = chain.size(); j-- > 0;) {
                s = chain[j]->scale * s;
                o = chain[j]->scale * o + chain[j]->offset;
            }
            in.scale = s;
            in.offset = o;
            leaf.inputs[port.name] = in;
        }
    }
}

void Elaborator::resolve_boundary(const NodeInfo* root, Model& m) {
    for (const Field& f : root->inputs) {
        // The root's inputs become the fields of `Sim::Signals::In`, so the
        // host writes `sim.sig.in.axle_torque` and the manifest addresses the
        // same thing as `in.axle_torque`.
        BoundaryIn b;
        b.ident = f.name;
        b.path = "in." + f.name;
        b.type = f.type;
        m.boundary_in.push_back(std::move(b));
    }

    // A root output is named in the manifest by the leaf that actually
    // produces it, because that is the storage the host would read.
    for (const Field& f : root->outputs) {
        std::string k = f.name;
        for (std::size_t guard = 0; guard <= producers_.size(); ++guard) {
            auto it = producers_.find(k);
            if (it == producers_.end()) break;
            k = it->second.source;
        }
        auto out_it = leaf_outputs_.find(k);
        if (out_it == leaf_outputs_.end()) continue;   // undriven; simply unread
        const Leaf& leaf = m.leaves[out_it->second.first];
        // §6.2a — the substituted port, so the manifest reports the unit the
        // host actually receives rather than the definition's `(U)`.
        const Field* port = nullptr;
        for (const Field& lf : leaf.ports_out)
            if (lf.name == out_it->second.second) { port = &lf; break; }
        if (!port) continue;

        // Records are listed field by field: the manifest is read by a human
        // wiring up a host, and `ws` alone would not tell them anything.
        std::function<void(const std::string&, const std::string&, const Type&)> expand =
            [&](const std::string& path, const std::string& expr, const Type& t) {
                if (!t.is_record) {
                    BoundaryOut b;
                    b.path = path;
                    b.expr = expr;
                    b.type = t;
                    m.boundary_out.push_back(std::move(b));
                    return;
                }
                for (const Field& sf : t.record->fields)
                    expand(path + "." + sf.name, expr + "." + sf.name, field_type(t, sf));
            };
        expand(sub(leaf.path, port->name), "sig." + leaf.ident + "." + port->name,
               port->type);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Recording and logging
// ═════════════════════════════════════════════════════════════════════════════

void Elaborator::resolve_record(const ast::RecordBlock& block, const Source& src,
                                Model& m) {
    m.record.present = true;
    m.record.file = block.has_file ? block.file : "run.csv";
    m.record.every = block.has_every ? block.every : 1;
    if (m.record.every < 1) m.record.every = 1;

    for (const ast::Path& p : block.signals) {
        const std::string path = p.str();

        // §13.6 — the engine's own metrics are addressed in the same scheme.
        if (p.segs.size() == 2 && p.segs[0] == "sim") {
            RecordedSignal s;
            s.path = path;
            Unit pct;
            lookup_unit_symbol("%", pct);
            s.unit = pct;
            if (p.segs[1] == "utilization") s.expr = "se_rt::realtime().utilization()";
            else if (p.segs[1] == "overrun_pct") s.expr = "se_rt::realtime().overrun_pct()";
            else {
                diag_.error("SE0304", src, p.loc, "unknown engine metric `" + path + "`",
                            "not a metric",
                            {note("`sim.utilization` and `sim.overrun_pct` (§13.6)")});
                failed_ = true;
                continue;
            }
            m.record.signals.push_back(std::move(s));
            continue;
        }

        // Longest instance-path prefix wins, so `whl.out.ws.slip` splits at
        // `whl` and the rest is an accessor path.
        std::size_t best = 0;
        std::string leaf_path;
        for (std::size_t n = p.segs.size(); n-- > 0;) {
            std::string candidate;
            for (std::size_t i = 0; i < n; ++i) candidate = sub(candidate, p.segs[i]);
            if (leaf_by_path_.count(candidate)) {
                leaf_path = candidate;
                best = n;
                break;
            }
        }
        if (leaf_path.empty() && !leaf_by_path_.count("")) {
            diag_.error("SE0304", src, p.loc, "no node at `" + path + "`",
                        "unresolved signal path",
                        {note("signals are dotted paths from the root (§13.2)")});
            failed_ = true;
            continue;
        }
        const Leaf& leaf = m.leaves[leaf_by_path_[leaf_path]];
        std::vector<std::string> rest(p.segs.begin() + best, p.segs.end());
        if (rest.empty()) {
            diag_.error("SE0304", src, p.loc, "`" + path + "` names a node, not a signal",
                        "not a signal");
            failed_ = true;
            continue;
        }

        std::string kind;
        if (rest[0] == "out" || rest[0] == "state" || rest[0] == "var") {
            kind = rest[0];
            rest.erase(rest.begin());
            if (rest.empty()) {
                diag_.error("SE0304", src, p.loc, "`" + path + "` names no member",
                            "incomplete path");
                failed_ = true;
                continue;
            }
        } else {
            // §13.5 — a bare name is legal when unambiguous, and loud when not.
            const bool is_state = leaf.node->state(rest[0]) != nullptr;
            const bool is_out = leaf.node->output(rest[0]) != nullptr;
            if (is_state && is_out) {
                diag_.error("SE0461", src, p.loc,
                            "`" + path + "` is ambiguous", "both a state and an output",
                            {note("`" + leaf.path + ".state." + rest[0] + "` names the state"),
                             note("`" + leaf.path + ".out." + rest[0] + "` names the output"),
                             note("a node may legitimately have both (§6.11)")});
                failed_ = true;
                continue;
            }
            kind = is_state ? "state" : (is_out ? "out" : "var");
        }

        RecordedSignal s;
        s.path = path;
        const std::string base = leaf.ident;

        if (kind == "state") {
            const NodeInfo::StateInfo* st = leaf.node->state(rest[0]);
            if (!st || rest.size() != 1) {
                diag_.error("SE0304", src, p.loc, "`" + path + "` names no state",
                            "unresolved");
                failed_ = true;
                continue;
            }
            // Read the block, not the node's view of it. A continuous state is
            // a slot in `x` — the solver's ABI — and a discrete one is a field
            // of `dis`; the node holds only proxies into both (§15.5).
            if (st->continuous) {
                std::size_t slot = 0;
                for (const StateSlot& ss : leaf.states)
                    if (ss.name == rest[0]) slot = ss.slot;
                s.expr = "x[" + std::to_string(slot) + "]";
            } else {
                s.expr = "dis." + base + "." + rest[0];
            }
            // §6.2a — the slot's unit is substituted; the node's is still `(U)`.
            s.unit = st->type.unit;
            for (const StateSlot& ss : leaf.states)
                if (ss.name == rest[0]) s.unit = ss.unit;
        } else {
            const Field* f = nullptr;
            if (kind == "out")
                for (const Field& lf : leaf.ports_out)
                    if (lf.name == rest[0]) f = &lf;
            if (!f)
                for (const Field& v : leaf.node->vars)
                    if (kind == "var" && v.name == rest[0]) f = &v;
            if (!f) {
                diag_.error("SE0304", src, p.loc,
                            "`" + path + "` names no " + kind + " of `" +
                                leaf.node->def->name + "`",
                            "unresolved");
                failed_ = true;
                continue;
            }
            // An output lives in the signal block; a var is deliberately NOT a
            // block member — §6.5 makes it private per-instance storage outside
            // the override channel — so it is still read through the node.
            s.expr = kind == "out" ? "sig." + base + "." + rest[0]
                                   : base + ".var." + rest[0];
            Type t = f->type;
            bool ok = true;
            for (std::size_t i = 1; i < rest.size(); ++i) {
                if (!t.is_record) {
                    ok = false;
                    break;
                }
                const Field* sf = nullptr;
                for (const Field& c : t.record->fields)
                    if (c.name == rest[i]) sf = &c;
                if (!sf) {
                    ok = false;
                    break;
                }
                s.expr += "." + rest[i];
                t = field_type(t, *sf);
            }
            if (!ok || t.is_record) {
                diag_.error("SE0304", src, p.loc,
                            "`" + path + "` does not name a scalar signal",
                            t.is_record ? "a record cannot be a CSV column" : "no such field");
                failed_ = true;
                continue;
            }
            s.unit = t.unit;
        }
        m.record.signals.push_back(std::move(s));
    }
}

void Elaborator::resolve_log(const ast::LogBlock& block, const Source& src, Model& m) {
    m.log.present = true;
    m.log.file = block.has_file ? block.file : "";
    if (block.has_level) {
        m.log.level = block.level;
        if (!one_of(block.level, kLevels, 5))
            diag_.error("SE0264", src, block.loc, "unknown log level `" + block.level + "`",
                        "not a level",
                        {note("`trace`, `debug`, `info`, `warn`, `error` (§13.7)")});
    }
    for (const auto& kv : block.levels) {
        if (!one_of(kv.second, kLevels, 5)) {
            diag_.error("SE0264", src, kv.first.loc,
                        "unknown log level `" + kv.second + "`", "not a level");
            continue;
        }
        m.log.levels.push_back({kv.first.str(), kv.second});
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Driver
// ═════════════════════════════════════════════════════════════════════════════

bool Elaborator::run(const ast::SimFile& sim, const Source& sim_src,
                     const std::string& sim_dir, Model& m) {
    m.name = sim.name;

    const int before = diag_.error_count();
    read_sim_entries(sim, sim_src, m);

    const ast::SimEntry* root_entry = nullptr;
    const ast::SimEntry* record_entry = nullptr;
    const ast::SimEntry* log_entry = nullptr;
    for (const ast::SimEntry& e : sim.entries) {
        if (e.key == "root") root_entry = &e;
        else if (e.key == "record") record_entry = &e;
        else if (e.key == "log") log_entry = &e;
        else if (e.key == "settings")
            load_settings_file(join_file(sim_dir, e.text), sim_src, e.loc);
    }
    if (!root_entry) return false;

    m.root = resolver_.resolve_root(root_entry->root.str(), sim_src, root_entry->loc);
    if (!m.root) return false;
    m.program = &resolver_.program();
    m.root_namespace = m.root->file->package;

    // §13.3 — the sim file's root block is the third writer; the `.settings`
    // source is the fourth, and is applied inside instantiate().
    std::map<std::string, Pin> pinned;
    std::set<std::string> bound;
    for (const ast::Binding& b : root_entry->bindings) {
        const NodeInfo::SettingInfo* target = m.root->setting(b.name);
        if (!target) {
            diag_.error("SE0441", sim_src, b.loc,
                        "`" + m.root->def->name + "` has no setting `" + b.name + "`",
                        "unknown setting");
            continue;
        }
        if (!bound.insert(b.name).second) {
            diag_.error("SE0441", sim_src, b.loc,
                        "setting `" + b.name + "` is bound twice", "already bound");
            continue;
        }
        Value v;
        if (!eval(b.value.get(), sim_src, Env{}, v)) continue;
        double d = 0.0;
        ExprCode code;
        if (coerce(v, target->type, sim_src, b.loc, ("setting `" + b.name + "`").c_str(), d,
                   &code))
            // The root block is evaluated in an empty environment — there is no
            // enclosing node whose settings it could read — so it always folds.
            pinned[b.name] = Pin{d, std::move(code), v.constant};
    }

    instantiate(m.root, "", pinned, sim_src, root_entry->loc, 0, m);
    if (m.leaves.empty()) {
        diag_.error("SE0511", sim_src, root_entry->loc,
                    "the model has no leaf nodes", "nothing to run",
                    {note("a composite leaves no runtime trace at all (§15.2)")});
        return false;
    }

    // §I5 — generated identifiers are entirely the generator's business, so
    // uniqueness is guaranteed here by construction rather than by a language
    // rule the user has to know about.
    std::map<std::string, int> used;
    for (Leaf& leaf : m.leaves) {
        // The root itself may be a leaf, in which case its model path is empty
        // — §13.2 addresses from the root without naming it.
        std::string base = leaf.path.empty() ? "root" : ident_of(leaf.path);
        int& n = used[base];
        leaf.ident = n == 0 ? base : base + "_" + std::to_string(n);
        ++n;
    }

    resolve_inputs(m);
    resolve_boundary(m.root, m);

    std::size_t slot = 0;
    for (Leaf& leaf : m.leaves)
        for (StateSlot& s : leaf.states)
            if (s.continuous) s.slot = slot++;
    m.n_states = slot;

    if (record_entry) resolve_record(record_entry->record, sim_src, m);
    if (log_entry) resolve_log(log_entry->log, sim_src, m);

    // Every override must have landed on something. A typo here is otherwise
    // completely silent, which is the worst possible failure for the layer
    // that parameter sweeps generate.
    for (const OverrideEntry& o : overrides_) {
        if (o.used) continue;
        std::vector<Attachment> att;
        auto inst = instances_.find(o.path);
        if (inst != instances_.end()) {
            att.push_back(note("`" + o.path + "` names a node, not a setting"));
        } else {
            const std::size_t dot = o.path.rfind('.');
            const std::string prefix = dot == std::string::npos ? "" : o.path.substr(0, dot);
            const std::string name = dot == std::string::npos ? o.path : o.path.substr(dot + 1);
            auto owner = instances_.find(prefix);
            if (owner != instances_.end() && owner->second->output(name))
                att.push_back(note("`" + o.path +
                                   "` names an output, and outputs are computed (§13.3)"));
        }
        // A typo here is otherwise completely silent, so spend the budget on
        // pointing at what was probably meant.
        std::string best;
        int best_d = 4;
        for (const std::string& known : known_setting_paths_) {
            const int d = path_distance(o.path, known);
            if (d < best_d) {
                best_d = d;
                best = known;
            }
        }
        if (!best.empty()) att.push_back(help("did you mean `" + best + "`?"));
        att.push_back(note("overrides address a setting or a state by a dotted path from "
                           "the root (§13.2, §13.4)"));
        diag_.error("SE0460", *o.src, o.loc, "override `" + o.path + "` matches nothing",
                    "no such setting or state", std::move(att));
        failed_ = true;
    }

    return !failed_ && diag_.error_count() == before;
}

}  // namespace se
